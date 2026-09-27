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

# apk --root reads its repositories from <root>/etc/apk/repositories, so seed
# it before installing anything.
ALPINE_VER="$(cut -d. -f1,2 /etc/alpine-release 2>/dev/null || echo 3.20)"
if [ ! -s /etc/apk/repositories ]; then
	printf '%s\n' \
		"https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}/main" \
		"https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}/community" \
		> /etc/apk/repositories
fi
mkdir -p "${ROOTFS}/etc/apk"
cp /etc/apk/repositories "${ROOTFS}/etc/apk/repositories"
# apk --root also verifies the index with keys from <root>/etc/apk/keys.
mkdir -p "${ROOTFS}/etc/apk/keys"
cp -a /etc/apk/keys/. "${ROOTFS}/etc/apk/keys/" 2>/dev/null || true

apk add --no-cache --quiet --root "${ROOTFS}" --initdb --arch "${ARCH}" \
	alpine-base busybox openrc util-linux \
	linux-lts linux-firmware \
	wpa_supplicant iw ifupdown-ng \
	alsa-lib alsa-utils libgcc \
	libdrm mesa eudev-libs libxkbcommon \
	e2fsprogs dosfstools blkid ca-certificates

# Wi-Fi firmware for common chipsets; not every package exists on every branch.
for fw in linux-firmware-iwlwifi linux-firmware-realtek linux-firmware-brcm \
	linux-firmware-rtlwifi linux-firmware-rtw88 linux-firmware-mediatek \
	linux-firmware-ath9k; do
	apk add --no-cache --quiet --root "${ROOTFS}" --initdb --arch "${ARCH}" \
		"$fw" >/dev/null 2>&1 || true
done

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
tty1::respawn:/sbin/agetty --autologin root --noclear tty1 linux
tty2::respawn:/sbin/agetty --autologin root --noclear tty2 linux
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
enable local boot
enable wpa_supplicant default

if [ -x "${REPO_ROOT}/scripts/iso/build-mmcore.sh" ]; then
	log "Building mmcore for the rootfs"
	REPO_ROOT="${REPO_ROOT}" ROOTFS="${ROOTFS}" MMB_VERSION="${VERSION}" \
		sh "${REPO_ROOT}/scripts/iso/build-mmcore.sh"
fi

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
