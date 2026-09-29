SUMMARY = "Matrix rain and Thumbs Up artwork on the local console"
LICENSE = "CLOSED"

SRC_URI = "file://matrix-console \
           file://matrix.awk \
           file://thumbs.txt \
           file://matrix-console.service \
           "

inherit allarch systemd features_check
REQUIRED_DISTRO_FEATURES = "systemd"

RDEPENDS:${PN} = "gawk coreutils util-linux-setterm ${VIRTUAL-RUNTIME_base-utils}"
SYSTEMD_SERVICE:${PN} = "matrix-console.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_install() {
    install -d ${D}${bindir} ${D}${datadir}/matrix-console ${D}${systemd_system_unitdir}
    install -m 0755 ${UNPACKDIR}/matrix-console ${D}${bindir}/matrix-console
    sed -i 's|@DATADIR@|${datadir}|g' ${D}${bindir}/matrix-console
    install -m 0644 ${UNPACKDIR}/matrix.awk ${UNPACKDIR}/thumbs.txt ${D}${datadir}/matrix-console/
    install -m 0644 ${UNPACKDIR}/matrix-console.service ${D}${systemd_system_unitdir}/
    sed -i 's|@BINDIR@|${bindir}|g' ${D}${systemd_system_unitdir}/matrix-console.service

    # Mask only the display VT, including logind's automatic getty activation.
    install -d ${D}${sysconfdir}/systemd/system/getty.target.wants
    ln -s /dev/null ${D}${sysconfdir}/systemd/system/getty@tty1.service
    ln -s ${systemd_system_unitdir}/getty@.service \
        ${D}${sysconfdir}/systemd/system/getty.target.wants/getty@tty2.service
}

FILES:${PN} += "${datadir}/matrix-console ${sysconfdir}/systemd/system"
