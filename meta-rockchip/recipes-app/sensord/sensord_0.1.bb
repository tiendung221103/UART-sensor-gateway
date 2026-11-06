SUMMARY = "UART sensor gateway daemon: /dev/sensor_bridge to MQTT"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "mosquitto"

SRC_URI = " \
    file://sensord.c \
    file://sensord.service \
    file://sensord.default \
    file://99-sensor-bridge.rules \
"

S = "${WORKDIR}"

inherit systemd useradd

SYSTEMD_SERVICE:${PN} = "sensord.service"
SYSTEMD_AUTO_ENABLE = "enable"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "-r sensord"
USERADD_PARAM:${PN} = "-r -M -s /sbin/nologin -g sensord sensord"

CONFFILES:${PN} = "${sysconfdir}/default/sensord"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} sensord.c -o sensord -lmosquitto
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 sensord ${D}${bindir}/sensord

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/sensord.service ${D}${systemd_system_unitdir}/sensord.service

    install -d ${D}${sysconfdir}/default
    install -m 0644 ${WORKDIR}/sensord.default ${D}${sysconfdir}/default/sensord

    install -d ${D}${sysconfdir}/udev/rules.d
    install -m 0644 ${WORKDIR}/99-sensor-bridge.rules ${D}${sysconfdir}/udev/rules.d/99-sensor-bridge.rules
}
