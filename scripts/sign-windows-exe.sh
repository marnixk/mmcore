#!/usr/bin/env bash
# Authenticode-sign Windows executables for the build/release pipeline.
#
# Usage: sign-windows-exe.sh FILE [FILE...]
#
# A no-op unless a signing tool and certificate are configured, so a plain
# `scripts/package-windows.sh` still works on a developer machine with neither.
# Set MMCORE_REQUIRE_WIN_SIGN=1 for a release build: a missing tool, missing
# certificate, or failed verification is then fatal, mirroring the macOS
# MMCORE_REQUIRE_NOTARY gate. MMCORE_SKIP_WIN_SIGN=1 is the local escape hatch
# (smoke tests only) and conflicts with the release gate.
#
# Environment:
#   MMCORE_REQUIRE_WIN_SIGN=1  signing is mandatory; fail if it cannot be done
#   MMCORE_SKIP_WIN_SIGN=1     leave the file unsigned (local smoke tests)
#   SIGNTOOL                   signer executable to use (default: signtool,
#                              then osslsigncode, from PATH)
#   WIN_SIGN_TOOL              signer name to look up when SIGNTOOL is unset
#   WIN_SIGN_PFX               PKCS#12 (.pfx/.p12) certificate file
#   WIN_SIGN_PFX_BASE64        same certificate, base64-encoded (for CI secrets)
#   WIN_SIGN_PASSWORD          password for the PKCS#12 file
#   WIN_SIGN_THUMBPRINT        certificate SHA-1 thumbprint in the Windows store
#   WIN_SIGN_SUBJECT           certificate subject name in the Windows store
#   WIN_SIGN_TIMESTAMP         RFC 3161 timestamp server
#                              (default: http://timestamp.digicert.com)
#   WIN_SIGN_COMMAND           custom signing command, run with the file as $1
#                              (cloud HSM: DigiCert KeyLocker, Azure Trusted
#                              Signing); replaces signtool/osslsigncode
set -euo pipefail

die() {
	printf 'sign-windows-exe: %s\n' "$*" >&2
	exit 1
}
log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

[ $# -ge 1 ] || die "usage: sign-windows-exe.sh FILE [FILE...]"

MMCORE_REQUIRE_WIN_SIGN="${MMCORE_REQUIRE_WIN_SIGN:-}"
MMCORE_SKIP_WIN_SIGN="${MMCORE_SKIP_WIN_SIGN:-}"
WIN_SIGN_TIMESTAMP="${WIN_SIGN_TIMESTAMP:-http://timestamp.digicert.com}"

if [ "${MMCORE_REQUIRE_WIN_SIGN}" = "1" ] \
	&& [ "${MMCORE_SKIP_WIN_SIGN}" = "1" ]; then
	die "MMCORE_REQUIRE_WIN_SIGN=1 conflicts with MMCORE_SKIP_WIN_SIGN=1: cannot ship an unsigned Windows build"
fi

if [ "${MMCORE_SKIP_WIN_SIGN}" = "1" ]; then
	log "Skipping Authenticode signing (MMCORE_SKIP_WIN_SIGN=1)"
	exit 0
fi

# A base64 PKCS#12 (e.g. a GitHub Actions secret) is materialised to a temp
# file and removed on exit.
PFX_TMP=""
cleanup() {
	if [ -n "${PFX_TMP}" ]; then
		rm -f "${PFX_TMP}"
	fi
	return 0
}
trap cleanup EXIT
if [ -z "${WIN_SIGN_PFX:-}" ] && [ -n "${WIN_SIGN_PFX_BASE64:-}" ]; then
	PFX_TMP="$(mktemp "${TMPDIR:-/tmp}/mmcore-codesign.XXXXXX")"
	decode_ok=0
	if command -v base64 >/dev/null 2>&1; then
		printf '%s' "${WIN_SIGN_PFX_BASE64}" | base64 -d >"${PFX_TMP}" 2>/dev/null \
			&& decode_ok=1
		if [ "${decode_ok}" = "0" ]; then
			printf '%s' "${WIN_SIGN_PFX_BASE64}" | base64 -D >"${PFX_TMP}" 2>/dev/null \
				&& decode_ok=1
		fi
	fi
	if [ "${decode_ok}" = "0" ]; then
		printf '%s' "${WIN_SIGN_PFX_BASE64}" | python3 -c \
			'import base64, sys; sys.stdout.buffer.write(base64.b64decode(sys.stdin.read()))' \
			>"${PFX_TMP}" || die "could not decode WIN_SIGN_PFX_BASE64"
	fi
	WIN_SIGN_PFX="${PFX_TMP}"
fi

resolve_tool() {
	if [ -n "${SIGNTOOL:-}" ]; then
		if command -v "${SIGNTOOL}" >/dev/null 2>&1 || [ -x "${SIGNTOOL}" ]; then
			printf '%s\n' "${SIGNTOOL}"
			return 0
		fi
		die "SIGNTOOL=${SIGNTOOL} is not executable"
	fi
	if [ -n "${WIN_SIGN_TOOL:-}" ]; then
		command -v "${WIN_SIGN_TOOL}" 2>/dev/null && return 0
		return 1
	fi
	command -v signtool 2>/dev/null || command -v osslsigncode 2>/dev/null
}

TOOL="$(resolve_tool || true)"

cert_configured() {
	[ -n "${WIN_SIGN_COMMAND:-}" ] || [ -n "${WIN_SIGN_PFX:-}" ] \
		|| [ -n "${WIN_SIGN_THUMBPRINT:-}" ] || [ -n "${WIN_SIGN_SUBJECT:-}" ]
}

if [ -z "${TOOL}" ] && [ -z "${WIN_SIGN_COMMAND:-}" ]; then
	if [ "${MMCORE_REQUIRE_WIN_SIGN}" = "1" ]; then
		die "MMCORE_REQUIRE_WIN_SIGN=1 but no signing tool is available (install signtool or osslsigncode, or set SIGNTOOL/WIN_SIGN_COMMAND)"
	fi
	log "No Authenticode signing tool available; leaving executables unsigned"
	exit 0
fi

if ! cert_configured; then
	if [ "${MMCORE_REQUIRE_WIN_SIGN}" = "1" ]; then
		die "MMCORE_REQUIRE_WIN_SIGN=1 but no certificate is configured (set WIN_SIGN_PFX/WIN_SIGN_PFX_BASE64, WIN_SIGN_THUMBPRINT, WIN_SIGN_SUBJECT, or WIN_SIGN_COMMAND)"
	fi
	log "No code-signing certificate configured; leaving executables unsigned"
	exit 0
fi

case "$(basename "${TOOL:-}")" in
*signtool*) FLAVOR="signtool" ;;
osslsigncode*) FLAVOR="osslsigncode" ;;
*) FLAVOR="signtool" ;;
esac

