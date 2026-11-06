// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_bridge - tuần 3: read() chặn bằng wait queue, poll(), O_NONBLOCK,
 * sysfs thống kê, xử lý gỡ thiết bị khi còn reader đang chờ.
 *
 * ESP32 --UART--> serdev (receive_buf) --> kfifo --> read()/poll() --> userspace
 *
 * Sysfs: /sys/class/sensor_bridge/sensor_bridge/{rx_bytes,dropped,fifo_used,baud}
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/serdev.h>
#include <linux/kfifo.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/mod_devicetable.h>

#define DEV_NAME		"sensor_bridge"
#define SB_FIFO_SIZE		4096		/* phải là lũy thừa của 2 */
#define SB_DEFAULT_BAUD		115200

struct sb_dev {
	struct device dev;			/* vòng đời, node /dev, sysfs */
	struct cdev cdev;
	struct serdev_device *serdev;
	DECLARE_KFIFO(fifo, u8, SB_FIFO_SIZE);
	struct mutex read_lock;			/* nhiều reader cùng đọc */
	wait_queue_head_t wq;			/* reader/poll chờ dữ liệu */
	bool gone;				/* thiết bị đã bị gỡ */
	unsigned int baud;			/* baud thực tế */
	atomic64_t rx_bytes;
	atomic64_t dropped;
};

static dev_t sb_devt;
static struct class *sb_class;

/* ---------- serdev: phía nhận dữ liệu từ UART ---------- */

static int sb_receive_buf(struct serdev_device *serdev,
			  const unsigned char *buf, size_t count)
{
	struct sb_dev *sb = serdev_device_get_drvdata(serdev);
	unsigned int in;

	in = kfifo_in(&sb->fifo, buf, count);
	atomic64_add(in, &sb->rx_bytes);

	if (in)
		wake_up_interruptible(&sb->wq);

	if (in < count) {
		atomic64_add(count - in, &sb->dropped);
		dev_warn_ratelimited(&serdev->dev,
				     "rx fifo full, dropped %zu bytes\n",
				     count - in);
	}

	return count;	/* báo đã tiêu thụ hết; chính sách khi đầy: bỏ byte mới */
}

static const struct serdev_device_ops sb_serdev_ops = {
	.receive_buf	= sb_receive_buf,
	.write_wakeup	= serdev_device_write_wakeup,
};

/* ---------- char device: phía userspace ---------- */

static int sb_open(struct inode *inode, struct file *file)
{
	file->private_data = container_of(inode->i_cdev, struct sb_dev, cdev);
	return 0;
}

static int sb_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t sb_read(struct file *file, char __user *buf,
		       size_t count, loff_t *ppos)
{
	struct sb_dev *sb = file->private_data;
	unsigned int copied;
	int ret;

	if (!count)
		return 0;

	for (;;) {
		if (mutex_lock_interruptible(&sb->read_lock))
			return -ERESTARTSYS;
		ret = kfifo_to_user(&sb->fifo, buf, count, &copied);
		mutex_unlock(&sb->read_lock);

		if (ret)
			return ret;
		if (copied)
			return copied;

		if (READ_ONCE(sb->gone))
			return -ENODEV;
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* chờ NGOÀI mutex để reader khác không bị giữ lại */
		ret = wait_event_interruptible(sb->wq,
				!kfifo_is_empty(&sb->fifo) ||
				READ_ONCE(sb->gone));
		if (ret)
			return ret;	/* -ERESTARTSYS khi có tín hiệu */
	}
}

static __poll_t sb_poll(struct file *file, poll_table *wait)
{
	struct sb_dev *sb = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &sb->wq, wait);

	if (!kfifo_is_empty(&sb->fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(sb->gone))
		mask |= EPOLLHUP;

	return mask;
}

static const struct file_operations sb_fops = {
	.owner		= THIS_MODULE,
	.open		= sb_open,
	.release	= sb_release,
	.read		= sb_read,
	.poll		= sb_poll,
};

/* ---------- sysfs: thống kê ---------- */

static ssize_t rx_bytes_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct sb_dev *sb = container_of(dev, struct sb_dev, dev);

	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sb->rx_bytes));
}
static DEVICE_ATTR_RO(rx_bytes);

static ssize_t dropped_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct sb_dev *sb = container_of(dev, struct sb_dev, dev);

	return sysfs_emit(buf, "%lld\n", (long long)atomic64_read(&sb->dropped));
}
static DEVICE_ATTR_RO(dropped);

static ssize_t fifo_used_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct sb_dev *sb = container_of(dev, struct sb_dev, dev);

	return sysfs_emit(buf, "%u\n", kfifo_len(&sb->fifo));
}
static DEVICE_ATTR_RO(fifo_used);

