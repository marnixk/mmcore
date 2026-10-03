#!/usr/bin/env bash
# Write the mmcore live ISO to a USB device and add a persistent MMCORE
# partition, so mmcore's C:/ drive survives reboots.
#
# The ISO is written from sector 0 (BIOS El Torito + UEFI), then a second GPT
# partition is carved out of the free space and labelled MMCORE. The live
# system mounts it at /media/mmcore on boot (see the ISO's mmcore-persist
# script).
#
# Linux only. The image may be a raw .iso or a compressed .iso.zst/.xz/.gz;
# compressed input is decompressed on the fly (releases ship .iso.zst). Usage:
#   sudo ./install-usb.sh --iso dist/mmcore-fb-x86_64.iso /dev/sdX
#   sudo ./install-usb.sh --iso mmcore-fb-x86_64.iso.zst /dev/sdX
#   sudo ./install-usb.sh --read-write --iso mmcore-fb-x86_64.iso /dev/mmcblk0
#   sudo ./install-usb.sh --no-persist /dev/sdX
#
# --read-write does not install onto an internal disk. It seeds the MMCORE
# partition with a marker (/.mmcore-rw) so the live boot uses that partition
# as its writable upper layer and as C:, which is what a microSD live boot
# needs when the ISO itself is read-only.
set -euo pipefail

ISO=""
DEVICE=""
PERSIST=1
READ_WRITE=0
ASSUME_YES=0
DECOMPRESS=(cat)

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'install-usb: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage: install-usb.sh [--iso PATH] [--read-write] [--no-persist] [-y|--yes] /dev/sdX

Write the mmcore live ISO to a USB stick or microSD.

Options:
  --iso PATH     image to write: raw .iso or compressed .iso.zst/.xz/.gz
                 (default: dist/mmcore-fb-x86_64.iso)
  --read-write   provision the free space as a read-write store (label
                 MMCORE, marker /.mmcore-rw). The live session uses it as
                 its writable layer and C: persists there. This does not
                 install mmcore onto an internal disk.
  --no-persist   do not add the MMCORE persistent partition
  -y, --yes      skip the confirmation prompt
  -h, --help     show this help

The device must not be mounted. Everything on it is overwritten.
--read-write and --no-persist cannot be combined.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--iso) ISO="$2"; shift 2 ;;
		--read-write) READ_WRITE=1; shift ;;
		--no-persist) PERSIST=0; shift ;;
		-y|--yes) ASSUME_YES=1; shift ;;
		-h|--help) usage; exit 0 ;;
		-*) die "unknown option: $1 (see --help)" ;;
		*)
			[ -z "${DEVICE}" ] || die "unexpected argument: $1"
			DEVICE="$1"
			shift
			;;
	esac
done

if [ "${READ_WRITE}" = 1 ] && [ "${PERSIST}" = 0 ]; then
	die "--read-write cannot be combined with --no-persist"
fi
if [ "${READ_WRITE}" = 1 ]; then
	PERSIST=1
fi

[ "$(uname -s)" = "Linux" ] || die "writing USB images is Linux-only"
[ -n "${DEVICE}" ] || die "no device given (see --help)"
[ -b "${DEVICE}" ] || die "${DEVICE} is not a block device"
[ -n "${ISO}" ] || ISO="dist/mmcore-fb-x86_64.iso"
[ -f "${ISO}" ] || die "${ISO} not found"

case "${ISO}" in
	*.zst)
		command -v zstd >/dev/null 2>&1 || die "zstd is required to read ${ISO}"
		DECOMPRESS=(zstd -dc)
		;;
	*.xz)
		command -v xz >/dev/null 2>&1 || die "xz is required to read ${ISO}"
		DECOMPRESS=(xz -dc)
		;;
	*.gz)
		command -v gzip >/dev/null 2>&1 || die "gzip is required to read ${ISO}"
		DECOMPRESS=(gzip -dc)
		;;
esac

if grep -q "^${DEVICE}[0-9p]" /proc/mounts 2>/dev/null; then
	die "${DEVICE} has mounted partitions; unmount them first"
