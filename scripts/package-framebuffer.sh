#!/usr/bin/env bash
# Package the native KMS/DRM framebuffer build into a release tarball.
#
# Output: dist/mmcore-fb-linux-x86_64.tar.gz
# Contains the mmcore-fb binary, a short README with the runtime dependencies,
# and VERSION.txt. Linux x86_64 only: the framebuffer backend is DRM/KMS.
#
# Environment:
#   VERSION      clean release version baked into the binary (e.g. "0.215.0");
#                unset falls back to git describe for dev builds.
#   DIST         output directory (default <repo>/dist)
#   SKIP_BUILD=1 do not (re)build the binary first
set -euo pipefail

case "$(uname -s)" in
	Linux) ;;
	*)
		printf 'package-framebuffer: the KMS/DRM build is Linux-only (found %s)\n' \
			"$(uname -s)" >&2
		exit 1
		;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${DIST:-${REPO_ROOT}/dist}"
NAME="mmcore-fb-linux-x86_64"
STAGE="${DIST}/${NAME}"
OUT="${DIST}/${NAME}.tar.gz"
BIN="${REPO_ROOT}/native/mmcore-fb"

if [ -n "${VERSION:-}" ]; then
	MMB_VERSION="v${VERSION#v}"
else
	MMB_VERSION="$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || echo dev)"
fi

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'package-framebuffer: %s\n' "$*" >&2
	exit 1
}

if [ "${SKIP_BUILD:-}" != "1" ]; then
	log "Building framebuffer binary (${MMB_VERSION})"
	"${REPO_ROOT}/scripts/build-native.sh" sdl-fb MMB_VERSION="${MMB_VERSION}"
fi
[ -x "${BIN}" ] || die "${BIN} not built (SDL2 dev headers missing?)"

log "Staging ${STAGE}"
rm -rf "${STAGE}" "${OUT}"
mkdir -p "${STAGE}"
cp "${BIN}" "${STAGE}/mmcore-fb"

cat > "${STAGE}/README.md" <<'EOF'
# mmcore-fb — Linux framebuffer (KMS/DRM) build

`mmcore-fb` is the mmcore interpreter rendering straight to the Linux
framebuffer over DRM/KMS, with no X11 or Wayland. Run it from a text virtual
terminal (VT), not from inside a desktop session.

## Runtime dependencies

- SDL2 built with the `kmsdrm` video driver (`libsdl2-2.0-0` on Debian/Ubuntu
  ships it)
- `libdrm2` and `libgbm1`
- ALSA (`libasound2`) for `PLAY`/audio
- a DRM/KMS-capable kernel driver for your GPU

On Debian/Ubuntu:

    sudo apt-get install libsdl2-2.0-0 libdrm2 libgbm1 libasound2

## Run

    chmod +x mmcore-fb
    ./mmcore-fb

It opens fullscreen on the primary display. `SDL_VIDEODRIVER` overrides the
built-in `kmsdrm` default (for example `SDL_VIDEODRIVER=dummy` to run headless
in a test). There is no window manager, so the system cursor is hidden;
PAINT's tool cursor and the `MOUSE ON` software cursor are the pointer.
EOF

{
	echo "mmcore framebuffer build $(basename "${NAME}")"
	printf 'Version: %s\n' "${MMB_VERSION}"
	printf 'Built: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "${STAGE}/VERSION.txt"

log "Packing ${OUT}"
tar -C "${DIST}" -czf "${OUT}" "${NAME}"
ls -la "${OUT}"
