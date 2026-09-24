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
# Circle sysinit halt()s when _end (text+data+BSS) exceeds KERNEL_MAX_SIZE.
# The .img file omits BSS, so a ~1.4MB kernel8.img can still sit at ~4MB
# in RAM (512KB TERM ring, wordpad, util). 4MB left almost no headroom
# once WLAN is linked; 8MB then 9MB (PAINT/TDF wave) are the configured caps.
# QEMU usb-net sits on the DWC2 root port (no hub); Circle's NAK and USB
# timing fixes keep CDC Ethernet from freezing or starving bulk IN.
# configure --qemu also sets NO_SCREEN_DMA_BURST_LENGTH (SetArea falls back
# to CPU memcpy). Hardware builds omit that flag so CBcmFrameBuffer::SetArea
# uses CDMAChannel::SetupMemCopy2D for HDMI presents (Pi 2–4 / 400).
QEMU_USB_DEFS=""
if [ "${QEMU:-1}" = "1" ]; then
  QEMU_USB_DEFS="-d USE_NAK_USB_FIX -d USE_QEMU_USB_FIX"
fi
( cd "${CIRCLE_DIR}" && ./configure -r "${RASPPI}" -p "${PREFIX64}" ${QEMU_FLAG} ${QEMU_USB_DEFS} --kernel-max-size 9 -f )

if grep -q 'mmbasic-issue-149' "${CIRCLE_DIR}/addon/wlan/ether4330.c" 2>/dev/null; then
	:
else
	log "Applying Circle Wi-Fi patches (#149)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-wifi-149.patch"
fi

if grep -q 'mmbasic-tcp-robust' "${CIRCLE_DIR}/lib/net/tcpconnection.cpp" 2>/dev/null; then
	:
else
	log "Applying Circle TCP receive patches (segment trimming, reassembly, close reason)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-tcp-robust.patch"
fi

if grep -q 'mmbasic-tcp-send' "${CIRCLE_DIR}/lib/net/netdevlayer.cpp" 2>/dev/null; then
	:
else
	log "Applying Circle TCP send-hole patches (defer failed frames, no SND.NXT skip)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-tcp-send.patch"
fi

if grep -q 'mmbasic-tcp-ack' "${CIRCLE_DIR}/lib/net/tcpconnection.cpp" 2>/dev/null; then
	:
else
	log "Applying Circle TCP ACK/window patches (1-byte ACK flush, partial ACK, RCV.WND)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-tcp-ack.patch"
fi

if grep -q 'mmbasic-fb-doublebuf' "${CIRCLE_DIR}/lib/screen.cpp" 2>/dev/null; then
	:
else
	log "Applying Circle FB double-buffer / draw-offset patches (PAGE DISPLAY flip)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-fb-doublebuf.patch"
fi

if grep -q 'mmbasic-usb-cdc-rx' "${CIRCLE_DIR}/lib/usb/usbcdcethernet.cpp" 2>/dev/null; then
	:
else
	log "Applying Circle USB CDC Ethernet RX/TX framing patches (#431)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-usb-cdc-rx.patch"
fi

if grep -q 'mmbasic-console-state' "${CIRCLE_DIR}/include/circle/terminal.h" 2>/dev/null; then
	:
else
	log "Applying Circle text-console snapshot/restore patches (virtual consoles)"
	patch -d "${CIRCLE_DIR}" -p1 --forward < "${REPO_ROOT}/patches/circle-console-state.patch"
fi

MODE_STAMP="${CONSOLE_DIR}/.circle-build-mode"
MODE="RASPPI=${RASPPI} QEMU=${QEMU:-1}"
if [ -f "${CIRCLE_DIR}/Config.mk" ]; then
	MODE="${MODE} $(tr '\n' ' ' < "${CIRCLE_DIR}/Config.mk")"
fi
if [ ! -f "${MODE_STAMP}" ] || [ "$(cat "${MODE_STAMP}")" != "${MODE}" ]; then
	log "Circle build mode changed; cleaning libraries so NO_SDHOST/RASPPI match"
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
	printf '%s\n' "${MODE}" > "${MODE_STAMP}"
fi

log "Building Circle core library"
make -C "${CIRCLE_DIR}/lib" -j"$(nproc)"

log "Building Circle SD card, FatFs, USB, sound, filesystem, net, and scheduler libraries"
make -C "${CIRCLE_DIR}/addon/SDCard" -j"$(nproc)"
make -C "${CIRCLE_DIR}/addon/fatfs" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/fs" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/input" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/usb" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/sound" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/sched" -j"$(nproc)"
make -C "${CIRCLE_DIR}/lib/net" -j"$(nproc)"

if [ "${QEMU:-1}" = "0" ]; then
  if [ ! -f "${CIRCLE_DIR}/addon/wlan/hostap/wpa_supplicant/Makefile.circle" ]; then
    log "Initialising Circle hostap submodule (WPA2 supplicant)"
    git -C "${CIRCLE_DIR}" submodule update --init addon/wlan/hostap
  fi
  log "Building Circle WLAN and hostap libraries"
  make -C "${CIRCLE_DIR}/addon/wlan" -j"$(nproc)"
  make -C "${CIRCLE_DIR}/addon/wlan/hostap/wpa_supplicant" -f Makefile.circle -j"$(nproc)"
fi

# wlan.o / net.o / eth.o change with MMB_CIRCLE_NET / MMB_CIRCLE_WLAN.
rm -f "${CONSOLE_DIR}/wlan.o" "${CONSOLE_DIR}/wlan.d" \
      "${CONSOLE_DIR}/net.o" "${CONSOLE_DIR}/net.d" \
      "${CONSOLE_DIR}/eth.o" "${CONSOLE_DIR}/eth.d"

log "Building console kernel image"
RAMDISK_EXCLUDE="${RAMDISK_EXCLUDE:-}"
if [ -n "${MMB_VERSION:-}" ]; then
  make -C "${CONSOLE_DIR}" -j"$(nproc)" MMB_VERSION="${MMB_VERSION}" RAMDISK_EXCLUDE="${RAMDISK_EXCLUDE}"
else
  make -C "${CONSOLE_DIR}" -j"$(nproc)" RAMDISK_EXCLUDE="${RAMDISK_EXCLUDE}"
fi

KERNEL_NAME="kernel8.img"
if [ "${RASPPI}" = "4" ]; then
  KERNEL_NAME="kernel8-rpi4.img"
elif [ "${RASPPI}" = "5" ]; then
  KERNEL_NAME="kernel_2712.img"
fi

log "Build complete: ${CONSOLE_DIR}/${KERNEL_NAME}"
ls -la "${CONSOLE_DIR}/${KERNEL_NAME}"
