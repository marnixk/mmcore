#!/usr/bin/env bash
# Build the native (Linux/macOS) MMBasic binary: linux/mmbasic.
#
# Generates the shared build artefacts (ramdisk/help/version) and compiles the
# portable interpreter plus the native platform backend. Any extra args are
# passed to make (e.g. `scripts/build-linux.sh clean`, `... CC=gcc`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

make -C "${REPO_ROOT}/linux" "$@"

for bin in mmbasic mmbasic-sdl; do
	if [ -x "${REPO_ROOT}/linux/${bin}" ]; then
		printf '\n\033[1;34m==>\033[0m Native build: %s\n' "${REPO_ROOT}/linux/${bin}"
	fi
done
