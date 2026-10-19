#!/usr/bin/env bash
# Build a bootable Alpine Linux live ISO for mmcore (x86_64).
#
# The build runs inside an amd64 alpine container so the host only needs
# Docker. On a Linux x86_64 machine with apk, set ISO_DIRECT=1 to run the
# builder in place. Output: dist/mmcore-fb-x86_64.iso (hybrid BIOS+UEFI,
# dd-able to a USB stick).
#
# Environment:
#   VERSION        clean release version (e.g. "0.215.0"); default dev
#   DIST           output directory (default <repo>/dist)
#   ALPINE_IMAGE   builder image (default alpine:3.20)
#   ISO_ARCH       target architecture (default x86_64)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${DIST:-${REPO_ROOT}/dist}"
ALPINE_IMAGE="${ALPINE_IMAGE:-alpine:3.20}"
OUT_NAME="mmcore-fb-${ISO_ARCH:-x86_64}.iso"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'build-iso: %s\n' "$*" >&2
	exit 1
}

# Resolve DIST to an absolute path: docker -v requires one, and the container
# gets it mounted at a fixed path (/iso-out) so a custom DIST is honoured.
mkdir -p "${DIST}"
DIST="$(cd "${DIST}" && pwd)"
OUT="${DIST}/${OUT_NAME}"
rm -f "${OUT}"

if [ "${ISO_DIRECT:-0}" = "1" ]; then
	log "Building the ISO directly (ISO_DIRECT=1)"
	REPO_ROOT="${REPO_ROOT}" MMCORE_VERSION="${VERSION:-dev}" \
		ISO_ARCH="${ISO_ARCH:-x86_64}" ISO_OUT="${OUT}" \
		bash "${REPO_ROOT}/scripts/iso/build-in-container.sh"
	[ -f "${OUT}" ] || die "the ISO was not produced"
	log "Built ${OUT}"
	exit 0
fi

command -v docker >/dev/null 2>&1 \
	|| die "docker is required (or set ISO_DIRECT=1 on an Alpine Linux host)"

log "Building ${OUT_NAME} in ${ALPINE_IMAGE} (linux/amd64)"
docker run --rm \
	--platform "linux/${ISO_ARCH:-x86_64}" \
	-e "MMCORE_VERSION=${VERSION:-dev}" \
	-e "ISO_ARCH=${ISO_ARCH:-x86_64}" \
	-e "ISO_OUT=/iso-out/${OUT_NAME}" \
	-v "${REPO_ROOT}:/repo" \
	-v "${DIST}:/iso-out" \
	-w /repo \
	"${ALPINE_IMAGE}" \
	/repo/scripts/iso/build-in-container.sh

[ -f "${OUT}" ] || die "the ISO was not produced"
log "Built ${OUT}"
ls -la "${OUT}"
