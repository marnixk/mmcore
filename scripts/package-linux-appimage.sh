#!/usr/bin/env bash
# Build the native Linux SDL binary and package it as an AppImage.
#
# Output: dist/MMBasic-<version>-<arch>.AppImage
# Requires: a C toolchain, pkg-config, libsdl2-dev, python3, curl/wget.
# linuxdeploy + appimagetool are downloaded into dist/.appimage-tools (and run
# with APPIMAGE_EXTRACT_AND_RUN so no FUSE is needed).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${REPO_ROOT}/dist"
APPDIR="${DIST}/MMBasic.AppDir"
TOOLS="${DIST}/.appimage-tools"
ARCH="${APPIMAGE_ARCH:-$(uname -m)}"
VERSION="$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || echo dev)"
OUT="${DIST}/MMBasic-${VERSION}-${ARCH}.AppImage"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

log "Building native SDL binary"
"${REPO_ROOT}/scripts/build-linux.sh"
BIN="${REPO_ROOT}/linux/mmbasic-sdl"
[ -x "${BIN}" ] || { echo "error: ${BIN} not built (SDL2 dev headers missing?)" >&2; exit 1; }

log "Staging AppDir"
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/usr/bin" \
	 "${APPDIR}/usr/share/applications" \
	 "${APPDIR}/usr/share/icons/hicolor/256x256/apps"
cp "${BIN}" "${APPDIR}/usr/bin/mmbasic-sdl"

python3 "${REPO_ROOT}/scripts/gen-appicon.py" \
	"${APPDIR}/usr/share/icons/hicolor/256x256/apps/mmbasic.png"
cp "${APPDIR}/usr/share/icons/hicolor/256x256/apps/mmbasic.png" \
	"${APPDIR}/mmbasic.png"

cat > "${APPDIR}/usr/share/applications/mmbasic.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=MMBasic
Comment=Colour Maximite 2 compatible BASIC interpreter
Exec=mmbasic-sdl
Icon=mmbasic
Categories=Development;Education;
Terminal=true
EOF
cp "${APPDIR}/usr/share/applications/mmbasic.desktop" "${APPDIR}/mmbasic.desktop"

mkdir -p "${TOOLS}"
fetch() {
	local url="$1" out="$2"
	if [ ! -f "${out}" ]; then
		command -v curl >/dev/null && curl -fL --retry 3 -o "${out}" "${url}" \
			|| wget -q -O "${out}" "${url}"
	fi
	chmod +x "${out}"
}

LINUXDEPLOY="${TOOLS}/linuxdeploy-${ARCH}.AppImage"
APPIMAGETOOL="${TOOLS}/appimagetool-${ARCH}.AppImage"
fetch "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage" "${LINUXDEPLOY}"
fetch "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-${ARCH}.AppImage" "${APPIMAGETOOL}"

export APPIMAGE_EXTRACT_AND_RUN=1
export ARCH

log "Bundling libraries (linuxdeploy)"
"${LINUXDEPLOY}" --appdir "${APPDIR}" \
	--executable "${APPDIR}/usr/bin/mmbasic-sdl" \
	--desktop-file "${APPDIR}/usr/share/applications/mmbasic.desktop" \
	--icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/mmbasic.png"

log "Packing AppImage"
"${APPIMAGETOOL}" "${APPDIR}" "${OUT}"

log "Built ${OUT}"
ls -la "${OUT}"
