FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " file://bsp.cfg"
KERNEL_FEATURES:append = " bsp.cfg"
SRC_URI += "file://user_2026-01-21-03-24-00.cfg \
            file://user_2026-01-24-04-16-00.cfg \
            file://drm-fixes.cfg \
            file://user_2026-01-25-14-18-00.cfg \
            file://user_2026-01-25-14-56-00.cfg \
            file://0002-add-rehsd-hdmi-to-whitelist.patch \
            "

