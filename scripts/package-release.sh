#!/usr/bin/env bash
# Build hardware images for Raspberry Pi 3, Zero 2 / Zero 2 W, and
# Raspberry Pi 400 and pack FAT-ready SD card zips — one zip per platform.
# Pi Zero 2 and Zero 2 W reuse the RASPPI=3 kernel (BCM2710).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE_DIR="${CIRCLE_DIR:-${REPO_ROOT}/circle}"
CONSOLE_DIR="${CONSOLE_DIR:-${REPO_ROOT}/console}"
BOOT_DIR="${BOOT_DIR:-${CIRCLE_DIR}/boot}"
WLAN_FW_DIR="${WLAN_FW_DIR:-${CIRCLE_DIR}/addon/wlan/firmware}"
VERSION="${VERSION:-0.1.1}"
DIST="${DIST:-${REPO_ROOT}/dist}"
PREFIX64="${PREFIX64:-aarch64-none-elf-}"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

die() {
	printf 'package-release: %s\n' "$*" >&2
	exit 1
}

clean_build_tree() {
	make -C "${CIRCLE_DIR}/lib" clean
	make -C "${CIRCLE_DIR}/addon/SDCard" clean
	make -C "${CIRCLE_DIR}/addon/fatfs" clean
	make -C "${CIRCLE_DIR}/lib/usb" clean
	make -C "${CIRCLE_DIR}/lib/fs" clean
	make -C "${CIRCLE_DIR}/lib/input" clean
	make -C "${CIRCLE_DIR}/lib/sound" clean
	make -C "${CIRCLE_DIR}/lib/net" clean || true
	make -C "${CIRCLE_DIR}/lib/sched" clean || true
	make -C "${CIRCLE_DIR}/addon/wlan" clean || true
	if [ -f "${CIRCLE_DIR}/addon/wlan/hostap/wpa_supplicant/Makefile.circle" ]; then
		make -C "${CIRCLE_DIR}/addon/wlan/hostap/wpa_supplicant" -f Makefile.circle clean || true
	fi
	make -C "${CONSOLE_DIR}" clean
}

kernel_elf_for_rasppi() {
	local rasppi="$1"
	if [ "${rasppi}" = "4" ]; then
		printf '%s\n' "${CONSOLE_DIR}/kernel8-rpi4.elf"
	else
		printf '%s\n' "${CONSOLE_DIR}/kernel8.elf"
	fi
}

# Circle compares _end to MEM_KERNEL_START+KERNEL_MAX_SIZE (not .img size).
check_kernel_end() {
	local elf="$1"
	local max_mb="${KERNEL_MAX_SIZE_MB:-8}"
	[ -f "${elf}" ] || die "missing ${elf}"
	PREFIX64="${PREFIX64}" python3 - "${elf}" "${max_mb}" <<'PY'
import os, subprocess, sys

elf, max_mb = sys.argv[1], int(sys.argv[2])
prefix = os.environ.get("PREFIX64", "aarch64-none-elf-")
nm = prefix + "nm"
out = subprocess.check_output([nm, elf], text=True, errors="replace")
end = None
for line in out.splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[-1] == "_end":
        end = int(parts[0], 16)
        break
if end is None:
    sys.stderr.write("package-release: no _end in %s\n" % elf)
    sys.exit(1)
start = 0x80000
limit = start + max_mb * 0x100000
used = end - start
print("kernel _end=0x%x (%d bytes from 0x%x, %.2f MiB) limit=%d MiB" % (
    end, used, start, used / 1048576.0, max_mb))
if end >= limit:
    sys.stderr.write(
        "package-release: %s _end 0x%x exceeds KERNEL_MAX_SIZE %dMB (limit 0x%x)\n"
        % (elf, end, max_mb, limit)
    )
    sys.exit(1)
PY
}

build_hardware() {
	local rasppi="$1"
	local elf
	log "Building hardware kernel (RASPPI=${rasppi}, AArch64, no QEMU extras)"
	clean_build_tree
	QEMU=0 RASPPI="${rasppi}" PREFIX64="${PREFIX64}" \
		MMB_VERSION="v${VERSION}" bash "${REPO_ROOT}/scripts/build.sh"
	elf="$(kernel_elf_for_rasppi "${rasppi}")"
	check_kernel_end "${elf}"
}

firmware_git_rev() {
	awk -F'= ' '/^FIRMWARE \?=/{print $2; exit}' "${CIRCLE_DIR}/boot/Makefile"
}

ensure_dtb() {
	local name="$1"
	local dest="${BOOT_DIR}/${name}"
	local rev url
	if [ -f "${dest}" ]; then
		return 0
	fi
	rev="$(firmware_git_rev)"
	[ -n "${rev}" ] || die "could not read Circle boot firmware revision"
	url="https://github.com/raspberrypi/firmware/raw/${rev}/boot/${name}"
	log "Downloading ${name}"
	wget -q -O "${dest}" "${url}" || {
		rm -f "${dest}"
		die "failed to download ${url}"
	}
}

