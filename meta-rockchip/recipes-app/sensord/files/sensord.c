// SPDX-License-Identifier: MIT
/*
 * sensord - daemon userspace của UART Sensor Gateway (giai đoạn 2)
 *
 *   /dev/sensor_bridge --poll/read--> parser frame (CRC16, resync)
 *        --> JSON --> MQTT (libmosquitto, luồng mạng riêng của thư viện)
 *
 * Vòng lặp chính chạy một luồng với poll(): chờ dữ liệu từ thiết bị, đồng
 * thời làm các việc định kỳ (watchdog, mở lại thiết bị, thống kê).
 *
 * Topic:
 *   gateway/<node>/telemetry   JSON, QoS cấu hình được (-q), không retain
 *   gateway/<node>/status      "online"/"offline", QoS 1, retain; "offline" là LWT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <mosquitto.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---------- giao thức frame (khớp với esp32_sender.ino) ---------- */

#define SOF		0xAA
#define MAX_PAYLOAD	64
#define TYPE_SENSOR	0x01
#define PARSER_BUF	256

static uint16_t crc16_ccitt(uint16_t crc, const uint8_t *d, size_t n)
{
	while (n--) {
		crc ^= (uint16_t)(*d++) << 8;
		for (int i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
					     : (uint16_t)(crc << 1);
	}
	return crc;
}

typedef void (*frame_cb)(void *arg, uint8_t type, uint8_t seq,
			 const uint8_t *payload, uint8_t len);

struct parser {
	uint8_t buf[PARSER_BUF];
	size_t len;
	int last_seq;			/* -1 khi chưa có frame nào */
	uint64_t ok, crc_err, bad_len, skipped, lost;
};

static void parser_init(struct parser *p)
{
	memset(p, 0, sizeof(*p));
	p->last_seq = -1;
}

/* Bỏ mảnh frame dở và mốc SEQ, GIỮ NGUYÊN bộ đếm thống kê */
static void parser_reset(struct parser *p)
{
	p->len = 0;
	p->last_seq = -1;
}

static void parser_drop(struct parser *p, size_t n)
{
	memmove(p->buf, p->buf + n, p->len - n);
	p->len -= n;
}

static void parser_run(struct parser *p, frame_cb cb, void *arg)
{
	for (;;) {
		size_t i = 0;

		while (i < p->len && p->buf[i] != SOF)
			i++;
		if (i) {
			p->skipped += i;
			parser_drop(p, i);
		}
		if (p->len < 2)
			return;

		uint8_t plen = p->buf[1];

		if (plen > MAX_PAYLOAD) {		/* SOF giả: resync */
			p->bad_len++;
			parser_drop(p, 1);
			continue;
		}

		size_t total = 4 + (size_t)plen + 2;

		if (p->len < total)
			return;				/* chờ thêm byte */

		uint16_t rx = p->buf[total - 2] | (p->buf[total - 1] << 8);

		if (crc16_ccitt(0xFFFF, p->buf + 1, 3 + plen) != rx) {
			p->crc_err++;			/* bỏ đúng 1 byte rồi tìm SOF tiếp */
			parser_drop(p, 1);
			continue;
		}

		uint8_t seq = p->buf[3];

		if (p->last_seq >= 0)
			p->lost += (uint8_t)(seq - p->last_seq - 1);
		p->last_seq = seq;
		p->ok++;

		cb(arg, p->buf[2], seq, p->buf + 4, plen);
		parser_drop(p, total);
	}
}

static void parser_feed(struct parser *p, const uint8_t *data, size_t n,
			frame_cb cb, void *arg)
{
	while (n) {
		size_t m = sizeof(p->buf) - p->len;

		if (m > n)
			m = n;
		memcpy(p->buf + p->len, data, m);
		p->len += m;
		data += m;
		n -= m;
		parser_run(p, cb, arg);
	}
}

/* ---------- cấu hình và ngữ cảnh ---------- */

struct config {
	const char *dev_path;
	const char *host;
	int port;
	const char *node;
	int qos;
	int keepalive;
};

struct ctx {
	struct config cfg;
	struct mosquitto *mosq;
	char topic_tel[96];
	char topic_status[96];
	atomic_int mqtt_up;
	atomic_int offline_mid;		/* mid đang chờ PUBACK khi tắt, -1 nếu không */
	atomic_int offline_acked;
	uint64_t published, publish_fail, unknown_type;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- systemd: sd_notify tự cài, không cần libsystemd ---------- */

static void sd_notify_str(const char *msg)
{
	const char *path = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	size_t n;
	int fd;

	if (!path || !*path)
		return;
	n = strlen(path);
	if (n >= sizeof(sa.sun_path))
		return;
	memcpy(sa.sun_path, path, n);
	if (sa.sun_path[0] == '@')		/* socket trừu tượng */
		sa.sun_path[0] = '\0';

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa,
	       offsetof(struct sockaddr_un, sun_path) + n);
	close(fd);
}

/* ---------- MQTT ---------- */

static void on_connect(struct mosquitto *m, void *obj, int rc)
{
	struct ctx *c = obj;

	if (rc != 0) {
		logmsg("mqtt: ket noi bi tu choi (rc=%d: %s)", rc,
		       mosquitto_connack_string(rc));
		return;
	}
	atomic_store(&c->mqtt_up, 1);
	logmsg("mqtt: da ket noi %s:%d", c->cfg.host, c->cfg.port);
	mosquitto_publish(m, NULL, c->topic_status, 6, "online", 1, true);
}

static void on_disconnect(struct mosquitto *m, void *obj, int rc)
{
	struct ctx *c = obj;

	(void)m;
	atomic_store(&c->mqtt_up, 0);
	if (rc)
		logmsg("mqtt: ket noi bi ngat hoac that bai (rc=%d), thu vien tu thu lai", rc);
}

static void on_publish(struct mosquitto *m, void *obj, int mid)
{
	struct ctx *c = obj;

	(void)m;
	if (mid == atomic_load(&c->offline_mid))
		atomic_store(&c->offline_acked, 1);
}

static void fmt_x10(char *out, size_t n, int v)
{
	snprintf(out, n, "%s%d.%d", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10);
}

static void on_frame(void *arg, uint8_t type, uint8_t seq,
		     const uint8_t *p, uint8_t len)
{
	struct ctx *c = arg;
	char t[16], h[16], json[160];
	int jl, rc;

	if (type != TYPE_SENSOR || len != 8) {
		c->unknown_type++;
		return;
	}

	/* little-endian, giải mã từng byte để không phụ thuộc căn chỉnh */
	int16_t temp = (int16_t)(p[0] | (p[1] << 8));
	uint16_t hum = (uint16_t)(p[2] | (p[3] << 8));
	uint32_t up = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
		      ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);

	fmt_x10(t, sizeof(t), temp);
	fmt_x10(h, sizeof(h), hum);
	jl = snprintf(json, sizeof(json),
		      "{\"seq\":%u,\"temp_c\":%s,\"hum_pct\":%s,\"uptime_ms\":%u}",
		      seq, t, h, (unsigned)up);

	rc = mosquitto_publish(c->mosq, NULL, c->topic_tel, jl, json,
			       c->cfg.qos, false);
	if (rc == MOSQ_ERR_SUCCESS)
		c->published++;
	else
		c->publish_fail++;	/* chính sách: dữ liệu cảm biến cũ không đáng xếp hàng */
}

