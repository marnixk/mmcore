#!/bin/sh
# Build the mmcore live ISO rootfs and hybrid ISO. Runs inside an amd64 Alpine
# container (or directly on an Alpine Linux host with ISO_DIRECT=1).
#
# Everything lives in the initramfs: the kernel unpacks it as the root
# filesystem, so there is no squashfs/live-boot layer and no loop devices.
# Files under scripts/iso/rootfs-overlay/ are copied in afterwards, which is
# how the mmcore autostart, persistence, and network services are added.
set -eu

REPO_ROOT="${REPO_ROOT:-/repo}"
VERSION="${MMCORE_VERSION:-dev}"
ARCH="${ISO_ARCH:-x86_64}"
OUT="${ISO_OUT:-/repo/dist/mmcore-fb-${ARCH}.iso}"
ROOTFS="/tmp/mmcore-rootfs"
ISOROOT="/tmp/mmcore-isoroot"
OVERLAY="${REPO_ROOT}/scripts/iso/rootfs-overlay"

log() { printf '\n==> %s\n' "$*"; }
die() {
	printf 'iso-build: %s\n' "$*" >&2
	exit 1
}

log "Installing builder tools"
# grub-bios is optional (present on x86_64); grub-mkrescue needs the rest.
for pkg in cpio grub grub-efi grub-bios xorriso mtools dosfstools e2fsprogs; do
	apk add --no-cache --quiet "$pkg" >/dev/null 2>&1 || true
done
command -v grub-mkrescue >/dev/null 2>&1 || die "grub-mkrescue is unavailable"
command -v xorriso >/dev/null 2>&1 || die "xorriso is unavailable"

log "Building the Alpine rootfs (${ARCH})"
rm -rf "${ROOTFS}" "${ISOROOT}"
mkdir -p "${ROOTFS}" "${ISOROOT}/boot/grub" "$(dirname "${OUT}")"

apk add --no-cache --quiet --root "${ROOTFS}" --initdb --arch "${ARCH}" \
	alpine-base busybox openrc util-linux \
	linux-lts linux-firmware \
	wpa_supplicant iw ifupdown-ng \
	e2fsprogs dosfstools blkid ca-certificates

log "Configuring the live system"
cp /etc/resolv.conf "${ROOTFS}/etc/resolv.conf" 2>/dev/null || true
: > "${ROOTFS}/etc/fstab"
printf 'mmcore\n' > "${ROOTFS}/etc/hostname"
printf '127.0.0.1\tlocalhost mmcore\n' > "${ROOTFS}/etc/hosts"
ln -sf /sbin/init "${ROOTFS}/init"

cat > "${ROOTFS}/etc/inittab" <<'EOF'
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
tty1::respawn:/sbin/getty 38400 tty1
ttyS0::respawn:/sbin/agetty --autologin root --noclear ttyS0 115200 vt100
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
EOF

enable() {
	chroot "${ROOTFS}" /sbin/rc-update add "$1" "$2" >/dev/null 2>&1 || true
}

# Bring up the base system. The live rootfs is not laid down by setup-alpine,
# so the runlevels are seeded explicitly rather than assumed.
enable devfs sysinit
enable dmesg sysinit
enable mdev sysinit
enable hwdrivers sysinit
enable modules boot
enable sysctl boot
enable hostname boot
enable bootmisc boot
enable syslog boot
enable networking boot

if [ -d "${OVERLAY}" ]; then
	log "Applying rootfs overlay"
	cp -a "${OVERLAY}/." "${ROOTFS}/"
	chroot "${ROOTFS}" /bin/sh -c 'chmod +x /etc/local.d/*.start 2>/dev/null || true'
fi

log "Packing kernel + initramfs"
KERNEL="$(ls "${ROOTFS}"/boot/vmlinuz-* 2>/dev/null | head -n 1)"
[ -n "${KERNEL}" ] || die "no kernel found in the rootfs"
cp "${KERNEL}" "${ISOROOT}/boot/vmlinuz-lts"
( cd "${ROOTFS}" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 ) \
	> "${ISOROOT}/boot/initramfs-lts"

cat > "${ISOROOT}/boot/grub/grub.cfg" <<'EOF'
set timeout=5
set default=0
menuentry "mmcore" {
	linux /boot/vmlinuz-lts console=tty0 console=ttyS0,115200
	initrd /boot/initramfs-lts
}
EOF

log "Building the hybrid ISO"
rm -f "${OUT}"
grub-mkrescue -o "${OUT}" "${ISOROOT}" >/dev/null
[ -f "${OUT}" ] || die "grub-mkrescue produced no ISO"
printf '==> Built %s (%s, version %s)\n' "${OUT}" "$(wc -c < "${OUT}") bytes" "${VERSION}"
