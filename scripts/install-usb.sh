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
# --update refreshes the mmcore system files on an already-written stick: it
# copies the persistent MMCORE partition's contents to a staging directory,
# rewrites the whole image (fresh GPT plus a freshly formatted MMCORE
# partition), then copies the files back. C:, .mmbasic.ini, saved Wi-Fi
# settings and any other user files survive, whether or not the new image grew.
# The staging directory defaults to $TMPDIR (else /tmp) and must hold the used
# bytes of MMCORE; override it with MMCORE_UPDATE_BACKUP. It requires an
# existing MMCORE partition. A failed update keeps the staged copy and reports
# where it is.
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
UPDATE_BACKUP_DIR=""
UPDATE_MOUNT_DIR=""

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
	# A failed --update must never throw away the only copy of the user's
	# files: keep the staging directory and point at it.
	if [ -n "${UPDATE_BACKUP_DIR}" ]; then
		printf 'install-usb: MMCORE back-up kept at %s\n' \
			"${UPDATE_BACKUP_DIR}" >&2
	fi
	exit 1
}
cleanup() {
	[ -n "${UPDATE_MOUNT_DIR}" ] && umount "${UPDATE_MOUNT_DIR}" 2>/dev/null
	return 0
}
trap cleanup EXIT

# Locate the MMCORE partition node on ${DEVICE} (/dev/sdX5, /dev/nvme0n1p5, ...).
mmcore_part() {
	local p="${DEVICE}5"
	[ -b "${p}" ] || p="${DEVICE}p5"
	[ -b "${p}" ] || return 1
	printf '%s' "${p}"
}

# --update step 1: stage the MMCORE partition's contents outside the device so
# the normal install path can rewrite the whole medium (fresh GPT and a freshly
# formatted MMCORE partition). update_restore puts them back afterwards. The
# staging directory defaults to TMPDIR (else /tmp); MMCORE_UPDATE_BACKUP over-
# rides it.
update_backup() {
	command -v sgdisk >/dev/null 2>&1 \
		|| die "sgdisk (gdisk) is required to find the MMCORE partition"
	LC_ALL=C sgdisk --info=5 "${DEVICE}" >/dev/null 2>&1 \
		|| die "no MMCORE partition on ${DEVICE}; use a normal install (this wipes the device)"
	local part5
	part5="$(mmcore_part || true)"
	if [ -z "${part5}" ]; then
		partprobe "${DEVICE}" 2>/dev/null || true
		udevadm settle 2>/dev/null || true
		part5="$(mmcore_part || true)"
	fi
	[ -n "${part5}" ] || die "could not find the MMCORE partition node on ${DEVICE}"

	UPDATE_BACKUP_DIR="$(mktemp -d \
		"${MMCORE_UPDATE_BACKUP:-${TMPDIR:-/tmp}}/mmcore-update.XXXXXX")" \
		|| die "could not create a staging directory; set MMCORE_UPDATE_BACKUP"
	UPDATE_MOUNT_DIR="${UPDATE_BACKUP_DIR}/mnt"
	mkdir -p "${UPDATE_MOUNT_DIR}" "${UPDATE_BACKUP_DIR}/data"
	# A dirty ext4 (unclean power-off) refuses a read-only mount; noload reads
	# the last consistent state without replaying the journal.
	mount -o ro "${part5}" "${UPDATE_MOUNT_DIR}" 2>/dev/null \
		|| mount -o ro,noload "${part5}" "${UPDATE_MOUNT_DIR}" \
		|| die "could not mount ${part5} to back up the MMCORE contents"
	local used_kb avail_kb
	used_kb="$(df -Pk "${UPDATE_MOUNT_DIR}" | awk 'NR==2{print $3}')"
	avail_kb="$(df -Pk "${UPDATE_BACKUP_DIR}" | awk 'NR==2{print $4}')"
	if [ -n "${used_kb}" ] && [ -n "${avail_kb}" ] \
		&& [ "${used_kb}" -gt "${avail_kb}" ]; then
		die "not enough space in ${UPDATE_BACKUP_DIR} for the MMCORE contents (${used_kb} KiB needed, ${avail_kb} KiB free); set MMCORE_UPDATE_BACKUP to a larger location"
	fi
	cp -a "${UPDATE_MOUNT_DIR}/." "${UPDATE_BACKUP_DIR}/data/" \
		|| die "could not back up the MMCORE contents"
	sync
	umount "${UPDATE_MOUNT_DIR}" \
		|| die "could not unmount ${part5} after backing it up"
	UPDATE_MOUNT_DIR=""
	log "Backed up the MMCORE contents to ${UPDATE_BACKUP_DIR}"
}

# --update step 2: put the staged contents back into the newly formatted MMCORE
# partition (the normal install created it and ran mkfs.ext4 on it).
update_restore() {
	UPDATE_MOUNT_DIR="${UPDATE_BACKUP_DIR}/mnt"
	mkdir -p "${UPDATE_MOUNT_DIR}"
	mount -o rw "${part}" "${UPDATE_MOUNT_DIR}" \
		|| die "could not mount ${part} to restore the MMCORE contents"
	cp -a "${UPDATE_BACKUP_DIR}/data/." "${UPDATE_MOUNT_DIR}/" \
		|| die "could not restore the MMCORE contents"
	sync
	umount "${UPDATE_MOUNT_DIR}" \
		|| die "could not unmount ${part} after restoring the contents"
	UPDATE_MOUNT_DIR=""
	log "Restored the MMCORE contents"
	rm -rf "${UPDATE_BACKUP_DIR}"
	UPDATE_BACKUP_DIR=""
}

usage() {
	cat <<'EOF'
Usage: install-usb.sh [--iso PATH] [--read-write] [--no-persist] [--update] [-y|--yes] /dev/sdX

Write the mmcore live ISO to a USB stick or microSD.

Options:
  --iso PATH     image to write: raw .iso or compressed .iso.zst/.xz/.gz
                 (default: dist/mmcore-fb-x86_64.iso)
  --update       refresh the system files on a stick that already has an
                 MMCORE partition: its contents are backed up, the image is
                 rewritten, then the contents are restored, so C:,
                 .mmbasic.ini and saved settings survive. Requires an
                 existing MMCORE partition and staging space for its
                 contents (MMCORE_UPDATE_BACKUP, default $TMPDIR or /tmp).
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
	log "About to update the mmcore system image on ${DEVICE} (MMCORE contents backed up and restored)"
else
	log "About to overwrite ${DEVICE} with ${ISO}"
fi
if [ "${ASSUME_YES}" != "1" ]; then
	printf 'Type "yes" to continue: '
	read -r reply
	[ "${reply}" = "yes" ] || die "aborted"
fi

if [ "${UPDATE}" = 1 ]; then
	update_backup
fi

if [ "${UPDATE}" = 1 ]; then
	# A previously mounted hybrid ISO can leave the whole disk read-only.
	blockdev --setrw "${DEVICE}" 2>/dev/null || true
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

if [ "${UPDATE}" = 1 ]; then
	update_restore
fi

if [ "${UPDATE}" = 1 ]; then
	log "Done. The system image was updated and the MMCORE contents (C:, .mmbasic.ini, settings) were restored."
elif [ "${READ_WRITE}" = "1" ]; then
	log "Done. Boot the device: the live system is read-write and C: persists on MMCORE."
elif [ "${PERSIST}" = "1" ]; then
	log "Done. Boot the device and mmcore's C: will persist on MMCORE."
else
	log "Done. Boot the device. No MMCORE partition was added; C: is lost on power-off."
fi