static ssize_t baud_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct sb_dev *sb = container_of(dev, struct sb_dev, dev);

	return sysfs_emit(buf, "%u\n", sb->baud);
}
static DEVICE_ATTR_RO(baud);

static struct attribute *sb_attrs[] = {
	&dev_attr_rx_bytes.attr,
	&dev_attr_dropped.attr,
	&dev_attr_fifo_used.attr,
	&dev_attr_baud.attr,
	NULL,
};
ATTRIBUTE_GROUPS(sb);

/* ---------- vòng đời ---------- */

static void sb_dev_release(struct device *dev)
{
	kfree(container_of(dev, struct sb_dev, dev));
}

static int sb_probe(struct serdev_device *serdev)
{
	struct sb_dev *sb;
	u32 baud = SB_DEFAULT_BAUD;
	int ret;

	sb = kzalloc(sizeof(*sb), GFP_KERNEL);
	if (!sb)
		return -ENOMEM;

	device_initialize(&sb->dev);
	sb->dev.class = sb_class;
	sb->dev.parent = &serdev->dev;
	sb->dev.devt = sb_devt;
	sb->dev.release = sb_dev_release;
	sb->dev.groups = sb_groups;
	ret = dev_set_name(&sb->dev, DEV_NAME);
	if (ret)
		goto err_put;

	INIT_KFIFO(sb->fifo);
	mutex_init(&sb->read_lock);
	init_waitqueue_head(&sb->wq);
	sb->serdev = serdev;
	cdev_init(&sb->cdev, &sb_fops);
	sb->cdev.owner = THIS_MODULE;

	serdev_device_set_drvdata(serdev, sb);
	serdev_device_set_client_ops(serdev, &sb_serdev_ops);

	of_property_read_u32(serdev->dev.of_node, "current-speed", &baud);

	ret = serdev_device_open(serdev);
	if (ret) {
		dev_err(&serdev->dev, "serdev_device_open failed: %d\n", ret);
		goto err_put;
	}

	sb->baud = serdev_device_set_baudrate(serdev, baud);
	if (sb->baud != baud)
		dev_warn(&serdev->dev, "requested %u baud, got %u\n",
			 baud, sb->baud);
	serdev_device_set_flow_control(serdev, false);
	ret = serdev_device_set_parity(serdev, SERDEV_PARITY_NONE);
	if (ret) {
		dev_err(&serdev->dev, "set_parity failed: %d\n", ret);
		goto err_close;
	}

	ret = cdev_device_add(&sb->cdev, &sb->dev);
	if (ret) {
		dev_err(&serdev->dev, "cdev_device_add failed: %d\n", ret);
		goto err_close;
	}

	dev_info(&serdev->dev, "bound: %u baud, fifo %u bytes\n",
		 sb->baud, SB_FIFO_SIZE);
	return 0;

err_close:
	serdev_device_close(serdev);
err_put:
	put_device(&sb->dev);
	return ret;
}

static void sb_remove(struct serdev_device *serdev)
{
	struct sb_dev *sb = serdev_device_get_drvdata(serdev);

	serdev_device_close(serdev);		/* dừng nhận trước */

	WRITE_ONCE(sb->gone, true);		/* đánh thức mọi reader đang chờ */
	wake_up_interruptible_all(&sb->wq);

	cdev_device_del(&sb->cdev, &sb->dev);
	put_device(&sb->dev);
}

static const struct of_device_id sb_of_match[] = {
	{ .compatible = "gateway,sensor-bridge" },
	{ }
};
MODULE_DEVICE_TABLE(of, sb_of_match);

static struct serdev_device_driver sb_driver = {
	.probe	= sb_probe,
	.remove	= sb_remove,
	.driver	= {
		.name		= DEV_NAME,
		.of_match_table	= sb_of_match,
	},
};

static int __init sb_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&sb_devt, 0, 1, DEV_NAME);
	if (ret)
		return ret;

	sb_class = class_create(DEV_NAME);
	if (IS_ERR(sb_class)) {
		ret = PTR_ERR(sb_class);
		goto err_region;
	}

	ret = serdev_device_driver_register(&sb_driver);
	if (ret)
		goto err_class;

	return 0;

err_class:
	class_destroy(sb_class);
err_region:
	unregister_chrdev_region(sb_devt, 1);
	return ret;
}

static void __exit sb_exit(void)
{
	serdev_device_driver_unregister(&sb_driver);
	class_destroy(sb_class);
	unregister_chrdev_region(sb_devt, 1);
}

module_init(sb_init);
module_exit(sb_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("UART sensor bridge: serdev + kfifo + char device (week 3)");