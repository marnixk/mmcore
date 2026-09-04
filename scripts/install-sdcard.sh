#!/usr/bin/env bash
# Prepare or update a Raspberry Pi SD card with a bootable MMBasic console.
set -euo pipefail

SCRIPT_PATH="$(readlink -f "${BASH_SOURCE[0]}" 2>/dev/null || realpath "${BASH_SOURCE[0]}")"
SCRIPT_DIR="$(cd "$(dirname "${SCRIPT_PATH}")" && pwd)"

MODE=""
MODEL=""
FROM=""
ASSUME_YES=0
ALLOW_NON_REMOVABLE=0
DEVICE=""

MOUNT_DIR=""
FROM_TMP=""
OFFSET_LOOP=""
CLEANED=0

usage() {
	cat <<'EOF'
Usage: install-sdcard.sh --bootstrap|--update --model MODEL [options] DEVICE

Install a bootable MMBasic console onto a Linux block device (microSD card).
This image is bare-metal: there is no Linux on the card.

Modes:
  --bootstrap         Partition and format DEVICE (MBR + one FAT32 partition),
                      then copy the boot files. ERASES THE WHOLE CARD.
  --update            Overwrite kernel, firmware, and config.txt on the existing
                      first FAT partition. BASIC programs and other files on C:
                      are left in place.

Options:
  --model MODEL       Board family (required):
                        rpi3    Raspberry Pi 3, 3B+, 3A+
                        pi400   Raspberry Pi 400 (also Pi 4B / CM4)
                      Aliases: pi3, 3, rpi4, pi4, 400
  --from PATH         Directory or zip of boot files. Defaults to this script's
                      directory when it already contains the kernel, otherwise a
                      matching mmbasic-console-<model>-v*.zip next to the script,
                      in the current directory, or in <repo>/dist.
  --yes               Do not prompt for confirmation (unsafe devices are still
                      refused)
  --allow-non-removable
                      Permit nvme/sata/virtio disks (normally refused; SD cards
                      show up as usb or mmc)
  -h, --help          Show this help

DEVICE is a whole-disk path such as /dev/sdb or /dev/mmcblk0. Pass the disk,
not a partition, to --bootstrap.

Examples:
  sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdb
  sudo ./install-sdcard.sh --update --model pi400 /dev/mmcblk0
  sudo ./install-sdcard.sh --bootstrap --model pi400 \
      --from mmbasic-console-pi400-v0.2.0.zip --yes /dev/sdb
EOF
}

die() {
	printf 'install-sdcard: %s\n' "$*" >&2
	exit 1
}

log() { printf '==> %s\n' "$*"; }

cleanup() {
	if [ "${CLEANED}" = "1" ]; then
		return 0
	fi
	CLEANED=1
	if [ -n "${MOUNT_DIR}" ] && findmnt -n "${MOUNT_DIR}" >/dev/null 2>&1; then
		umount "${MOUNT_DIR}" 2>/dev/null || umount -l "${MOUNT_DIR}" 2>/dev/null || true
	fi
	if [ -n "${MOUNT_DIR}" ] && [ -d "${MOUNT_DIR}" ]; then
		rmdir "${MOUNT_DIR}" 2>/dev/null || true
	fi
	if [ -n "${OFFSET_LOOP}" ]; then
		losetup -d "${OFFSET_LOOP}" 2>/dev/null || true
	fi
	if [ -n "${FROM_TMP}" ] && [ -d "${FROM_TMP}" ]; then
		rm -rf "${FROM_TMP}"
	fi
}
trap cleanup EXIT INT TERM

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing command '$1' (install the package that provides it)"
}

canonical_model() {
	local raw
	raw="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
	case "${raw}" in
		rpi3|pi3|3|raspberrypi3|rp3)
			printf 'rpi3\n'
			;;
		pi400|rpi400|400|rpi4|pi4|raspberrypi400|cm4)
			printf 'pi400\n'
			;;
		*)
			die "unknown --model '${1}' (expected rpi3 or pi400)"
			;;
	esac
}

model_label() {
	case "$1" in
		rpi3) printf 'Raspberry Pi 3 / 3B+ / 3A+\n' ;;
		pi400) printf 'Raspberry Pi 400 (also Pi 4B / CM4)\n' ;;
	esac
}

