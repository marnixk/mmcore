#!/usr/bin/env bash
# Build static OpenSSL for the macOS universal app slices (#1480).
#
# `scripts/package-macos-app.sh` links each slice against its own static
# `libssl.a` + `libcrypto.a`, so a universal bundle never depends on Homebrew
# OpenSSL (which only ships the host architecture). The libraries are built
# from the pinned OpenSSL source release and cached per architecture:
#
#   .cache/openssl-<version>/<arch>/include/openssl/...
#   .cache/openssl-<version>/<arch>/lib/libcrypto.a
#   .cache/openssl-<version>/<arch>/lib/libssl.a
#
# No developer-machine path ends up in the shipped binary: the libraries are
# static, and `--openssldir=/etc/ssl` points OpenSSL's default trust lookup at
# the macOS system CA file.
#
# Usage:
#   scripts/build-openssl-macos.sh [arch...]      # default: arm64 x86_64
#
# Environment:
#   OPENSSL_VERSION   OpenSSL release to build (default 3.5.4)
#   OPENSSL_CACHE     cache directory (default .cache/openssl-<version>)
#   JOBS              parallel make jobs (default: logical CPU count)
set -euo pipefail

case "$(uname -s)" in
	Darwin) ;;
	*)
		printf 'build-openssl-macos: macOS only (found %s)\n' "$(uname -s)" >&2
		exit 1
		;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPENSSL_VERSION="${OPENSSL_VERSION:-3.5.4}"
OPENSSL_CACHE="${OPENSSL_CACHE:-${REPO_ROOT}/.cache/openssl-${OPENSSL_VERSION}}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

ARCHES=("$@")
if [ "${#ARCHES[@]}" -eq 0 ]; then
	ARCHES=(arm64 x86_64)
fi

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'build-openssl-macos: %s\n' "$*" >&2
	exit 1
}

target_for_arch() {
	case "$1" in
	arm64) printf 'darwin64-arm64-cc' ;;
	x86_64) printf 'darwin64-x86_64-cc' ;;
	*) die "unsupported macOS architecture: $1" ;;
	esac
}

prefix_for_arch() { printf '%s/%s' "${OPENSSL_CACHE}" "$1"; }

tarball="${OPENSSL_CACHE}/openssl-${OPENSSL_VERSION}.tar.gz"

fetch_source() {
	local sha_file="${tarball}.sha256" expected actual
	local url="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}"

	mkdir -p "${OPENSSL_CACHE}"
	if [ ! -f "${tarball}" ]; then
		log "Fetching OpenSSL ${OPENSSL_VERSION}"
		curl -fSL --retry 3 -o "${tarball}.tmp" \
			"${url}/openssl-${OPENSSL_VERSION}.tar.gz"
		mv -f "${tarball}.tmp" "${tarball}"
	fi
	if [ ! -f "${sha_file}" ]; then
		curl -fSL --retry 3 -o "${sha_file}.tmp" \
			"${url}/openssl-${OPENSSL_VERSION}.tar.gz.sha256"
		mv -f "${sha_file}.tmp" "${sha_file}"
	fi
	expected="$(awk 'NR == 1 { print $1 }' "${sha_file}")"
	actual="$(shasum -a 256 "${tarball}" | awk '{ print $1 }')"
	[ -n "${expected}" ] || die "could not read SHA-256 from ${sha_file}"
	[ "${expected}" = "${actual}" ] \
		|| die "SHA-256 mismatch for ${tarball} (expected ${expected}, got ${actual})"
}

build_arch() {
	local arch="$1" prefix src target
	prefix="$(prefix_for_arch "${arch}")"
	if [ -f "${prefix}/lib/libssl.a" ] && [ -f "${prefix}/lib/libcrypto.a" ] \
		&& [ -f "${prefix}/include/openssl/ssl.h" ]; then
		log "Reusing static OpenSSL ${OPENSSL_VERSION} for ${arch} (${prefix})"
		return 0
	fi

	target="$(target_for_arch "${arch}")"
	src="${OPENSSL_CACHE}/src-${arch}"
	log "Building static OpenSSL ${OPENSSL_VERSION} for ${arch}"
	rm -rf "${src}"
	mkdir -p "${src}"
	tar -xzf "${tarball}" -C "${src}" --strip-components=1

	(
		cd "${src}"
		./Configure "${target}" --prefix=/usr/local --openssldir=/etc/ssl \
			no-shared no-tests no-apps no-docs \
			> "${OPENSSL_CACHE}/configure-${arch}.log" 2>&1 \
			|| { tail -40 "${OPENSSL_CACHE}/configure-${arch}.log"; exit 1; }
		make -j"${JOBS}" build_libs \
			> "${OPENSSL_CACHE}/build-${arch}.log" 2>&1 \
			|| { tail -60 "${OPENSSL_CACHE}/build-${arch}.log"; exit 1; }
	) || die "OpenSSL ${arch} build failed (see ${OPENSSL_CACHE}/build-${arch}.log)"

	mkdir -p "${prefix}/lib" "${prefix}/include"
	cp -f "${src}/libssl.a" "${src}/libcrypto.a" "${prefix}/lib/"
	rm -rf "${prefix}/include/openssl"
	cp -R "${src}/include/openssl" "${prefix}/include/"

	for lib in libssl.a libcrypto.a; do
		lipo -archs "${prefix}/lib/${lib}" | grep -qw "${arch}" \
			|| die "${lib} for ${arch} is not a ${arch} archive"
	done
	log "Static OpenSSL ${OPENSSL_VERSION} ready for ${arch}: ${prefix}"
}

fetch_source
for arch in "${ARCHES[@]}"; do
	build_arch "${arch}"
done
