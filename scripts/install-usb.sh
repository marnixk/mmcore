#!/usr/bin/env bash
# Write the mmcore live ISO to a USB device and add a persistent MMCORE
# partition, so mmcore's C:/ drive survives reboots.
#
# The ISO is written from sector 0 (BIOS El Torito + UEFI), then a second GPT
# partition is carved out of the free space and labelled MMCORE. The live
# system mounts it at /media/mmcore on boot (see the ISO's mmcore-persist
# script).
#
# Linux only. Usage:
#   sudo ./install-usb.sh --iso dist/mmcore-fb-x86_64.iso /dev/sdX
#   sudo ./install-usb.sh --no-persist /dev/sdX
set -euo pipefail

ISO=""
DEVICE=""
PERSIST=1
ASSUME_YES=0

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'install-usb: %s\n' "$*" >&2
	exit 1
}

usage() {
	cat <<'EOF'
Usage: install-usb.sh [--iso PATH] [--no-persist] [-y|--yes] /dev/sdX

Write the mmcore live ISO to a USB device.

Options:
  --iso PATH    ISO to write (default: dist/mmcore-fb-x86_64.iso)
  --no-persist  do not add the MMCORE persistent partition
  -y, --yes     skip the confirmation prompt
  -h, --help    show this help

The device must not be mounted. Everything on it is overwritten.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--iso) ISO="$2"; shift 2 ;;
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

[ "$(uname -s)" = "Linux" ] || die "writing USB images is Linux-only"
[ -n "${DEVICE}" ] || die "no device given (see --help)"
[ -b "${DEVICE}" ] || die "${DEVICE} is not a block device"
[ -n "${ISO}" ] || ISO="dist/mmcore-fb-x86_64.iso"
[ -f "${ISO}" ] || die "${ISO} not found"

if grep -q "^${DEVICE}[0-9p]" /proc/mounts 2>/dev/null; then
	die "${DEVICE} has mounted partitions; unmount them first"
fi

log "About to overwrite ${DEVICE} with ${ISO}"
if [ "${ASSUME_YES}" != "1" ]; then
	printf 'Type "yes" to continue: '
	read -r reply
	[ "${reply}" = "yes" ] || die "aborted"
fi

log "Writing the ISO"
dd if="${ISO}" of="${DEVICE}" bs=4M conv=fsync status=progress

	if [ "${PERSIST}" = "1" ]; then
		command -v sgdisk >/dev/null 2>&1 || die "sgdisk (gdisk) is required for --persist"
		command -v mkfs.ext4 >/dev/null 2>&1 || die "mkfs.ext4 (e2fsprogs) is required for --persist"
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
		# /dev/sdX5 or /dev/nvme0n1p5, /dev/mmcblk0p5, ...
		part="${DEVICE}5"
		[ -b "${part}" ] || part="${DEVICE}p5"
		[ -b "${part}" ] || die "could not find the new partition on ${DEVICE}"
		mkfs.ext4 -q -L MMCORE "${part}"
		log "Persistence partition ready: ${part} (label MMCORE)"
	fi

log "Done. Boot the device and mmcore's C: will persist on MMCORE."
