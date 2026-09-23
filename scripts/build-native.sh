#!/usr/bin/env bash
# Build the native (Linux/macOS) MMBasic binary: native/mmbasic.
#
# Generates the shared build artefacts (ramdisk/help/version) and compiles the
# portable interpreter plus the native platform backend. Any extra args are
# passed to make (e.g. `scripts/build-native.sh clean`, `... CC=gcc`).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

make -C "${REPO_ROOT}/native" "$@"

for bin in mmbasic mmcore; do
	if [ -x "${REPO_ROOT}/native/${bin}" ]; then
		printf '\n\033[1;34m==>\033[0m Native build: %s\n' "${REPO_ROOT}/native/${bin}"
	fi
done