fi

log "About to overwrite ${DEVICE} with ${ISO}"
if [ "${ASSUME_YES}" != "1" ]; then
	printf 'Type "yes" to continue: '
	read -r reply
	[ "${reply}" = "yes" ] || die "aborted"
fi

log "Writing the image"
"${DECOMPRESS[@]}" "${ISO}" | dd of="${DEVICE}" bs=4M conv=fsync status=progress

if [ "${PERSIST}" = "1" ]; then
	command -v sgdisk >/dev/null 2>&1 || die "sgdisk (gdisk) is required to add the MMCORE partition"
	command -v mkfs.ext4 >/dev/null 2>&1 || die "mkfs.ext4 (e2fsprogs) is required to add the MMCORE partition"
	# The mmcore ISO is a hybrid image whose GPT already uses partitions
	# 1-4 (Gap0, EFI boot partition, HFSPLUS, Gap1), so the persistent
	# partition is 5, carved from the free space beyond the ISO.
	log "Adding the MMCORE partition"
	sgdisk --move-second-header "${DEVICE}" >/dev/null \
		|| die "sgdisk could not resize the GPT on ${DEVICE}"
	sgdisk --new=5:0:0 --typecode=5:8300 --change-name=5:MMCORE \
		"${DEVICE}" >/dev/null \
		|| die "sgdisk could not add partition 5 (is ${DEVICE} larger than ${ISO}?)"
	partprobe "${DEVICE}" 2>/dev/null || true
	udevadm settle 2>/dev/null || true
	# Loop devices and hosts without udev do not create the new node on
	# their own. partx -a adds partition 5 when it is not already there.
	if [ ! -b "${DEVICE}5" ] && [ ! -b "${DEVICE}p5" ]; then
		partx -u "${DEVICE}" 2>/dev/null || true
	fi
	if [ ! -b "${DEVICE}5" ] && [ ! -b "${DEVICE}p5" ]; then
		partx -a -n 5:5 "${DEVICE}" 2>/dev/null || true
	fi
	# /dev/sdX5 or /dev/nvme0n1p5, /dev/mmcblk0p5, ...
	part="${DEVICE}5"
	[ -b "${part}" ] || part="${DEVICE}p5"
	[ -b "${part}" ] || die "could not find the new partition on ${DEVICE}"
	# A hybrid ISO mount can leave the whole disk read-only. Clear that
	# before formatting so the new filesystem is actually writable.
	blockdev --setrw "${part}" 2>/dev/null || true
	blockdev --setrw "${DEVICE}" 2>/dev/null || true
	mkfs.ext4 -q -L MMCORE "${part}"
	if [ "${READ_WRITE}" = "1" ]; then
		log "Seeding the read-write store on ${part}"
		seed="$(mktemp -d)"
		if ! mount -o rw "${part}" "${seed}"; then
			rmdir "${seed}"
			die "could not mount ${part} to seed the read-write store"
		fi
		if ! mkdir -p "${seed}/C" "${seed}/rw/upper" "${seed}/rw/work"; then
			umount "${seed}" 2>/dev/null || true
			rmdir "${seed}" 2>/dev/null || true
			die "could not seed directories on ${part}"
		fi
		printf '%s\n' 'mmcore read-write persistence' > "${seed}/.mmcore-rw"
		sync
		if ! umount "${seed}"; then
			die "could not unmount ${part} after seeding the read-write store"
		fi
		rmdir "${seed}"
		log "Read-write store ready: ${part} (label MMCORE, C: persists here)"
	else
		log "Persistence partition ready: ${part} (label MMCORE)"
	fi
fi

if [ "${READ_WRITE}" = "1" ]; then
	log "Done. Boot the device: the live system is read-write and C: persists on MMCORE."
elif [ "${PERSIST}" = "1" ]; then
	log "Done. Boot the device and mmcore's C: will persist on MMCORE."
else
	log "Done. Boot the device. No MMCORE partition was added; C: is lost on power-off."
fi
