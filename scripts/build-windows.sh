#!/usr/bin/env bash
# Build the native Windows (x86_64) MMBasic binary with MinGW-w64.
#
# Produces native/mmbasic.exe (headless stdio) and native/mmbasic-sdl.exe.
# Run from MSYS2's MINGW64 shell on Windows, or with a MinGW cross compiler on
# macOS/Linux. SDL2 is found via pkg-config, or point SDL2_ROOT at an unpacked
# SDL2-devel-*-mingw tree.
#
# Environment:
#   CC          compiler (default x86_64-w64-mingw32-gcc)
#   SDL2_ROOT   SDL2 mingw install (include/, lib/, bin/); skips pkg-config
#   PKG_CONFIG  pkg-config command (default pkg-config)
# Any extra args are passed to make (e.g. `scripts/build-windows.sh clean`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CC="${CC:-x86_64-w64-mingw32-gcc}"
PKG_CONFIG="${PKG_CONFIG:-pkg-config}"

strip_sdl_exe_flags() {
	# SDL's mingw pkg-config declares SDL's WinMain shim (-lSDL2main and
	# -mwindows) and a main() macro; the native build supplies its own main().
	sed -E 's/ -lSDL2main//g; s/ -mwindows//g; s/ -Dmain=SDL_main//g'
}

if [ -n "${SDL2_ROOT:-}" ]; then
	SDL_CFLAGS="-I${SDL2_ROOT}/include -I${SDL2_ROOT}/include/SDL2"
	SDL_LIBS="-L${SDL2_ROOT}/lib -lSDL2"
else
	command -v "${PKG_CONFIG}" >/dev/null 2>&1 \
		|| { echo "build-windows: ${PKG_CONFIG} not found; set SDL2_ROOT" >&2; exit 1; }
	SDL_CFLAGS="$("${PKG_CONFIG}" --cflags sdl2 | strip_sdl_exe_flags)"
	SDL_LIBS="$("${PKG_CONFIG}" --libs sdl2 | strip_sdl_exe_flags)"
	if [ -z "${SDL_LIBS// /}" ]; then
		echo "build-windows: pkg-config could not find sdl2; set SDL2_ROOT" >&2
		exit 1
	fi
fi

make -C "${REPO_ROOT}/native" all TARGET_WINDOWS=1 CC="${CC}" \
	SDL_CFLAGS="${SDL_CFLAGS}" SDL_LIBS="${SDL_LIBS}" "$@"

for bin in mmbasic mmbasic-sdl; do
	if [ -f "${REPO_ROOT}/native/${bin}.exe" ]; then
		printf '\n\033[1;34m==>\033[0m Native build: %s\n' \
			"${REPO_ROOT}/native/${bin}.exe"
	fi
done
