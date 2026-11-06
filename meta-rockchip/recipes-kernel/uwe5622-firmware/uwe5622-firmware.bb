SUMMARY = "Unisoc UWE5622 WiFi/BT firmware"
LICENSE = "CLOSED"

SRC_URI = "file://uwe5622"
S = "${WORKDIR}"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/uwe5622
    install -m 0644 ${WORKDIR}/uwe5622/*.bin ${D}${nonarch_base_libdir}/firmware/uwe5622/
    install -m 0644 ${WORKDIR}/uwe5622/*.ini ${D}${nonarch_base_libdir}/firmware/
}

FILES:${PN} = " \
    ${nonarch_base_libdir}/firmware/uwe5622/*.bin \
    ${nonarch_base_libdir}/firmware/*.ini \
"