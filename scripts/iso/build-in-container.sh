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
SPLASH="${REPO_ROOT}/scripts/iso/boot/splash.png"

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
	linux-lts linux-firmware sof-firmware \
	wpa_supplicant iw ifupdown-ng \
	alsa-lib alsa-utils libgcc \
	libdrm mesa mesa-gbm mesa-egl mesa-gles mesa-dri-gallium \
	eudev-libs libxkbcommon \
	e2fsprogs dosfstools blkid ca-certificates \
	parted gptfdisk util-linux-misc grub grub-efi grub-bios

# Wi-Fi firmware for common chipsets; not every package exists on every branch.
for fw in linux-firmware-iwlwifi linux-firmware-realtek linux-firmware-brcm \
	linux-firmware-rtlwifi linux-firmware-rtw88 linux-firmware-mediatek \
	linux-firmware-ath9k; do
	apk add --no-cache --quiet --root "${ROOTFS}" --initdb --arch "${ARCH}" \
		"$fw" >/dev/null 2>&1 || true
done

# mmcore-update fetches over HTTPS (#892). `apk add --root` may not run the
# ca-certificates trigger, so guarantee a usable trust store: busybox wget
# verifies certificates against the PEM bundle (the hash symlinks are only
# needed for OpenSSL's directory lookup, not for wget).
CA_BUNDLE="${ROOTFS}/etc/ssl/certs/ca-certificates.crt"
if [ ! -s "${CA_BUNDLE}" ]; then
	log "Assembling the CA bundle in the rootfs"
	mkdir -p "$(dirname "${CA_BUNDLE}")"
	{
		find "${ROOTFS}/usr/share/ca-certificates" -name '*.crt' -type f \
			2>/dev/null
	} | while IFS= read -r cert; do cat "${cert}"; done > "${CA_BUNDLE}"
fi
[ -s "${CA_BUNDLE}" ] || die "the CA bundle is missing: ${CA_BUNDLE}"

log "Configuring the live system"
cp /etc/resolv.conf "${ROOTFS}/etc/resolv.conf" 2>/dev/null || true
: > "${ROOTFS}/etc/fstab"
printf 'mmcore\n' > "${ROOTFS}/etc/hostname"
printf '127.0.0.1\tlocalhost mmcore\n' > "${ROOTFS}/etc/hosts"
# The release version, stamped so mmcore-install can record it on the installed
# system partition and mmcore-update can compare against published builds.
printf '%s\n' "${VERSION}" > "${ROOTFS}/etc/mmcore-version"
ln -sf /sbin/init "${ROOTFS}/init"

cat > "${ROOTFS}/etc/inittab" <<'EOF'
::sysinit:/usr/local/bin/mmcore-splash-start
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

log "Adding the boot splash to the rootfs"
# The kernel's framebuffer console binds to tty1 and erases GRUB's artwork, so
# bake the same image into the rootfs and repaint it from a tiny framebuffer
# helper until mmcore modesets its own KMS surface (#902). png_to_ppm.py turns
# the committed PNG into the PPM the helper reads (no image library).
[ -f "${SPLASH}" ] || die "splash image not found: ${SPLASH}"
apk add --no-cache --quiet python3 build-base linux-headers >/dev/null 2>&1 \
	|| true
mkdir -p "${ROOTFS}/usr/share/mmcore" "${ROOTFS}/usr/local/bin"
python3 "${REPO_ROOT}/scripts/iso/boot/png_to_ppm.py" "${SPLASH}" \
	"${ROOTFS}/usr/share/mmcore/splash.ppm" \
	|| die "could not convert ${SPLASH} to PPM"
cc -O2 -Wall -Wextra -o "${ROOTFS}/usr/local/bin/mmcore-splash" \
	"${REPO_ROOT}/scripts/iso/boot/mmcore-splash.c" \
	|| die "could not build mmcore-splash"
chmod 0755 "${ROOTFS}/usr/local/bin/mmcore-splash"

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

# Quiet boot: hide the menu for ~1 s (Shift still reveals it), show a centered
# mmcore logo on the graphical console, and keep kernel/userspace chatter off
# tty0. /dev/console is the last console= (ttyS0), so OpenRC/local.d output
# lands on serial, not the display; tty2 and serial stay root shells.
cp "${SPLASH}" "${ISOROOT}/boot/grub/splash.png"

cat > "${ISOROOT}/boot/grub/grub.cfg" <<'EOF'
set timeout=1
set default=0
set timeout_style=hidden

insmod all_video
insmod gfxterm
insmod png
set gfxmode=1024x768,auto
set gfxpayload=keep
terminal_output gfxterm

# Centered mmcore logo while GRUB waits. The kernel's framebuffer console
# erases it when it binds to tty1; mmcore-splash repaints the same artwork
# until mmcore modesets its own KMS surface (#902).
insmod gfxterm_background
background_image /boot/grub/splash.png

menuentry "mmcore" {
	linux /boot/vmlinuz-lts console=tty0 console=ttyS0,115200 quiet loglevel=3 vt.global_cursor_default=0 logo.nologo
	initrd /boot/initramfs-lts
}
EOF

log "Building the hybrid ISO"
rm -f "${OUT}"
grub-mkrescue -o "${OUT}" "${ISOROOT}" >/dev/null
[ -f "${OUT}" ] || die "grub-mkrescue produced no ISO"
printf '==> Built %s (%s, version %s)\n' "${OUT}" "$(wc -c < "${OUT}") bytes" "${VERSION}"
