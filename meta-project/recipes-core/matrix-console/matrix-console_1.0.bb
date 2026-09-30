# SPDX-License-Identifier: BSD-2-Clause
SUMMARY = "Matrix rain and Thumbs Up artwork on the local console"
LICENSE = "BSD-2-Clause"
LIC_FILES_CHKSUM = "file://LICENSE.matrix-console;md5=aad8c7afb36824a2d3e1dea9ad401339 \
                    file://LICENSE.spleen;md5=6b0faaab001f2e05d12e136feb82cf71"

SRC_URI = "file://matrix-console \
           file://matrix-render.c file://matrix-fb.h file://matrix-font.h \
           file://LICENSE.matrix-console file://LICENSE.spleen \
           file://thumbs.txt \
           file://matrix-console.service \
           file://00-matrix-console.preset \
           "

S = "${UNPACKDIR}"

inherit systemd features_check
REQUIRED_DISTRO_FEATURES = "systemd"

RDEPENDS:${PN} = "coreutils util-linux-setterm ${VIRTUAL-RUNTIME_base-utils}"
SYSTEMD_SERVICE:${PN} = "matrix-console.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    ${CC} ${CPPFLAGS} ${CFLAGS} ${S}/matrix-render.c ${LDFLAGS} -o ${B}/matrix-render
}

do_install() {
    install -d ${D}${bindir} ${D}${datadir}/matrix-console ${D}${systemd_system_unitdir}
    install -m 0755 ${UNPACKDIR}/matrix-console ${D}${bindir}/matrix-console
    sed -i -e 's|@DATADIR@|${datadir}|g' -e 's|@BINDIR@|${bindir}|g' ${D}${bindir}/matrix-console
    install -m 0755 ${B}/matrix-render ${D}${bindir}/matrix-render
    install -m 0644 ${UNPACKDIR}/LICENSE.matrix-console ${D}${datadir}/matrix-console/
    install -m 0644 ${UNPACKDIR}/LICENSE.spleen ${D}${datadir}/matrix-console/
    install -m 0644 ${UNPACKDIR}/thumbs.txt ${D}${datadir}/matrix-console/
    install -m 0644 ${UNPACKDIR}/matrix-console.service ${D}${systemd_system_unitdir}/
    sed -i 's|@BINDIR@|${bindir}|g' ${D}${systemd_system_unitdir}/matrix-console.service

    # rootfs-postcommands runs preset-all after installing all packages. Tell it
    # to skip the masked instance and enable tty2 instead of DefaultInstance=tty1.
    install -Dm 0644 ${UNPACKDIR}/00-matrix-console.preset \
        ${D}${systemd_unitdir}/system-preset/00-matrix-console.preset

    # Mask only the display VT, including logind's automatic getty activation.
    install -d ${D}${sysconfdir}/systemd/system/getty.target.wants
    ln -s /dev/null ${D}${sysconfdir}/systemd/system/getty@tty1.service
    ln -s ${systemd_system_unitdir}/getty@.service \
        ${D}${sysconfdir}/systemd/system/getty.target.wants/getty@tty2.service
}

FILES:${PN} += "${datadir}/matrix-console ${sysconfdir}/systemd/system \
                ${systemd_unitdir}/system-preset/00-matrix-console.preset"
