#!/usr/bin/env bash
# Package the native Windows build into a release zip.
#
# Output: dist/mmcore-windows-x86_64.zip
# Contains mmbasic-sdl.exe plus the SDL2 and MinGW runtime DLLs it needs, and a
# short README. Run scripts/build-windows.sh first (or let this call it).
#
# Environment:
#   CC          compiler (default x86_64-w64-mingw32-gcc)
#   SDL2_ROOT   SDL2 mingw install (include/, lib/, bin/); skips pkg-config
#   PKG_CONFIG  pkg-config command (default pkg-config)
#   SKIP_BUILD=1  do not (re)build the binary first
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${DIST:-${REPO_ROOT}/dist}"
CC="${CC:-x86_64-w64-mingw32-gcc}"
PKG_CONFIG="${PKG_CONFIG:-pkg-config}"
NAME="mmcore-windows-x86_64"
STAGE="${DIST}/${NAME}"
OUT="${DIST}/${NAME}.zip"
EXE="${REPO_ROOT}/native/mmbasic-sdl.exe"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'package-windows: %s\n' "$*" >&2
	exit 1
}

if [ "${SKIP_BUILD:-}" != "1" ]; then
	log "Building Windows binary"
	SDL2_ROOT="${SDL2_ROOT:-}" CC="${CC}" PKG_CONFIG="${PKG_CONFIG}" \
		bash "${REPO_ROOT}/scripts/build-windows.sh"
fi
[ -f "${EXE}" ] || die "${EXE} not built (run scripts/build-windows.sh)"

# SDL2.dll lives next to the SDL2 import library's parent bin/.
if [ -n "${SDL2_ROOT:-}" ]; then
	SDL2_DLL="${SDL2_ROOT}/bin/SDL2.dll"
else
	sdl_libdir="$("${PKG_CONFIG}" --variable=libdir sdl2 2>/dev/null || true)"
	[ -n "${sdl_libdir}" ] \
		|| die "cannot locate SDL2 (set SDL2_ROOT or install pkg-config sdl2)"
	SDL2_DLL="$(dirname "${sdl_libdir}")/bin/SDL2.dll"
fi
[ -f "${SDL2_DLL}" ] || die "SDL2.dll not found at ${SDL2_DLL}"

log "Staging ${STAGE}"
rm -rf "${STAGE}" "${OUT}"
mkdir -p "${STAGE}"
cp "${EXE}" "${STAGE}/mmbasic-sdl.exe"

# Copy every MinGW runtime DLL the executable and its DLLs depend on. System
# DLLs (kernel32, user32, ...) are not found on disk, so they are skipped;
# SDL2.dll is supplied from the SDL2 install.
OBJDUMP="${OBJDUMP:-$(command -v "${CC%-gcc}-objdump" || command -v objdump || true)}"

# Locate a runtime DLL. GCC's -print-prog-name works for a Homebrew cross
# toolchain, but MSYS2 returns the bare name, so also look beside the compiler
# and under the MINGW prefix.
find_dll() {
	local name="$1" p
	for p in \
		"$("${CC}" -print-prog-name="${name}" 2>/dev/null || true)" \
		"$(dirname "$(command -v "${CC}" 2>/dev/null || true)")/${name}" \
		"${MINGW_PREFIX:-}/bin/${name}" \
		"/mingw64/bin/${name}" \
		"/clang64/bin/${name}" \
		"$(command -v "${name}" 2>/dev/null || true)"; do
		[ -n "${p}" ] && [ -f "${p}" ] && {
			printf '%s\n' "${p}"
			return 0
		}
	done
	return 1
}

STAGED_DLLS=""
stage_dll() {
	local name="$1" path dep

	case " ${STAGED_DLLS} " in
	*" ${name} "*) return 0 ;;
	esac
	STAGED_DLLS="${STAGED_DLLS} ${name}"
	if [ "${name}" = "SDL2.dll" ]; then
		path="${SDL2_DLL}"
	else
		path="$(find_dll "${name}" || true)"
	fi
	[ -n "${path}" ] && [ -f "${path}" ] || return 0
	cp "${path}" "${STAGE}/${name}"
	if [ -n "${OBJDUMP}" ]; then
		while IFS= read -r dep; do
			[ -n "${dep}" ] && stage_dll "${dep}"
		done < <("${OBJDUMP}" -p "${path}" | sed -n 's/.*DLL Name: //p')
	fi
}

# Fail if the executable imports a MinGW DLL we could locate but did not stage:
# shipping without it would fail on a clean Windows machine.
verify_dlls_staged() {
	local pe="$1" dep p

	[ -n "${OBJDUMP}" ] || return 0
	while IFS= read -r dep; do
		[ -n "${dep}" ] || continue
		case " ${STAGED_DLLS} " in *" ${dep} "*) continue ;; esac
		if p="$(find_dll "${dep}")"; then
			die "unstaged runtime DLL ${dep} needed by $(basename "${pe}") (found ${p})"
		fi
	done < <("${OBJDUMP}" -p "${pe}" | sed -n 's/.*DLL Name: //p')
}

if [ -n "${OBJDUMP}" ]; then
	while IFS= read -r dep; do
		[ -n "${dep}" ] && stage_dll "${dep}"
	done < <("${OBJDUMP}" -p "${EXE}" | sed -n 's/.*DLL Name: //p')
else
	log "objdump not found; staging SDL2 and libwinpthread"
	stage_dll SDL2.dll
	stage_dll libwinpthread-1.dll
fi
verify_dlls_staged "${EXE}"
[ -f "${STAGE}/SDL2.dll" ] && verify_dlls_staged "${STAGE}/SDL2.dll"

cat > "${STAGE}/README.txt" <<'EOF'
mmcore for Windows (x86_64)
===========================

Colour Maximite 2 compatible BASIC interpreter. Run mmbasic-sdl.exe: a window
opens with the BASIC prompt. Type in the SDL window once it is focused.
Redirected stdin (pipes/files) is also read, for scripting.

Keep mmbasic-sdl.exe, SDL2.dll, and libwinpthread-1.dll in the same folder.

Usage
  mmbasic-sdl.exe                  interactive prompt
  mmbasic-sdl.exe "PRINT 6*7"      run one line and exit
  mmbasic-sdl.exe --drive DIR      mount the host directory DIR as D:
  mmbasic-sdl.exe game.app         run a packaged .app, then exit
  mmbasic-sdl.exe --help           full usage

The persistent C: drive lives under %USERPROFILE%\.mmbasic\C. Override the base
with the MMB_DRIVE_ROOT environment variable.
EOF

log "Packing ${OUT}"
( cd "${STAGE}" && zip -r -q "${OUT}" . )
[ -f "${OUT}" ] || die "zip failed"

log "Built ${OUT}"
ls -la "${OUT}"
