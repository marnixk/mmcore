#!/usr/bin/env bash
# Select the test suites related to the changes since the previous release.
#
# Thin wrapper around scripts/release_tests.py that prefers the project venv
# (which has pytest + pytest-xdist) over the system python. See
# `scripts/release_tests.py --help` and the github-release skill.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# A git worktree can share the main clone's venv; look there too so a worker
# worktree with no local .venv still gets pytest + pytest-xdist.
COMMON_DIR="$(git -C "${REPO_ROOT}" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
MAIN_ROOT=""
if [ -n "${COMMON_DIR}" ]; then
	MAIN_ROOT="$(cd "${COMMON_DIR}/.." 2>/dev/null && pwd || true)"
fi

if [ -z "${PY:-}" ]; then
	for candidate in \
		"${REPO_ROOT}/.venv/bin/python" \
		"${MAIN_ROOT}/.venv/bin/python"; do
		if [ -n "${candidate}" ] && [ -x "${candidate}" ]; then
			PY="${candidate}"
			break
		fi
	done
fi
PY="${PY:-python3}"

exec "${PY}" "${REPO_ROOT}/scripts/release_tests.py" "$@"
