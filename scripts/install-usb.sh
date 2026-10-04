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
#   sudo ./install-usb.sh --update --iso mmcore-fb-x86_64.iso.zst /dev/sdX
#
# --update refreshes the mmcore system files on an already-written stick
# without touching the persistent MMCORE partition. It writes the new ISO only
# as far as the existing partition 5 starts, then re-creates that GPT entry at
# its old offset, so C:, .mmbasic.ini, saved Wi-Fi settings and any other user
# files on the ext4 survive. No reformat, no mkfs. It requires an existing
# MMCORE partition and refuses an image that would overlap it.
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
UPDATE=0
ASSUME_YES=0
DECOMPRESS=(cat)
TMP_ISO=""

# GNU coreutils dd accepts status=progress; BusyBox dd (the default on Alpine,
# the distro the ISO itself targets) aborts with an invalid-argument error
# (#1065). Probe the host dd once and only pass the flag where it works.
DD_PROGRESS=""
if dd --version 2>/dev/null | grep -q coreutils; then
	DD_PROGRESS="status=progress"
fi

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'install-usb: %s\n' "$*" >&2
	exit 1
}
cleanup() {
	[ -n "${TMP_ISO}" ] && rm -f "${TMP_ISO}"
	return 0
}
trap cleanup EXIT

usage() {
	cat <<'EOF'
Usage: install-usb.sh [--iso PATH] [--read-write] [--no-persist] [--update] [-y|--yes] /dev/sdX

Write the mmcore live ISO to a USB stick or microSD.

Options:
  --iso PATH     image to write: raw .iso or compressed .iso.zst/.xz/.gz
                 (default: dist/mmcore-fb-x86_64.iso)
  --update       refresh the system files on a stick that already has an
                 MMCORE partition, without reformatting it: C:, .mmbasic.ini
                 and saved settings on that partition survive. Requires an
                 existing MMCORE partition.
  --read-write   provision the free space as a read-write store (label
                 MMCORE, marker /.mmcore-rw). The live session uses it as
                 its writable layer and C: persists there. This does not
                 install mmcore onto an internal disk.
  --no-persist   do not add the MMCORE persistent partition
  -y, --yes      skip the confirmation prompt
  -h, --help     show this help

The device must not be mounted. Everything on it is overwritten.
--read-write and --no-persist cannot be combined.
--update cannot be combined with --read-write or --no-persist.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--iso) ISO="$2"; shift 2 ;;
		--read-write) READ_WRITE=1; shift ;;
		--no-persist) PERSIST=0; shift ;;
		--update) UPDATE=1; shift ;;
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

if [ "${UPDATE}" = 1 ] && { [ "${READ_WRITE}" = 1 ] || [ "${PERSIST}" = 0 ]; }; then
	die "--update cannot be combined with --read-write or --no-persist"
fi
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

if [ "${UPDATE}" = 1 ]; then
	log "About to update the mmcore system image on ${DEVICE} (MMCORE partition kept)"
else
	log "About to overwrite ${DEVICE} with ${ISO}"
fi
if [ "${ASSUME_YES}" != "1" ]; then
	printf 'Type "yes" to continue: '
	read -r reply
	[ "${reply}" = "yes" ] || die "aborted"
fi

if [ "${UPDATE}" = 1 ]; then
	command -v sgdisk >/dev/null 2>&1 \
		|| die "sgdisk (gdisk) is required to update the MMCORE partition"
	# Read the persistent partition's exact geometry before the ISO write
	# replaces the primary GPT with the image's own (partitions 1-4).
	part5_info="$(LC_ALL=C sgdisk --info=5 "${DEVICE}" 2>/dev/null)" \
		|| die "no MMCORE partition on ${DEVICE}; use a normal install (this wipes the device)"
	part5_first="$(printf '%s\n' "${part5_info}" | awk '/^First sector:/{print $3; exit}')"
	part5_last="$(printf '%s\n' "${part5_info}" | awk '/^Last sector:/{print $3; exit}')"
	[ -n "${part5_first}" ] && [ -n "${part5_last}" ] \
		|| die "could not read the MMCORE partition geometry on ${DEVICE}"

	# Materialise the image so its real size can be checked against the
	# partition start: a compressed stream cannot be measured in advance,
	# and an image that grew past the partition would clobber C:.
	write_src="${ISO}"
	case "${ISO}" in
		*.zst|*.xz|*.gz)
			TMP_ISO="$(mktemp)"
			"${DECOMPRESS[@]}" "${ISO}" > "${TMP_ISO}"
			write_src="${TMP_ISO}"
			;;
	esac
	iso_bytes="$(wc -c < "${write_src}")"
	part5_bytes=$((part5_first * 512))
	if [ "${iso_bytes}" -gt "${part5_bytes}" ]; then
		die "the new image (${iso_bytes} bytes) would overlap the MMCORE partition at sector ${part5_first}; a normal install wipes the device, or back up C: and reinstall"
	fi

	log "Refreshing the system image before the MMCORE partition"
	# shellcheck disable=SC2086  # empty on BusyBox dd, a single word on GNU dd
	dd if="${write_src}" of="${DEVICE}" bs=4M conv=fsync ${DD_PROGRESS}

	# The ISO's own GPT names only partitions 1-4. Re-create the MMCORE
	# entry at its old offset (no mkfs), then move the backup header to the
	# device end so the partition stays addressable after a reboot.
	log "Keeping the MMCORE partition (sectors ${part5_first}-${part5_last})"
	sgdisk --move-second-header "${DEVICE}" >/dev/null \
		|| die "sgdisk could not resize the GPT on ${DEVICE}"
	sgdisk --new=5:"${part5_first}":"${part5_last}" --typecode=5:8300 \
		--change-name=5:MMCORE "${DEVICE}" >/dev/null \
		|| die "sgdisk could not restore partition 5 on ${DEVICE}"
	partprobe "${DEVICE}" 2>/dev/null || true
	udevadm settle 2>/dev/null || true
	# Same node fallback as the fresh install: hosts without udev (or a loop
	# device) may not get the partition node back after the GPT change.
	if [ ! -b "${DEVICE}5" ] && [ ! -b "${DEVICE}p5" ]; then
		partx -u "${DEVICE}" 2>/dev/null || true
	fi
	if [ ! -b "${DEVICE}5" ] && [ ! -b "${DEVICE}p5" ]; then
		partx -a -n 5:5 "${DEVICE}" 2>/dev/null || true
	fi
	log "Updated. The MMCORE partition (C:, .mmbasic.ini, settings) is untouched."
	exit 0
fi

log "Writing the image"
# shellcheck disable=SC2086  # empty on BusyBox dd, a single word on GNU dd
"${DECOMPRESS[@]}" "${ISO}" | dd of="${DEVICE}" bs=4M conv=fsync ${DD_PROGRESS}

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
