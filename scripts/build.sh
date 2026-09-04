#!/usr/bin/env bash
# Build the bare-metal console (and the Circle core library it links against).
#
# Idempotent: safe to run repeatedly. Default produces console/kernel8.img,
# the Raspberry Pi 3 image exercised by the QEMU test harness. RASPPI=4
# produces console/kernel8-rpi4.img for Pi 4 / Pi 400.
#
# Target defaults to a 64-bit Raspberry Pi 3 built for QEMU, which is what the
# automated harness emulates. Override with RASPPI / QEMU env vars.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CIRCLE_DIR="${REPO_ROOT}/circle"
CONSOLE_DIR="${REPO_ROOT}/console"

RASPPI="${RASPPI:-3}"
PREFIX64="${PREFIX64:-aarch64-none-elf-}"
QEMU_FLAG="--qemu"
[ "${QEMU:-1}" = "0" ] && QEMU_FLAG=""

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

if [ ! -f "${CIRCLE_DIR}/Rules.mk" ]; then
  log "Initialising git submodules (circle, picomite-fork)"
  git -C "${REPO_ROOT}" submodule update --init
fi

log "Configuring Circle (RASPPI=${RASPPI}, AArch64, ${QEMU_FLAG:-hardware})"
( cd "${CIRCLE_DIR}" && ./configure -r "${RASPPI}" -p "${PREFIX64}" ${QEMU_FLAG} -f )

log "Building Circle core library"
make -C "${CIRCLE_DIR}/lib" -j"$(nproc)"

log "Building console kernel image"
make -C "${CONSOLE_DIR}" -j"$(nproc)"

KERNEL_NAME="kernel8.img"
if [ "${RASPPI}" = "4" ]; then
  KERNEL_NAME="kernel8-rpi4.img"
elif [ "${RASPPI}" = "5" ]; then
  KERNEL_NAME="kernel_2712.img"
fi

log "Build complete: ${CONSOLE_DIR}/${KERNEL_NAME}"
ls -la "${CONSOLE_DIR}/${KERNEL_NAME}"
