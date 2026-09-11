#!/usr/bin/env bash
# Write mmbasic/include/mmb_version.h from MMB_VERSION or git describe.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${REPO_ROOT}/mmbasic/include/mmb_version.h}"
VER="${MMB_VERSION:-}"

if [ -z "${VER}" ]; then
	VER="$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || true)"
fi
if [ -z "${VER}" ]; then
	VER="dev"
fi

tmp="${OUT}.tmp"
mkdir -p "$(dirname "${OUT}")"
{
	echo "#ifndef MMB_VERSION_H"
	echo "#define MMB_VERSION_H"
	printf '#define MMB_VERSION "%s"\n' "${VER}"
	echo "#endif"
} > "${tmp}"
if [ -f "${OUT}" ] && cmp -s "${tmp}" "${OUT}"; then
	rm -f "${tmp}"
else
	mv "${tmp}" "${OUT}"
fi