ensure_firmware() {
	local needed=(
		bootcode.bin start.elf start4.elf fixup.dat fixup4.dat
		bcm2710-rpi-zero-2-w.dtb
		bcm2711-rpi-400.dtb bcm2711-rpi-4-b.dtb LICENCE.broadcom COPYING.linux
	)
	local missing=0
	if [ "${FORCE_FIRMWARE:-0}" = "1" ]; then
		missing=1
	else
		for f in "${needed[@]}"; do
			if [ ! -f "${BOOT_DIR}/${f}" ]; then
				missing=1
				break
			fi
		done
	fi
	if [ "${missing}" = "1" ]; then
		log "Downloading Raspberry Pi firmware (Circle boot/)"
		make -C "${BOOT_DIR}" firmware
	else
		log "Reusing existing firmware in circle/boot (set FORCE_FIRMWARE=1 to re-download)"
	fi

	ensure_dtb bcm2710-rpi-zero-2-w.dtb
	ensure_dtb bcm2710-rpi-zero-2.dtb

	log "Building Circle ARM stub for Raspberry Pi 4 / 400"
	make -C "${BOOT_DIR}" armstub64
}

ensure_wlan_firmware() {
	local needed=(
		brcmfmac43430-sdio.bin brcmfmac43430-sdio.txt
		brcmfmac43436-sdio.bin brcmfmac43436-sdio.txt
		brcmfmac43436s-sdio.bin brcmfmac43436s-sdio.txt
		brcmfmac43455-sdio.bin brcmfmac43455-sdio.txt
	)
	local missing=0
	local f
	if [ "${FORCE_WLAN_FIRMWARE:-0}" = "1" ]; then
		missing=1
	else
		for f in "${needed[@]}"; do
			if [ ! -f "${WLAN_FW_DIR}/${f}" ]; then
				missing=1
				break
			fi
		done
	fi
	if [ "${missing}" = "1" ]; then
		log "Downloading CYW4343x WLAN firmware (Circle addon/wlan/firmware)"
		make -C "${WLAN_FW_DIR}"
	else
		log "Reusing existing WLAN firmware (set FORCE_WLAN_FIRMWARE=1 to re-download)"
	fi
}

copy_wlan_firmware() {
	local stage="$1"
	mkdir -p "${stage}/firmware"
	# Circle looks up brcmfmac* by chip; ship the full set on WLAN zips.
	find "${WLAN_FW_DIR}" -maxdepth 1 -type f \( \
		-name 'brcmfmac*' -o -name 'LICENCE*' \
	\) -exec cp -a {} "${stage}/firmware/" \;
}

copy_if_present() {
	local src="$1"
	local dest="$2"
	if [ -f "${src}" ]; then
		cp -a "${src}" "${dest}"
	fi
}