required_files_for_model() {
	case "$1" in
		rpi3)
			printf '%s\n' kernel8.img config.txt bootcode.bin start.elf fixup.dat
			;;
		pi400)
			printf '%s\n' kernel8-rpi4.img config.txt armstub8-rpi4.bin start4.elf fixup4.dat
			;;
	esac
}

optional_files_for_model() {
	case "$1" in
		rpi3)
			printf '%s\n' LICENCE.broadcom INSTALL.md VERSION.txt install-sdcard.sh
			;;
		pi400)
			printf '%s\n' \
				bcm2711-rpi-400.dtb bcm2711-rpi-4-b.dtb \
				LICENCE.broadcom COPYING.linux INSTALL.md VERSION.txt install-sdcard.sh
			;;
	esac
}

kernel_name_for_model() {
	case "$1" in
		rpi3) printf 'kernel8.img\n' ;;
		pi400) printf 'kernel8-rpi4.img\n' ;;
	esac
}

zip_glob_for_model() {
	case "$1" in
		rpi3) printf 'mmbasic-console-rpi3-v*.zip\n' ;;
		pi400) printf 'mmbasic-console-pi400-v*.zip\n' ;;
	esac
}

is_blockdev() {
	[ -b "$1" ]
}

lsblk_type() {
	lsblk -ndo TYPE "$1" 2>/dev/null || true
}

lsblk_pkname() {
	lsblk -ndo PKNAME "$1" 2>/dev/null || true
}

lsblk_size() {
	lsblk -ndo SIZE "$1" 2>/dev/null || true
}

device_bytes() {
	blockdev --getsize64 "$1"
}

partition_node() {
	local disk="$1"
	local n="${2:-1}"
	local base
	base="$(basename "${disk}")"
	if [[ "${base}" =~ [0-9]$ ]]; then
		printf '%sp%s\n' "${disk}" "${n}"
	else
		printf '%s%s\n' "${disk}" "${n}"
	fi
}

root_block_device() {
	local src pk
	src="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
	src="${src%%[*}"
	if [ -n "${src}" ] && [ -b "${src}" ]; then
		pk="$(lsblk_pkname "${src}")"
		if [ -n "${pk}" ]; then
			printf '/dev/%s\n' "${pk}"
			return 0
		fi
		printf '%s\n' "${src}"
	fi
}

loop_backing_file() {
	local dev="$1"
	losetup -n --raw -O BACK-FILE "${dev}" 2>/dev/null | tr -d '[:space:]' || true
}

device_transport() {
	lsblk -ndo TRAN "$1" 2>/dev/null || true
}

