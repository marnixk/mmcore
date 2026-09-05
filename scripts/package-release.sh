#!/usr/bin/env bash
# Build hardware images for Raspberry Pi 3 and Raspberry Pi 400 and pack
# FAT-ready SD card zips — one zip per platform.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE_DIR="${REPO_ROOT}/circle"
CONSOLE_DIR="${REPO_ROOT}/console"
BOOT_DIR="${CIRCLE_DIR}/boot"
VERSION="${VERSION:-0.1.1}"
DIST="${REPO_ROOT}/dist"
PREFIX64="${PREFIX64:-aarch64-none-elf-}"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

clean_build_tree() {
	make -C "${CIRCLE_DIR}/lib" clean
	make -C "${CIRCLE_DIR}/addon/SDCard" clean
	make -C "${CIRCLE_DIR}/addon/fatfs" clean
	make -C "${CIRCLE_DIR}/lib/usb" clean
	make -C "${CIRCLE_DIR}/lib/fs" clean
	make -C "${CIRCLE_DIR}/lib/input" clean
	make -C "${CONSOLE_DIR}" clean
}

build_hardware() {
	local rasppi="$1"
	log "Building hardware kernel (RASPPI=${rasppi}, AArch64, no QEMU extras)"
	clean_build_tree
	QEMU=0 RASPPI="${rasppi}" PREFIX64="${PREFIX64}" bash "${REPO_ROOT}/scripts/build.sh"
}

ensure_firmware() {
	local needed=(
		bootcode.bin start.elf start4.elf fixup.dat fixup4.dat
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

	log "Building Circle ARM stub for Raspberry Pi 4 / 400"
	make -C "${BOOT_DIR}" armstub64
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

package_rpi3() {
	local stage="${DIST}/sdcard-rpi3"
	local zip_name="mmbasic-console-rpi3-v${VERSION}.zip"
	local kernel="kernel8.img"
	rm -rf "${stage}"
	mkdir -p "${stage}"
	cp -a "${CONSOLE_DIR}/${kernel}" "${stage}/${kernel}"
	cp -a "${REPO_ROOT}/scripts/sdcard/config.txt" "${stage}/config.txt"
	cp -a "${REPO_ROOT}/scripts/sdcard/cmdline.txt" "${stage}/cmdline.txt"
	cp -a "${REPO_ROOT}/INSTALL.md" "${stage}/INSTALL.md"
	cp -a "${REPO_ROOT}/scripts/install-sdcard.sh" "${stage}/install-sdcard.sh"
	cp -a "${BOOT_DIR}/bootcode.bin" "${stage}/bootcode.bin"
	cp -a "${BOOT_DIR}/start.elf" "${stage}/start.elf"
	cp -a "${BOOT_DIR}/fixup.dat" "${stage}/fixup.dat"
	cp -a "${BOOT_DIR}/LICENCE.broadcom" "${stage}/LICENCE.broadcom"
	write_version "${stage}" "Raspberry Pi 3 / 3B+ / 3A+ (AArch64)" "${kernel}"
	zip_stage "${stage}" "${zip_name}"
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
	if [ -f "${BOOT_DIR}/COPYING.linux" ]; then
		cp -a "${BOOT_DIR}/COPYING.linux" "${stage}/COPYING.linux"
	fi
	write_version "${stage}" \
		"Raspberry Pi 400 (BCM2711; also Pi 4B / CM4) (AArch64)" \
		"${kernel}"
	zip_stage "${stage}" "${zip_name}"
}

ensure_firmware
build_hardware 3
package_rpi3
build_hardware 4
package_pi400

log "Restoring default QEMU Raspberry Pi 3 Circle config"
clean_build_tree
( cd "${CIRCLE_DIR}" && ./configure -r 3 -p "${PREFIX64}" --qemu -f )

log "Release artifacts"
ls -la "${DIST}"/mmbasic-console-*-v"${VERSION}".zip
