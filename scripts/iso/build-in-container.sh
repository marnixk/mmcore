#!/bin/sh
# Build the mmcore live ISO rootfs and hybrid ISO. Runs inside an amd64 Alpine
# container (or directly on an Alpine Linux host with ISO_DIRECT=1).
#
# The root filesystem is a squashfs on the ISO. GRUB loads only the kernel and
# a small initramfs (scripts/iso/pack-initramfs.sh); that init mounts the
# squashfs with the kernel's own block driver. Files under
# scripts/iso/rootfs-overlay/ are copied into the rootfs first, which is how
# the mmcore autostart, persistence, and network services are added.
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
for pkg in cpio grub grub-efi grub-bios xorriso mtools dosfstools e2fsprogs squashfs-tools; do
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

# Firmware keep-list (#958, #964, #968). The `linux-firmware` meta pulls in
# ~100 subpackages (1118 MiB installed, most of the rootfs) covering ARM SoCs,
# server SmartNICs and embedded/DSL/USB-TV devices a desktop PC never has.
# Alpine splits linux-firmware by upstream folder, so the desktop/laptop
# keep-list is an explicit package list instead of the meta:
#   CPU:     amd-ucode (late-loadable AMD microcode; security-relevant)
#   GPU:     i915, amdgpu (+radeon), nvidia, intel (BT/audio), xe
#   WiFi/BT: brcm (+cypress, synaptics), mediatek, rtw88, rtw89, rtlwifi,
#            rtl_bt, ath10k, ath11k, ath12k, ath6k, ath9k_htc, qca
# `linux-firmware-amd-ucode` is kept (#964): microcode late-loads from
# /lib/firmware and carries security fixes; it is 104 KiB.
# `linux-firmware-ath9k_htc` is kept (#964): the common AR9271 USB Wi-Fi
# dongles, 140 KiB, sub-MiB like the other Atheros families.
# `linux-firmware-libertas` and `linux-firmware-mrvl` are a hard circular
# dependency in Alpine 3.20, so they are kept or dropped together. They are
# dropped (#964): mrvl also ships Marvell Prestera switch-ASIC and Octeon
# firmware -- server/embedded classes this keep-list exists to exclude -- so
# it cannot be taken as "Wi-Fi only", and its libertas/mwifiex Wi-Fi is
# legacy and rare on the supported x86_64 desktop/laptop hardware. Install
# `linux-firmware-mrvl` on an installed system if such a card is present.
# `linux-firmware-amd` (AMD SEV) is dropped too: virtualization firmware for
# SEV guests/hosts, not a framebuffer desktop client need.
FW_KEEP="linux-firmware-amd-ucode linux-firmware-i915 linux-firmware-amdgpu \
	linux-firmware-radeon linux-firmware-nvidia linux-firmware-intel \
	linux-firmware-xe linux-firmware-brcm linux-firmware-mediatek \
	linux-firmware-rtw88 linux-firmware-rtw89 linux-firmware-rtlwifi \
	linux-firmware-rtl_bt linux-firmware-ath10k linux-firmware-ath11k \
	linux-firmware-ath12k linux-firmware-ath6k linux-firmware-ath9k_htc \
	linux-firmware-qca"
# `linux-firmware-other` is upstream's uncategorized catch-all: it holds the
# Intel iwlwifi ucodes (which must ship) plus ~15 MiB of unrelated legacy
# TV/USB/embedded blobs. It cannot be dropped wholesale, so it is installed
# with the rest and pruned to the iwlwifi ucodes below using apk's own file
# list. It provides `linux-firmware-any` too.
FW_KEEP="${FW_KEEP} linux-firmware-other"

# `linux-lts` depends on the virtual `linux-firmware-any`. Installing the
# keep-list in the SAME apk transaction makes apk satisfy that virtual from
# these packages instead of pulling the `linux-firmware` meta. Installing them
# one-by-one afterwards instead leaves the meta in the rootfs and makes apk
# exit non-zero while purging it -- which a `|| true` then masked, the #968
# silent-no-op class. So the list is part of the base transaction, never a
# separate best-effort loop.
apk add --no-cache --quiet --root "${ROOTFS}" --initdb --arch "${ARCH}" \
	alpine-base busybox openrc util-linux \
	linux-lts sof-firmware \
	wpa_supplicant iw ifupdown-ng \
	alsa-lib alsa-utils libgcc \
	libdrm mesa mesa-gbm mesa-egl mesa-gles mesa-dri-gallium \
	eudev-libs libxkbcommon \
	e2fsprogs dosfstools blkid ca-certificates \
	parted gptfdisk util-linux-misc grub grub-efi grub-bios efibootmgr \
	${FW_KEEP}