device_is_unsafe() {
	local dev="$1"
	local root_dev name mp src pk backing tran dtype overlay
	root_dev="$(root_block_device || true)"
	if [ -n "${root_dev}" ] && [ "$(readlink -f "${dev}")" = "$(readlink -f "${root_dev}")" ]; then
		printf 'refusing to use %s: it holds the running system root\n' "${dev}"
		return 0
	fi

	while read -r name mp; do
		[ -z "${mp}" ] && continue
		case "${mp}" in
			/|/boot|/boot/*|/usr|/usr/*|/var|/var/*|/home|/home/*|/etc|/etc/*|/opt|/opt/*)
				printf 'refusing to use %s: mounted at %s\n' "${name}" "${mp}"
				return 0
				;;
		esac
	done < <(lsblk -lnpo NAME,MOUNTPOINT "${dev}" 2>/dev/null || true)

	src="$(findmnt -n -o SOURCE / 2>/dev/null || true)"
	if [ -n "${src}" ] && [ -b "${src}" ]; then
		if [ "$(readlink -f "${src}")" = "$(readlink -f "${dev}")" ]; then
			printf 'refusing to use %s: it is mounted at /\n' "${dev}"
			return 0
		fi
		pk="$(lsblk_pkname "${src}")"
		if [ -n "${pk}" ] && [ "$(readlink -f "/dev/${pk}")" = "$(readlink -f "${dev}")" ]; then
			printf 'refusing to use %s: it is the parent of the root filesystem\n' "${dev}"
			return 0
		fi
	fi

	dtype="$(lsblk_type "${dev}")"
	if [ "${dtype}" = "loop" ]; then
		backing="$(loop_backing_file "${dev}")"
		case "${backing}" in
			*overlay2*|*overlay*|*docker*|*containerd*|*isod*|*rootar*)
				printf 'refusing to use %s: backing file looks like the host overlay (%s)\n' "${dev}" "${backing}"
				return 0
				;;
		esac
		overlay="$(findmnt -n -o OPTIONS / 2>/dev/null || true)"
		if [ -n "${backing}" ] && [ -n "${overlay}" ] && printf '%s' "${overlay}" | grep -Fq "${backing}"; then
			printf 'refusing to use %s: it backs the running root overlay\n' "${dev}"
			return 0
		fi
	fi

	tran="$(device_transport "${dev}")"
	case "${tran}" in
		usb|mmc|sd|"")
			;;
		virtio|nvme|sata|ata|iscsi|fc)
			if [ "${ALLOW_NON_REMOVABLE}" != "1" ]; then
				printf 'refusing to use %s: transport is %s (pass --allow-non-removable only if this really is the SD card)\n' "${dev}" "${tran}"
				return 0
			fi
			;;
		*)
			if [ "${ALLOW_NON_REMOVABLE}" != "1" ] && [ "${dtype}" != "loop" ]; then
				printf 'refusing to use %s: unexpected transport %s (pass --allow-non-removable to override)\n' "${dev}" "${tran}"
				return 0
			fi
			;;
	esac
	return 1
}

unmount_device_tree() {
	local dev="$1"
	local mp
	# Deepest mountpoints first so nested mounts come off cleanly.
	while read -r mp; do
		[ -z "${mp}" ] && continue
		log "Unmounting ${mp}"
		umount "${mp}" || umount -l "${mp}"
	done < <(lsblk -lnpo MOUNTPOINT "${dev}" | awk 'NF{print}' | awk '{print length, $0}' | sort -nr | cut -d' ' -f2-)
}

confirm() {
	local prompt="$1"
	local reply
	if [ "${ASSUME_YES}" = "1" ]; then
		return 0
	fi
	if [ ! -t 0 ]; then
		die "refusing to run without a TTY; pass --yes after you have checked DEVICE"
	fi
	printf '%s\nType the device path to continue: ' "${prompt}" >&2
	read -r reply
	[ "${reply}" = "${DEVICE}" ] || die "aborted (typed '${reply}', expected '${DEVICE}')"
}

latest_zip_in() {
	local dir="$1"
	local pattern="$2"
	local files
	shopt -s nullglob
	# shellcheck disable=SC2086
	files=("${dir}"/${pattern})
	shopt -u nullglob
	if [ "${#files[@]}" -eq 0 ] || [ ! -e "${files[0]}" ]; then
		return 0
	fi
	printf '%s\n' "${files[@]}" | sort -V | tail -n 1
}

extract_zip() {
	local zip="$1"
	need_cmd unzip
	FROM_TMP="$(mktemp -d "${TMPDIR:-/tmp}/mmbasic-boot.XXXXXX")"
	unzip -q -o "${zip}" -d "${FROM_TMP}"
	if [ -f "${FROM_TMP}/$(kernel_name_for_model "${MODEL}")" ]; then
		printf '%s\n' "${FROM_TMP}"
		return 0
	fi
	# Zip with a single top-level directory.
	local kids kernel
	kernel="$(kernel_name_for_model "${MODEL}")"
	mapfile -t kids < <(find "${FROM_TMP}" -mindepth 1 -maxdepth 1 -type d)
	if [ "${#kids[@]}" -eq 1 ] && [ -f "${kids[0]}/${kernel}" ]; then
		printf '%s\n' "${kids[0]}"
		return 0
	fi
	die "zip '${zip}' does not contain ${kernel} for --model ${MODEL}"
}

resolve_from() {
	local kernel pattern candidate repo_root
	kernel="$(kernel_name_for_model "${MODEL}")"
	pattern="$(zip_glob_for_model "${MODEL}")"

	if [ -n "${FROM}" ]; then
		if [ -f "${FROM}" ]; then
			case "${FROM}" in
				*.zip|*.ZIP) extract_zip "$(readlink -f "${FROM}")"; return 0 ;;
			esac
			die "--from file must be a zip: ${FROM}"
		fi
		if [ -d "${FROM}" ]; then
			FROM="$(readlink -f "${FROM}")"
			[ -f "${FROM}/${kernel}" ] || die "--from directory is missing ${kernel}"
			printf '%s\n' "${FROM}"
			return 0
		fi
		die "--from path not found: ${FROM}"
	fi

	if [ -f "${SCRIPT_DIR}/${kernel}" ]; then
		printf '%s\n' "${SCRIPT_DIR}"
		return 0
	fi

	repo_root="$(cd "${SCRIPT_DIR}/.." && pwd)"
	for dir in "${SCRIPT_DIR}" "$(pwd)" "${repo_root}/dist"; do
		[ -d "${dir}" ] || continue
		candidate="$(latest_zip_in "${dir}" "${pattern}" || true)"
		if [ -n "${candidate}" ]; then
			log "Using boot files from ${candidate}"
			extract_zip "${candidate}"
			return 0
		fi
	done

	if [ -d "${repo_root}/dist/sdcard-${MODEL}" ] && [ -f "${repo_root}/dist/sdcard-${MODEL}/${kernel}" ]; then
		printf '%s\n' "${repo_root}/dist/sdcard-${MODEL}"
		return 0
	fi

	die "cannot find ${kernel} or ${pattern}; unzip a release next to this script or pass --from"
}

assert_boot_files() {
	local dir="$1"
	local f
	while read -r f; do
		[ -f "${dir}/${f}" ] || die "boot files missing ${f} (looked in ${dir})"
	done < <(required_files_for_model "${MODEL}")
}

write_mbr_fat32() {
	local dev="$1"
	need_cmd python3
	python3 - "${dev}" <<'PY'
import os, struct, sys

dev = sys.argv[1]
fd = os.open(dev, os.O_RDWR)
try:
    size = os.lseek(fd, 0, os.SEEK_END)
    if size < 32 * 1024 * 1024:
        raise SystemExit(f"{dev} is only {size} bytes; need at least 32 MiB")
    total_lba = size // 512
    start = 2048
    if total_lba <= start + 64:
        raise SystemExit(f"{dev} is too small for an MBR FAT32 partition")
    count = total_lba - start
    mbr = bytearray(512)
    mbr[510:512] = b"\x55\xaa"
    mbr[446:462] = struct.pack(
        "<B3sB3sII",
        0x80,
        bytes([0x00, 0x02, 0x00]),
        0x0C,
        bytes([0xFE, 0xFF, 0xFF]),
        start,
        count,
    )
    os.lseek(fd, 0, os.SEEK_SET)
    os.write(fd, mbr)
finally:
    os.close(fd)
PY
	sync
}

read_first_partition_lba() {
	local dev="$1"
	python3 - "${dev}" <<'PY'
import os, struct, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
try:
    data = os.read(fd, 512)
finally:
    os.close(fd)
if len(data) < 512 or data[510:512] != b"\x55\xaa":
    raise SystemExit("no MBR signature on " + sys.argv[1])
boot, chs_s, ptype, chs_e, start, count = struct.unpack_from("<B3sB3sII", data, 446)
if start == 0 or count == 0:
    raise SystemExit("MBR partition 1 is empty on " + sys.argv[1])
print(start)
print(count)
print(ptype)
PY
}

attach_offset_loop() {
	local disk="$1"
	local start_lba="$2"
	local count_lba="$3"
	local offset sizelimit
	offset=$((start_lba * 512))
	sizelimit=$((count_lba * 512))
	OFFSET_LOOP="$(losetup --find --show --offset "${offset}" --sizelimit "${sizelimit}" "${disk}")"
	printf '%s\n' "${OFFSET_LOOP}"
}

resolve_fat_partition() {
	local disk="$1"
	local part start_count start count ptype
	local t
	t="$(lsblk_type "${disk}")"
	if [ "${t}" = "part" ]; then
		printf '%s\n' "${disk}"
		return 0
	fi
	part="$(partition_node "${disk}" 1)"
	if [ -b "${part}" ]; then
		printf '%s\n' "${part}"
		return 0
	fi
	# Loop devices here often have max_part=0, so no /dev/loopNp1 node.
	start_count="$(read_first_partition_lba "${disk}")"
	start="$(printf '%s\n' "${start_count}" | sed -n '1p')"
	count="$(printf '%s\n' "${start_count}" | sed -n '2p')"
	ptype="$(printf '%s\n' "${start_count}" | sed -n '3p')"
	case "${ptype}" in
		1|4|6|11|12|14|16) ;;
		*)
			log "warning: MBR partition type is ${ptype} (expected FAT); continuing"
			;;
	esac
	log "No partition node for ${disk}; mapping FAT at LBA ${start} via loop"
	attach_offset_loop "${disk}" "${start}" "${count}"
}

copy_boot_files() {
	local src="$1"
	local dest="$2"
	local f
	while read -r f; do
		cp -a "${src}/${f}" "${dest}/${f}"
	done < <(required_files_for_model "${MODEL}")
	while read -r f; do
		if [ -f "${src}/${f}" ]; then
			cp -a "${src}/${f}" "${dest}/${f}"
		fi
	done < <(optional_files_for_model "${MODEL}")
	sync
}

run_mtools() {
	local err rc
	need_cmd "$1"
	err="$(mktemp "${TMPDIR:-/tmp}/mmbasic-mtools.XXXXXX")"
	set +e
	"$@" 2>"${err}"
	rc=$?
	set -e
	if [ "${rc}" -ne 0 ]; then
		grep -v 'Could not get geometry of device' "${err}" >&2 || cat "${err}" >&2
		rm -f "${err}"
		return "${rc}"
	fi
	rm -f "${err}"
	return 0
}

copy_boot_files_mtools() {
	local src="$1"
	local fat="$2"
	local f
	export MTOOLS_SKIP_CHECK=1
	while read -r f; do
		run_mtools mcopy -o -i "${fat}" "${src}/${f}" "::${f}"
	done < <(required_files_for_model "${MODEL}")
	while read -r f; do
		if [ -f "${src}/${f}" ]; then
			run_mtools mcopy -o -i "${fat}" "${src}/${f}" "::${f}"
		fi
	done < <(optional_files_for_model "${MODEL}")
	sync
}

install_boot_files() {
	local src="$1"
	local fat="$2"
	MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/mmbasic-mnt.XXXXXX")"
	if mount -t vfat -o utf8,shortname=mixed "${fat}" "${MOUNT_DIR}" 2>/dev/null \
		|| mount "${fat}" "${MOUNT_DIR}" 2>/dev/null; then
		log "Copying $(model_label "${MODEL}") boot files"
		copy_boot_files "${src}" "${MOUNT_DIR}"
		log "Installed files:"
		ls -l "${MOUNT_DIR}"
		sync
		umount "${MOUNT_DIR}"
	else
		command -v mcopy >/dev/null 2>&1 || die "cannot mount vfat on ${fat} and mtools (mcopy) is not installed"
		log "Kernel has no vfat mount; copying with mtools onto ${fat}"
		copy_boot_files_mtools "${src}" "${fat}"
		log "Installed files:"
		export MTOOLS_SKIP_CHECK=1
		run_mtools mdir -a -i "${fat}" || run_mtools mdir -i "${fat}"
	fi
	rmdir "${MOUNT_DIR}" 2>/dev/null || true
	MOUNT_DIR=""
}

parse_args() {
	if [ $# -eq 0 ]; then
		usage >&2
		exit 2
	fi
	while [ $# -gt 0 ]; do
		case "$1" in
			-h|--help)
				usage
				exit 0
				;;
			--bootstrap)
				[ -z "${MODE}" ] || die "pass only one of --bootstrap or --update"
				MODE=bootstrap
				shift
				;;
			--update)
				[ -z "${MODE}" ] || die "pass only one of --bootstrap or --update"
				MODE=update
				shift
				;;
			--model)
				[ $# -ge 2 ] || die "--model needs an argument"
				MODEL="$(canonical_model "$2")"
				shift 2
				;;
			--model=*)
				MODEL="$(canonical_model "${1#--model=}")"
				shift
				;;
			--from)
				[ $# -ge 2 ] || die "--from needs an argument"
				FROM="$2"
				shift 2
				;;
			--from=*)
				FROM="${1#--from=}"
				shift
				;;
			--yes|-y)
				ASSUME_YES=1
				shift
				;;
			--allow-non-removable)
				ALLOW_NON_REMOVABLE=1
				shift
				;;
			--)
				shift
				break
				;;
			-*)
				die "unknown option: $1"
				;;
			*)
				[ -z "${DEVICE}" ] || die "unexpected extra argument: $1"
				DEVICE="$1"
				shift
				;;
		esac
	done
	while [ $# -gt 0 ]; do
		[ -z "${DEVICE}" ] || die "unexpected extra argument: $1"
		DEVICE="$1"
		shift
	done
}

main() {
	parse_args "$@"

	[ -n "${MODE}" ] || die "choose --bootstrap or --update (see --help)"
	[ -n "${MODEL}" ] || die "--model is required (rpi3 or pi400)"
	[ -n "${DEVICE}" ] || die "DEVICE is required, e.g. /dev/sdb"

	need_cmd lsblk
	need_cmd mkfs.vfat
	need_cmd mount
	need_cmd umount
	need_cmd losetup
	need_cmd python3
	need_cmd blockdev

	if [ ! -b "${DEVICE}" ] && [ -b "/dev/${DEVICE#/dev/}" ]; then
		DEVICE="/dev/${DEVICE#/dev/}"
	fi
	DEVICE="$(readlink -f "${DEVICE}")"
	is_blockdev "${DEVICE}" || die "${DEVICE} is not a block device"

	local unsafe
	if unsafe="$(device_is_unsafe "${DEVICE}")"; then
		die "${unsafe}"
	fi

	local dtype
	dtype="$(lsblk_type "${DEVICE}")"
	if [ "${MODE}" = "bootstrap" ]; then
		if [ "${dtype}" = "part" ]; then
			die "--bootstrap needs the whole disk (got partition ${DEVICE}; try the parent disk)"
		fi
		if [ -n "$(lsblk_pkname "${DEVICE}")" ] && [ "${dtype}" != "loop" ]; then
			die "--bootstrap needs the whole disk, not ${DEVICE}"
		fi
	fi

	local bytes
	bytes="$(device_bytes "${DEVICE}")"
	if [ "${MODE}" = "bootstrap" ] && [ "${bytes}" -lt $((32 * 1024 * 1024)) ]; then
		die "${DEVICE} is only ${bytes} bytes; need at least 32 MiB"
	fi

	local src
	src="$(resolve_from)"
	assert_boot_files "${src}"

	if [ "$(id -u)" -ne 0 ]; then
		die "run as root (sudo $0 ...)"
	fi

	local size_h
	size_h="$(lsblk_size "${DEVICE}")"
	if [ "${MODE}" = "bootstrap" ]; then
		confirm "$(cat <<EOF
This will ERASE all data on ${DEVICE} (${size_h:-?}, $(model_label "${MODEL}")).
A new MBR partition table and FAT32 filesystem named MMBASIC will be created,
then the MMBasic boot files will be copied to the partition root.
EOF
)"
	else
		confirm "$(cat <<EOF
This will overwrite MMBasic boot files on ${DEVICE} (${size_h:-?}, $(model_label "${MODEL}")).
User files already on the FAT volume (C:) are kept.
EOF
)"
	fi

	unmount_device_tree "${DEVICE}"

	local fat
	if [ "${MODE}" = "bootstrap" ]; then
		if command -v wipefs >/dev/null 2>&1; then
			log "Wiping signatures on ${DEVICE}"
			wipefs -a "${DEVICE}" >/dev/null
		fi
		log "Writing MBR + FAT32 partition on ${DEVICE}"
		write_mbr_fat32 "${DEVICE}"
		if command -v partx >/dev/null 2>&1; then
			partx -u "${DEVICE}" 2>/dev/null || partx -a "${DEVICE}" 2>/dev/null || true
		fi
		sleep 0.2
		fat="$(resolve_fat_partition "${DEVICE}")"
		log "Formatting ${fat} as FAT32 (MMBASIC)"
		mkfs.vfat -F 32 -n MMBASIC "${fat}" >/dev/null
	else
		fat="$(resolve_fat_partition "${DEVICE}")"
		[ -b "${fat}" ] || die "could not find a FAT partition on ${DEVICE}"
	fi

	install_boot_files "${src}" "${fat}"
	if [ -n "${OFFSET_LOOP}" ]; then
		losetup -d "${OFFSET_LOOP}"
		OFFSET_LOOP=""
	fi
	sync

	log "Done. ${MODE} ${MODEL} on ${DEVICE}."
	log "Insert the card, connect HDMI + 3.3 V serial (115200 8N1 on GPIO 14/15), and power on."
}

main "$@"
