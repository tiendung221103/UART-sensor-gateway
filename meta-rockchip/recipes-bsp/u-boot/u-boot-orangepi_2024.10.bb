SUMMARY = "Mainline U-Boot for Orange Pi RK3399 boards"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://Licenses/README;md5=2ca5f2c35c8cc335f0a19756634782f1"

require recipes-bsp/u-boot/u-boot.inc

DEPENDS += "bc-native dtc-native bison-native flex-native python3-setuptools-native python3-pyelftools-native gnutls-native"

PV = "2024.10"
SRCREV = "573d69af36cfb10d58e189656b03a659654b0cd9"
SRC_URI = " \
    git://source.denx.de/u-boot/u-boot.git;protocol=https;branch=master \
    git://github.com/rockchip-linux/rkbin.git;protocol=https;branch=master;name=rkbin;destsuffix=rkbin \
"
SRCREV_rkbin = "3e288fe814e059dd06833495f845cab04ac20a5c"
SRCREV_FORMAT = "default_rkbin"
S = "${WORKDIR}/git"
B = "${WORKDIR}/build"

UBOOT_MACHINE = "orangepi-rk3399_defconfig"
UBOOT_BINARY = "u-boot.itb"

EXTRA_OEMAKE:append = " BL31=${WORKDIR}/rkbin/bin/rk33/rk3399_bl31_v1.36.elf"
EXTRA_OEMAKE:append = " ROCKCHIP_TPL=${WORKDIR}/rkbin/bin/rk33/rk3399_ddr_933MHz_v1.30.bin"

do_configure:prepend() {
    # giống patch board-orangepi4-rockchip-tpl.patch của Armbian
    echo "CONFIG_ROCKCHIP_EXTERNAL_TPL=y" >> ${S}/configs/${UBOOT_MACHINE}
}

PROVIDES = "virtual/bootloader"

do_deploy:append() {
    install -m 0644 ${B}/idbloader.img ${DEPLOYDIR}/idbloader.img
    install -m 0644 ${B}/u-boot.itb ${DEPLOYDIR}/u-boot.itb
}