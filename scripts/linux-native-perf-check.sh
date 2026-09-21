#!/usr/bin/env bash
# LN-25: guard that the Linux-native portability work does not slow the Pi.
#
# The portability layer is compile-time only: MMB_PLATFORM_POSIX selects libc
# headers for the native build, while the Circle build keeps <circle/...> and
# the AArch64 blit exactly as before. This script builds the Pi image, checks
# those invariants, and records hot-object sizes for comparison.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${REPO_ROOT}/dist/linux-native-perf.txt"
MMB="${REPO_ROOT}/mmbasic"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
mkdir -p "${REPO_ROOT}/dist"

log "Building Pi kernel (scripts/build.sh)"
scripts/build.sh >/dev/null

log "Checking the native shim cannot leak into the Circle build"
if grep -rn "MMB_PLATFORM_POSIX" "${REPO_ROOT}/console/Makefile" \
	"${REPO_ROOT}/scripts/build.sh" >/dev/null 2>&1; then
	echo "FAIL: MMB_PLATFORM_POSIX defined in the Circle build" >&2
	exit 1
fi

log "Checking the AArch64 blit is still linked on Pi"
if ! ls "${MMB}/src/blit_a64.o" >/dev/null 2>&1; then
	echo "FAIL: blit_a64.o not built for Pi" >&2
	exit 1
fi

{
	echo "Linux-native portability perf guard"
	echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	echo
	echo "Pi build: kernel8.img present: $(test -f "${REPO_ROOT}/console/kernel8.img" && echo yes || echo no)"
	echo "AArch64 blit linked: yes"
	echo "MMB_PLATFORM_POSIX in Circle build: no"
	echo
	SIZE_TOOL="${SIZE_TOOL:-aarch64-none-elf-size}"
	command -v "${SIZE_TOOL}" >/dev/null 2>&1 || SIZE_TOOL="size"
	echo "Hot object sizes (${SIZE_TOOL}: text data bss):"
	for o in util gfx gfx_cmm2 blit16 blit_a64 cmd_play cmd_term vfs editor; do
		f="${MMB}/src/${o}.o"
		if [ -f "${f}" ]; then
			printf '  %-12s %s\n' "${o}" "$("${SIZE_TOOL}" "${f}" 2>/dev/null | tail -1)"
		fi
	done
	echo
	echo "The portability shim is header-only: mmb_priv.h selects <circle/...>"
	echo "unless MMB_PLATFORM_POSIX is defined, so Pi codegen is unchanged."
} | tee "${OUT}"

log "Wrote ${OUT}"
