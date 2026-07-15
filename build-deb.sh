#!/bin/sh
set -eu

APP=xiaomi-pen-status
VERSION=0.2.1
ARCH="$(dpkg --print-architecture)"
ROOT="$(pwd)"
PKGROOT="$(mktemp -d)"
OUT="${ROOT}/${APP}_${VERSION}_${ARCH}.deb"
chmod 755 "${PKGROOT}"

cleanup() {
	rm -rf "${PKGROOT}"
}
trap cleanup EXIT

qmake6
make

install -Dm755 "${ROOT}/${APP}" "${PKGROOT}/usr/bin/${APP}"
install -Dm644 "${ROOT}/${APP}.desktop" \
	"${PKGROOT}/usr/share/applications/${APP}.desktop"
install -d -m755 "${PKGROOT}/etc/xdg/autostart"
sed 's/^Exec=.*/Exec=xiaomi-pen-status/' "${ROOT}/${APP}.desktop" \
	> "${PKGROOT}/etc/xdg/autostart/${APP}.desktop"
chmod 644 "${PKGROOT}/etc/xdg/autostart/${APP}.desktop"
install -Dm644 "${ROOT}/${APP}.svg" \
	"${PKGROOT}/usr/share/icons/hicolor/scalable/apps/${APP}.svg"

mkdir -p "${PKGROOT}/DEBIAN"
cat > "${PKGROOT}/DEBIAN/control" <<EOF
Package: ${APP}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libqt6dbus6, libqt6network6, libqt6svg6, libqt6widgets6
Maintainer: siergtc <i@4t.pw>
Description: Stylus status tray utility
 A Qt tray utility that reports stylus placement and battery level, derives
 the detected pen MAC from qcom_battmgr, automatically pairs and connects that
 address through BlueZ.
EOF

dpkg-deb --build --root-owner-group "${PKGROOT}" "${OUT}"
printf '%s\n' "${OUT}"