/* ---------- thiết bị ---------- */

static int open_dev(const char *path)
{
	return open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* trả 0 nếu ổn (hết dữ liệu hiện có), âm nếu thiết bị đã mất */
static int drain_dev(int fd, struct parser *p, frame_cb cb, void *arg)
{
	uint8_t buf[512];

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n > 0) {
			parser_feed(p, buf, (size_t)n, cb, arg);
			continue;
		}
		if (n == 0 || errno == EAGAIN || errno == EINTR)
			return 0;
		return -errno;
	}
}

/* ---------- main ---------- */

static void usage(const char *prog)
{
	fprintf(stderr,
		"dung: %s [-d /dev/sensor_bridge] [-H host] [-p port] [-n node] [-q 0|1|2] [-k keepalive]\n",
		prog);
}

int main(int argc, char **argv)
{
	struct ctx c = { .cfg = { .dev_path = "/dev/sensor_bridge",
				  .host = "127.0.0.1", .port = 1883,
				  .node = "opi4lts", .qos = 0, .keepalive = 30 } };
	struct parser parser;
	char client_id[64];
	int opt, rc, dev_fd = -1;
	uint64_t wd_usec = 0, wd_ms = 0;
	const char *e;

	while ((opt = getopt(argc, argv, "d:H:p:n:q:k:h")) != -1) {
		switch (opt) {
		case 'd': c.cfg.dev_path = optarg; break;
		case 'H': c.cfg.host = optarg; break;
		case 'p': c.cfg.port = atoi(optarg); break;
		case 'n': c.cfg.node = optarg; break;
		case 'q': c.cfg.qos = atoi(optarg); break;
		case 'k': c.cfg.keepalive = atoi(optarg); break;
		default: usage(argv[0]); return 2;
		}
	}
	if (c.cfg.qos < 0 || c.cfg.qos > 2 || c.cfg.port <= 0) {
		usage(argv[0]);
		return 2;
	}

	snprintf(c.topic_tel, sizeof(c.topic_tel), "gateway/%s/telemetry", c.cfg.node);
	snprintf(c.topic_status, sizeof(c.topic_status), "gateway/%s/status", c.cfg.node);
	snprintf(client_id, sizeof(client_id), "sensord-%s", c.cfg.node);
	atomic_init(&c.mqtt_up, 0);
	atomic_init(&c.offline_mid, -1);
	atomic_init(&c.offline_acked, 0);

	struct sigaction sa = { .sa_handler = on_signal };

	sigemptyset(&sa.sa_mask);		/* không SA_RESTART: poll() phải trả EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	if ((e = getenv("WATCHDOG_USEC")) != NULL)
		wd_usec = strtoull(e, NULL, 10);
	wd_ms = wd_usec / 2000;		/* báo mỗi nửa chu kỳ */

	mosquitto_lib_init();
	c.mosq = mosquitto_new(client_id, true, &c);
	if (!c.mosq) {
		logmsg("khong tao duoc mosquitto client");
		return 1;
	}
	mosquitto_will_set(c.mosq, c.topic_status, 7, "offline", 1, true);
	mosquitto_reconnect_delay_set(c.mosq, 1, 30, true);
	mosquitto_connect_callback_set(c.mosq, on_connect);
	mosquitto_disconnect_callback_set(c.mosq, on_disconnect);
	mosquitto_publish_callback_set(c.mosq, on_publish);

	/*
	 * connect_async có thể thất bại ngay (ví dụ broker chưa chạy: ECONNREFUSED)
	 * và khi đó thư viện KHÔNG tự thử lại lần đầu. Việc tự kết nối lại sau khi
	 * đã từng kết nối là của luồng mạng (reconnect_delay_set); lần đầu thì
	 * vòng lặp chính tự thử lại với backoff.
	 */
	int need_connect = 0;
	unsigned conn_backoff_ms = 1000;
	uint64_t t_conn = now_ms();

	rc = mosquitto_connect_async(c.mosq, c.cfg.host, c.cfg.port, c.cfg.keepalive);
	if (rc != MOSQ_ERR_SUCCESS) {
		logmsg("mqtt: ket noi lan dau that bai: %s, se thu lai", mosquitto_strerror(rc));
		need_connect = 1;
		t_conn += conn_backoff_ms;
	}
	rc = mosquitto_loop_start(c.mosq);
	if (rc != MOSQ_ERR_SUCCESS) {
		logmsg("mqtt: loop_start loi: %s", mosquitto_strerror(rc));
		return 1;
	}

	parser_init(&parser);
	dev_fd = open_dev(c.cfg.dev_path);
	if (dev_fd < 0)
		logmsg("chua mo duoc %s: %s (se thu lai)", c.cfg.dev_path, strerror(errno));

	sd_notify_str("READY=1");
	logmsg("sensord san sang: dev=%s broker=%s:%d node=%s qos=%d",
	       c.cfg.dev_path, c.cfg.host, c.cfg.port, c.cfg.node, c.cfg.qos);

	uint64_t t_wd = now_ms(), t_stats = t_wd, t_open = t_wd;

	while (!g_stop) {
		struct pollfd pfd = { .fd = dev_fd, .events = POLLIN };	/* fd âm: poll chỉ ngủ */
		int timeout = 1000;

		if (wd_ms && (int)wd_ms < timeout)
			timeout = (int)wd_ms;

		int r = poll(&pfd, 1, timeout);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			logmsg("poll loi: %s", strerror(errno));
			break;
		}

		if (r > 0 && dev_fd >= 0) {
			int lost = 0;

			if (pfd.revents & POLLIN)
				lost = drain_dev(dev_fd, &parser, on_frame, &c) < 0;
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
				lost = 1;
			if (lost) {
				logmsg("thiet bi %s da mat, se thu mo lai", c.cfg.dev_path);
				close(dev_fd);
				dev_fd = -1;
				parser_reset(&parser);	/* bỏ mảnh dở, giữ bộ đếm */
				t_open = now_ms();
			}
		}

		uint64_t now = now_ms();

		if (dev_fd < 0 && now - t_open >= 1000) {
			t_open = now;
			dev_fd = open_dev(c.cfg.dev_path);
			if (dev_fd >= 0)
				logmsg("da mo lai %s", c.cfg.dev_path);
		}

		if (need_connect && now >= t_conn) {
			rc = mosquitto_reconnect_async(c.mosq);
			if (rc == MOSQ_ERR_SUCCESS) {
				need_connect = 0;
			} else {
				if (conn_backoff_ms < 30000)
					conn_backoff_ms *= 2;
				t_conn = now + conn_backoff_ms;
				logmsg("mqtt: thu ket noi that bai (%s), thu lai sau %u ms",
				       mosquitto_strerror(rc), conn_backoff_ms);
			}
		}

		if (wd_ms && now - t_wd >= wd_ms) {
			t_wd = now;
			sd_notify_str("WATCHDOG=1");
		}

		if (now - t_stats >= 10000) {
			char st[256];

			t_stats = now;
			snprintf(st, sizeof(st),
				 "frames ok=%llu crc_err=%llu bad_len=%llu skipped=%llu lost_seq=%llu "
				 "unknown=%llu published=%llu pub_fail=%llu mqtt=%s dev=%s",
				 (unsigned long long)parser.ok, (unsigned long long)parser.crc_err,
				 (unsigned long long)parser.bad_len, (unsigned long long)parser.skipped,
				 (unsigned long long)parser.lost, (unsigned long long)c.unknown_type,
				 (unsigned long long)c.published, (unsigned long long)c.publish_fail,
				 atomic_load(&c.mqtt_up) ? "up" : "down",
				 dev_fd >= 0 ? "open" : "closed");
			logmsg("%s", st);

			char sd[300];

			snprintf(sd, sizeof(sd), "STATUS=%s", st);
			sd_notify_str(sd);
		}
	}

	/* tắt êm: báo offline có chủ đích (LWT chỉ chạy khi mất kết nối ngoài ý muốn) */
	sd_notify_str("STOPPING=1");
	logmsg("dang tat...");
	if (atomic_load(&c.mqtt_up)) {
		int mid = 0;

		atomic_store(&c.offline_mid, -2);
		if (mosquitto_publish(c.mosq, &mid, c.topic_status, 7, "offline", 1, true) ==
		    MOSQ_ERR_SUCCESS) {
			atomic_store(&c.offline_mid, mid);
			for (int i = 0; i < 100 && !atomic_load(&c.offline_acked); i++)
				usleep(10000);
		}
	}
	mosquitto_disconnect(c.mosq);
	mosquitto_loop_stop(c.mosq, false);
	mosquitto_destroy(c.mosq);
	mosquitto_lib_cleanup();
	if (dev_fd >= 0)
		close(dev_fd);
	return 0;
}