write_version() {
	local stage="$1"
	local target_label="$2"
	local kernel_file="$3"
	{
		echo "MMBasic console ${VERSION}"
		echo "Target: ${target_label}"
		echo "${kernel_file} $(wc -c < "${stage}/${kernel_file}") bytes"
		echo "Built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	} > "${stage}/VERSION.txt"
}

zip_stage() {
	local stage="$1"
	local zip_name="$2"
	mkdir -p "${DIST}"
	rm -f "${DIST}/${zip_name}"
	( cd "${stage}" && zip -9 -r "${DIST}/${zip_name}" . )
	log "Release zip: ${DIST}/${zip_name}"
	unzip -l "${DIST}/${zip_name}"
}

package_kernel8_board() {
	local slug="$1"
	local label="$2"
	local config_src="$3"
	local with_wlan="$4"
	local with_zero2_dtb="$5"
	local stage="${DIST}/sdcard-${slug}"
	local zip_name="mmbasic-console-${slug}-v${VERSION}.zip"
	local kernel="kernel8.img"
	rm -rf "${stage}"
	mkdir -p "${stage}"
	cp -a "${CONSOLE_DIR}/${kernel}" "${stage}/${kernel}"
	cp -a "${config_src}" "${stage}/config.txt"
	cp -a "${REPO_ROOT}/scripts/sdcard/cmdline.txt" "${stage}/cmdline.txt"
	cp -a "${REPO_ROOT}/INSTALL.md" "${stage}/INSTALL.md"
	cp -a "${REPO_ROOT}/scripts/install-sdcard.sh" "${stage}/install-sdcard.sh"
	cp -a "${BOOT_DIR}/bootcode.bin" "${stage}/bootcode.bin"
	cp -a "${BOOT_DIR}/start.elf" "${stage}/start.elf"
	cp -a "${BOOT_DIR}/fixup.dat" "${stage}/fixup.dat"
	cp -a "${BOOT_DIR}/LICENCE.broadcom" "${stage}/LICENCE.broadcom"
	if [ "${with_zero2_dtb}" = "1" ]; then
		[ -f "${BOOT_DIR}/bcm2710-rpi-zero-2-w.dtb" ] \
			|| die "missing ${BOOT_DIR}/bcm2710-rpi-zero-2-w.dtb"
		cp -a "${BOOT_DIR}/bcm2710-rpi-zero-2-w.dtb" \
			"${stage}/bcm2710-rpi-zero-2-w.dtb"
		copy_if_present "${BOOT_DIR}/bcm2710-rpi-zero-2.dtb" \
			"${stage}/bcm2710-rpi-zero-2.dtb"
		copy_if_present "${BOOT_DIR}/COPYING.linux" "${stage}/COPYING.linux"
	fi
	write_version "${stage}" "${label}" "${kernel}"
	if [ "${with_wlan}" = "1" ]; then
		copy_wlan_firmware "${stage}"
	fi
	zip_stage "${stage}" "${zip_name}"
}

package_rpi3() {
	package_kernel8_board rpi3 \
		"Raspberry Pi 3 / 3B+ / 3A+ (AArch64)" \
		"${REPO_ROOT}/scripts/sdcard/config.txt" \
		1 0
}

package_pizero2() {
	package_kernel8_board pizero2 \
		"Raspberry Pi Zero 2 (AArch64, no onboard WLAN)" \
		"${REPO_ROOT}/scripts/sdcard/config-pizero2.txt" \
		0 1
}

package_pizero2w() {
	package_kernel8_board pizero2w \
		"Raspberry Pi Zero 2 W (AArch64, CYW43436 WLAN)" \
		"${REPO_ROOT}/scripts/sdcard/config-pizero2w.txt" \
		1 1
}

package_pi400() {
	local stage="${DIST}/sdcard-pi400"
	local zip_name="mmbasic-console-pi400-v${VERSION}.zip"
	local kernel="kernel8-rpi4.img"
	rm -rf "${stage}"
	mkdir -p "${stage}"
	cp -a "${CONSOLE_DIR}/${kernel}" "${stage}/${kernel}"
	cp -a "${REPO_ROOT}/scripts/sdcard/config-pi400.txt" "${stage}/config.txt"
	cp -a "${REPO_ROOT}/scripts/sdcard/cmdline.txt" "${stage}/cmdline.txt"
	cp -a "${REPO_ROOT}/INSTALL.md" "${stage}/INSTALL.md"
	cp -a "${REPO_ROOT}/scripts/install-sdcard.sh" "${stage}/install-sdcard.sh"
	cp -a "${BOOT_DIR}/start4.elf" "${stage}/start4.elf"
	cp -a "${BOOT_DIR}/fixup4.dat" "${stage}/fixup4.dat"
	cp -a "${BOOT_DIR}/armstub8-rpi4.bin" "${stage}/armstub8-rpi4.bin"
	cp -a "${BOOT_DIR}/bcm2711-rpi-400.dtb" "${stage}/bcm2711-rpi-400.dtb"
	cp -a "${BOOT_DIR}/bcm2711-rpi-4-b.dtb" "${stage}/bcm2711-rpi-4-b.dtb"
	cp -a "${BOOT_DIR}/LICENCE.broadcom" "${stage}/LICENCE.broadcom"
	copy_if_present "${BOOT_DIR}/COPYING.linux" "${stage}/COPYING.linux"
	write_version "${stage}" \
		"Raspberry Pi 400 (BCM2711; also Pi 4B / CM4) (AArch64)" \
		"${kernel}"
	copy_wlan_firmware "${stage}"
	zip_stage "${stage}" "${zip_name}"
}

pack_all() {
	package_rpi3
	package_pizero2
	package_pizero2w
	package_pi400
}

if [ "${PACK_ONLY:-0}" = "1" ]; then
	[ -f "${CONSOLE_DIR}/kernel8.img" ] || die "PACK_ONLY needs ${CONSOLE_DIR}/kernel8.img"
	[ -f "${CONSOLE_DIR}/kernel8-rpi4.img" ] || die "PACK_ONLY needs ${CONSOLE_DIR}/kernel8-rpi4.img"
	pack_all
	log "PACK_ONLY artifacts"
	ls -la "${DIST}"/mmbasic-console-*-v"${VERSION}".zip
	exit 0
fi

ensure_firmware
ensure_wlan_firmware
build_hardware 3
package_rpi3
package_pizero2
package_pizero2w
build_hardware 4
package_pi400

log "Restoring default QEMU Raspberry Pi 3 Circle config"
clean_build_tree
( cd "${CIRCLE_DIR}" && ./configure -r 3 -p "${PREFIX64}" --qemu -d USE_NAK_USB_FIX -d USE_QEMU_USB_FIX --kernel-max-size 8 -f )

log "Release artifacts"
ls -la "${DIST}"/mmbasic-console-*-v"${VERSION}".zip
