#!/bin/sh
# Build native/mmcore-fb against Alpine's musl toolchain and install it into the
# ISO rootfs. Runs inside the ISO builder container.
#
# Set ROOTFS to the rootfs directory (default /tmp/mmcore-rootfs). If Alpine's
# SDL2 has no kmsdrm video driver, SDL2 is built from source with
# -DSDL_KMSDRM=ON and copied into the rootfs.
set -eu

REPO_ROOT="${REPO_ROOT:-/repo}"
ROOTFS="${ROOTFS:-/tmp/mmcore-rootfs}"
BUILD_DIR="${BUILD_DIR:-/tmp/mmcore-src}"
MMB_VERSION="${MMB_VERSION:-dev}"

log() { printf '\n==> %s\n' "$*"; }
die() {
	printf 'build-mmcore: %s\n' "$*" >&2
	exit 1
}

log "Installing mmcore build dependencies"
apk add --no-cache --quiet build-base pkgconf sdl2-dev python3 alsa-lib bash \
	>/dev/null

# The native build only needs these trees (no Circle/picomite submodules).
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
cp -a "${REPO_ROOT}/mmbasic" "${REPO_ROOT}/native" "${REPO_ROOT}/console" \
	"${REPO_ROOT}/ramdisk" "${REPO_ROOT}/assets" "${REPO_ROOT}/scripts" \
	"${BUILD_DIR}/"

SDL_CFLAGS="$(pkg-config --cflags sdl2)"
SDL_LIBS="$(pkg-config --libs sdl2)"

probe_dir="$(mktemp -d)"
cat > "${probe_dir}/probe.c" <<'EOF'
#include <SDL.h>
#include <string.h>
int main(void)
{
	int i, n = SDL_GetNumVideoDrivers();

	for (i = 0; i < n; i++)
		if (strcmp(SDL_GetVideoDriver(i), "kmsdrm") == 0)
			return 0;
	return 1;
}
EOF
if ! cc "${probe_dir}/probe.c" ${SDL_CFLAGS} ${SDL_LIBS} -o "${probe_dir}/probe" \
	|| ! "${probe_dir}/probe"; then
	log "Alpine SDL2 lacks kmsdrm; building SDL2 from source"
	apk add --no-cache --quiet cmake ninja samurai libdrm-dev mesa-dev \
		alsa-lib-dev eudev-dev linux-headers >/dev/null
	SDL_VER="${SDL_VER:-2.30.9}"
	PREFIX="/tmp/sdl2-prefix"
	rm -rf "/tmp/SDL2-${SDL_VER}" "${PREFIX}"
	cd /tmp
	wget -q "https://www.libsdl.org/release/SDL2-${SDL_VER}.tar.gz" -O sdl2.tar.gz
	tar xzf sdl2.tar.gz
	cmake -S "/tmp/SDL2-${SDL_VER}" -B "/tmp/SDL2-${SDL_VER}/build" \
		-G Ninja -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
		-DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON \
		-DSDL_KMSDRM=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF >/dev/null
	ninja -C "/tmp/SDL2-${SDL_VER}/build" >/dev/null
	ninja -C "/tmp/SDL2-${SDL_VER}/build" install >/dev/null
	SDL_CFLAGS="-I${PREFIX}/include/SDL2 -D_THREAD_SAFE"
	SDL_LIBS="-L${PREFIX}/lib -lSDL2"
	mkdir -p "${ROOTFS}/usr/lib"
	cp -a "${PREFIX}/lib"/libSDL2-2.0.so.0* "${ROOTFS}/usr/lib/" \
		2>/dev/null || true
fi

log "Building mmcore-fb (musl, ${MMB_VERSION})"
make -C "${BUILD_DIR}/native" sdl-fb \
	MMB_VERSION="v${MMB_VERSION#v}" SDL_CFLAGS="${SDL_CFLAGS}" SDL_LIBS="${SDL_LIBS}"

[ -x "${BUILD_DIR}/native/mmcore-fb" ] \
	|| die "mmcore-fb was not built"
mkdir -p "${ROOTFS}/usr/local/bin"
cp "${BUILD_DIR}/native/mmcore-fb" "${ROOTFS}/usr/local/bin/mmcore"
chmod 0755 "${ROOTFS}/usr/local/bin/mmcore"
log "Installed mmcore into ${ROOTFS}/usr/local/bin/mmcore"