sign_file() {
	local file="$1" args tmp
	[ -f "${file}" ] || die "not a file: ${file}"

	if [ -n "${WIN_SIGN_COMMAND:-}" ]; then
		log "Signing $(basename "${file}") via WIN_SIGN_COMMAND"
		bash -c "${WIN_SIGN_COMMAND}" _ "${file}" \
			|| die "WIN_SIGN_COMMAND failed for ${file}"
		return 0
	fi

	case "${FLAVOR}" in
	signtool)
		args=(sign /fd sha256 /tr "${WIN_SIGN_TIMESTAMP}" /td sha256)
		if [ -n "${WIN_SIGN_PFX:-}" ]; then
			args+=(/f "${WIN_SIGN_PFX}")
			[ -n "${WIN_SIGN_PASSWORD:-}" ] && args+=(/p "${WIN_SIGN_PASSWORD}")
		elif [ -n "${WIN_SIGN_THUMBPRINT:-}" ]; then
			args+=(/sha1 "${WIN_SIGN_THUMBPRINT}")
		else
			args+=(/n "${WIN_SIGN_SUBJECT}")
		fi
		log "Signing $(basename "${file}") with signtool"
		"${TOOL}" "${args[@]}" "${file}"
		;;
	osslsigncode)
		[ -n "${WIN_SIGN_PFX:-}" ] \
			|| die "osslsigncode signing needs WIN_SIGN_PFX (a PKCS#12 file)"
		tmp="${file}.signed"
		args=(sign -pkcs12 "${WIN_SIGN_PFX}")
		[ -n "${WIN_SIGN_PASSWORD:-}" ] && args+=(-pass "${WIN_SIGN_PASSWORD}")
		args+=(-h sha256 -t "${WIN_SIGN_TIMESTAMP}" -in "${file}" -out "${tmp}")
		log "Signing $(basename "${file}") with osslsigncode"
		"${TOOL}" "${args[@]}"
		mv -f "${tmp}" "${file}"
		;;
	esac
}

verify_file() {
	local file="$1"
	[ -n "${WIN_SIGN_COMMAND:-}" ] && return 0
	case "${FLAVOR}" in
	signtool)
		"${TOOL}" verify /pa /v "${file}" >/dev/null \
			|| die "signtool could not verify $(basename "${file}")"
		;;
	osslsigncode)
		"${TOOL}" verify -in "${file}" >/dev/null \
			|| die "osslsigncode could not verify $(basename "${file}")"
		;;
	esac
	log "Verified the Authenticode signature on $(basename "${file}")"
}

for file in "$@"; do
	sign_file "${file}"
	verify_file "${file}"
done
