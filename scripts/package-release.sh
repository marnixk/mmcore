#!/usr/bin/env bash
# Build a hardware Raspberry Pi 3 image and pack a FAT-ready SD card zip.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE_DIR="${REPO_ROOT}/circle"
CONSOLE_DIR="${REPO_ROOT}/console"
VERSION="${VERSION:-0.1.0}"
DIST="${REPO_ROOT}/dist"
STAGE="${DIST}/sdcard"
ZIP_NAME="mmbasic-console-rpi3-v${VERSION}.zip"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

log "Building hardware kernel (Raspberry Pi 3, AArch64, no QEMU extras)"
# Circle's Config.mk changed; force a rebuild so NO_SDHOST is not baked in.
make -C "${CIRCLE_DIR}/lib" clean
make -C "${CONSOLE_DIR}" clean
QEMU=0 RASPPI=3 bash "${REPO_ROOT}/scripts/build.sh"

log "Downloading Raspberry Pi firmware (Circle boot/)"
make -C "${REPO_ROOT}/circle/boot" firmware

rm -rf "${STAGE}"
mkdir -p "${STAGE}"

cp -a "${REPO_ROOT}/console/kernel8.img" "${STAGE}/kernel8.img"
cp -a "${REPO_ROOT}/scripts/sdcard/config.txt" "${STAGE}/config.txt"
cp -a "${REPO_ROOT}/INSTALL.md" "${STAGE}/INSTALL.md"
cp -a "${REPO_ROOT}/circle/boot/bootcode.bin" "${STAGE}/bootcode.bin"
cp -a "${REPO_ROOT}/circle/boot/start.elf" "${STAGE}/start.elf"
cp -a "${REPO_ROOT}/circle/boot/fixup.dat" "${STAGE}/fixup.dat"
cp -a "${REPO_ROOT}/circle/boot/LICENCE.broadcom" "${STAGE}/LICENCE.broadcom"

{
	echo "MMBasic console ${VERSION}"
	echo "Target: Raspberry Pi 3 / 3B+ / 3A+ (AArch64)"
	echo "kernel8.img $(wc -c < "${STAGE}/kernel8.img") bytes"
	echo "Built $(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${STAGE}/VERSION.txt"

mkdir -p "${DIST}"
rm -f "${DIST}/${ZIP_NAME}"
( cd "${STAGE}" && zip -9 -r "${DIST}/${ZIP_NAME}" . )

log "Release zip: ${DIST}/${ZIP_NAME}"
unzip -l "${DIST}/${ZIP_NAME}"
ls -la "${DIST}/${ZIP_NAME}" "${STAGE}"
