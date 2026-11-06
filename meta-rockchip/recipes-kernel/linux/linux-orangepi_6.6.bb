# Kernel recipe cho Orange Pi RK3399 series, dùng vendor tree Xunlong

require recipes-kernel/linux/linux-yocto.inc
require linux-rockchip.inc

inherit local-git

SRCREV = "122b41d84d018af909a766e48f3f90cbea9868e0"
SRC_URI = "git://github.com/orangepi-xunlong/linux-orangepi.git;protocol=https;nobranch=1;branch=orange-pi-6.6-rk35xx; \
           file://0001-arm64-dts-rockchip-orangepi-4-lts-enable-uart4.patch \
           file://0001-arm64-dts-rockchip-orangepi-4-lts-add-sensor-bridge-.patch \
           "

LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

KERNEL_VERSION_SANITY_SKIP = "1"
LINUX_VERSION ?= "6.6"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI:append = " \
    ${@bb.utils.contains('IMAGE_FSTYPES', 'ext4', 'file://ext4.cfg', '', d)} \
    file://motorcomm.cfg \
    file://wifi.cfg \
    file://uwe5622-fix.patch \
    file://uart4.cfg \
    file://localversion.cfg \
    file://ftrace.cfg \
"

KERNEL_MODULE_AUTOLOAD += "uwe5622_bsp_sdio sprdwl_ng"