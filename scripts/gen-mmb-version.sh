#!/usr/bin/env bash
# Write console/mmb_version.h from MMB_VERSION or git describe.
#
# The console Makefile FORCEs this target and several objects depend on it, so
# parallel make can invoke the recipe more than once for one build. Write to a
# per-invocation temp file and rename atomically so no invocation can expose a
# missing/garbled header to a concurrent compiler (#897).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${REPO_ROOT}/console/mmb_version.h}"
VER="${MMB_VERSION:-}"

if [ -z "${VER}" ]; then
	VER="$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || true)"
fi
if [ -z "${VER}" ]; then
	VER="dev"
fi

mkdir -p "$(dirname "${OUT}")"
tmp="$(mktemp "${OUT}.XXXXXX")"
trap 'rm -f "${tmp}"' EXIT
{
	echo "#ifndef MMB_VERSION_H"
	echo "#define MMB_VERSION_H"
	printf '#define MMB_VERSION "%s"\n' "${VER}"
	echo "#endif"
} > "${tmp}"
if [ -f "${OUT}" ] && cmp -s "${tmp}" "${OUT}"; then
	:
else
	mv -f "${tmp}" "${OUT}"
fi