# Prune `linux-firmware-other` (installed above) to the iwlwifi ucodes. The
# package stays installed, so it still verifies below; only its unrelated
# firmware files are removed. If the file list cannot be read the files are
# left intact: extra firmware, never missing firmware.
apk --root "${ROOTFS}" info -L linux-firmware-other 2>/dev/null |
	while IFS= read -r f; do
		case "$f" in
		*lib/firmware/iwlwifi-*) ;;
		*lib/firmware/*) rm -f "${ROOTFS}/${f#/}" ;;
		esac
	done

# #968: verify every requested firmware package really landed. A typo or an
# Alpine package rename used to be swallowed by `|| true` and ship an image
# without that firmware with no build signal (the #958 failure mode). `apk
# info -e` proves presence in the rootfs regardless of how apk's own exit code
# behaved, and a missing package fails the build.
for fw in ${FW_KEEP}; do
	apk --root "${ROOTFS}" info -e "$fw" >/dev/null 2>&1 \
		|| die "firmware package missing from the rootfs: $fw"
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

log "Packing the squashfs root and a small initramfs"
KERNEL="$(ls "${ROOTFS}"/boot/vmlinuz-* 2>/dev/null | head -n 1)"
[ -n "${KERNEL}" ] || die "no kernel found in the rootfs"
cp "${KERNEL}" "${ISOROOT}/boot/vmlinuz-lts"
kcfg="$(ls "${ROOTFS}"/boot/config-* 2>/dev/null | head -n 1)"
if [ -n "${kcfg}" ]; then
	for opt in CONFIG_SQUASHFS CONFIG_SQUASHFS_ZLIB CONFIG_OVERLAY_FS CONFIG_BLK_DEV_LOOP; do
		grep -q "^${opt}=[ym]" "${kcfg}" || die "${opt} is not enabled in ${kcfg}"
	done
fi
# 1 MiB blocks match flash drives that are fast at large sequential reads and
# slow at the small reads a firmware USB stack uses.
mksquashfs "${ROOTFS}" "${ISOROOT}/boot/rootfs.squashfs" \
	-comp gzip -b 1048576 -noappend -no-progress \
	-wildcards -e 'boot/vmlinuz-*' 'boot/System.map-*' 'boot/config-*'
sh "${REPO_ROOT}/scripts/iso/pack-initramfs.sh" \
	"${ROOTFS}" "${ISOROOT}/boot/initramfs-lts"

# Quiet boot: hide the menu for ~1 s (Shift still reveals it), show a centered
# mmcore logo on the graphical console, and keep kernel/userspace chatter off
# tty0. /dev/console is the last console= (ttyS0), so OpenRC/local.d output
# lands on serial, not the display; tty2 and serial stay root shells.
cp "${SPLASH}" "${ISOROOT}/boot/grub/splash.png"
# A marker at the ISO root identifies the live media (#976). mmcore-install
# copies only the live /boot onto the target disk, so an installed
# MMCORE-SYS never carries this file: live-init refuses to mount a disk's
# /boot/rootfs.squashfs on a live boot, and a chainloaded live grub.cfg can
# reset its root back to the stick. Keep it outside /boot so the installer's
# copy cannot bring it along.
printf 'mmcore live media %s\n' "${VERSION}" > "${ISOROOT}/mmcore-live-media"

cat > "${ISOROOT}/boot/grub/grub.cfg" <<'EOF'
set timeout=1
set default=0
set timeout_style=hidden

insmod all_video
insmod gfxterm
insmod png
insmod part_msdos
insmod part_gpt
insmod iso9660
# Pin $root to the live media by its own marker, never by the installed disk's
# MMCORE-SYS label (#976). A disk install that chainloads this file must not
# drag its own root (and thus the installed build) along.
insmod search
search --no-floppy --set=root --file /mmcore-live-media
set gfxmode=1024x768,auto
set gfxpayload=keep
terminal_output gfxterm

# Centered mmcore logo while GRUB waits. The kernel's framebuffer console
# erases it when it binds to tty1; mmcore-splash repaints the same artwork
# until mmcore modesets its own KMS surface (#902).
insmod gfxterm_background
background_image /boot/grub/splash.png

menuentry "mmcore" {
	linux /boot/vmlinuz-lts console=tty0 console=ttyS0,115200 quiet loglevel=3 vt.global_cursor_default=0 logo.nologo usb-storage.delay_use=0 modprobe.blacklist=uas mmcore.live=1
	initrd /boot/initramfs-lts
}
EOF

log "Building the hybrid ISO"
rm -f "${OUT}"
grub-mkrescue -o "${OUT}" "${ISOROOT}" >/dev/null
[ -f "${OUT}" ] || die "grub-mkrescue produced no ISO"
printf '==> Built %s (%s, version %s)\n' "${OUT}" "$(wc -c < "${OUT}") bytes" "${VERSION}"
