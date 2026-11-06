SUMMARY = "wlan0 WiFi autoconnect config (wpa_supplicant + systemd-networkd)"
LICENSE = "CLOSED"

SRC_URI = " \
    file://wpa_supplicant-nl80211-wlan0.conf \
    file://25-wlan0.network \
"

S = "${WORKDIR}"

RDEPENDS:${PN} = "wpa-supplicant"

do_install() {
    install -d ${D}${sysconfdir}/wpa_supplicant
    install -m 0600 ${WORKDIR}/wpa_supplicant-nl80211-wlan0.conf \
        ${D}${sysconfdir}/wpa_supplicant/wpa_supplicant-nl80211-wlan0.conf

    install -d ${D}${sysconfdir}/systemd/network
    install -m 0644 ${WORKDIR}/25-wlan0.network \
        ${D}${sysconfdir}/systemd/network/25-wlan0.network

    # Tương đương: systemctl enable wpa_supplicant-nl80211@wlan0.service
    install -d ${D}${sysconfdir}/systemd/system/multi-user.target.wants
    ln -sf ${systemd_system_unitdir}/wpa_supplicant-nl80211@.service \
        ${D}${sysconfdir}/systemd/system/multi-user.target.wants/wpa_supplicant-nl80211@wlan0.service
}

FILES:${PN} += " \
    ${sysconfdir}/wpa_supplicant/wpa_supplicant-nl80211-wlan0.conf \
    ${sysconfdir}/systemd/network/25-wlan0.network \
    ${sysconfdir}/systemd/system/multi-user.target.wants/wpa_supplicant-nl80211@wlan0.service \
"