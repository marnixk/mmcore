#!/usr/bin/env bash
# Look up, bump, build, and publish semantic GitHub releases for this product.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${REPO_ROOT}/dist"

usage() {
	cat <<'EOF'
Usage: github-release.sh <command> [version]

Commands:
  last-version     Print the latest published semantic version (no v prefix)
  next-minor       Print the default next version (minor bump of last-version)
  next-patch       Print last-version with the patch number incremented
  next-major       Print last-version with the major number incremented
  publish VERSION  Build hardware zips and create GitHub release vVERSION
  release-notes VERSION
                   Print the GitHub release notes for VERSION (no build)

publish builds board images with scripts/package-release.sh, then uploads:
  dist/mmbasic-console-rpi3-vVERSION.zip
  dist/mmbasic-console-pizero2-vVERSION.zip
  dist/mmbasic-console-pizero2w-vVERSION.zip
  dist/mmbasic-console-pi400-vVERSION.zip
  scripts/install-sdcard.sh

Each zip also contains install-sdcard.sh so a consumer can:

  unzip mmbasic-console-rpi3-vVERSION.zip
  sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX

  unzip mmbasic-console-pizero2-vVERSION.zip
  sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX

  unzip mmbasic-console-pizero2w-vVERSION.zip
  sudo ./install-sdcard.sh --bootstrap --model pizero2w /dev/sdX

Examples:
  scripts/github-release.sh last-version
  scripts/github-release.sh next-minor
  scripts/github-release.sh publish 0.2.0
EOF
}

die() {
	printf 'github-release: %s\n' "$*" >&2
	exit 1
}

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

normalize_version() {
	local raw="${1:-}"
	raw="${raw#v}"
	printf '%s' "${raw}" | python3 -c '
import re, sys
s = sys.stdin.read().strip()
if not re.fullmatch(r"\d+\.\d+\.\d+", s):
    sys.stderr.write("not a semantic version X.Y.Z: %r\n" % s)
    sys.exit(1)
print(s)
'
}

bump_version() {
	local kind="$1"
	local current="$2"
	python3 - "$kind" "$current" <<'PY'
import sys
kind, current = sys.argv[1], sys.argv[2]
maj, minr, pat = (int(x) for x in current.split("."))
if kind == "major":
    maj, minr, pat = maj + 1, 0, 0
elif kind == "minor":
    minr, pat = minr + 1, 0
elif kind == "patch":
    pat += 1
else:
    raise SystemExit(f"unknown bump {kind}")
print(f"{maj}.{minr}.{pat}")
PY
}

collect_tag_versions() {
	python3 - <<'PY'
import json, os, re, subprocess, sys

semver = re.compile(r"^v(\d+\.\d+\.\d+)$")
tags = []

def add(tag):
    m = semver.match(tag.strip())
    if m:
        tags.append(m.group(1))

try:
    raw = subprocess.check_output(
        ["gh", "release", "list", "--limit", "50", "--json", "tagName"],
        text=True,
        stderr=subprocess.DEVNULL,
    )
    for item in json.loads(raw):
        add(item.get("tagName") or "")
except (subprocess.CalledProcessError, json.JSONDecodeError, FileNotFoundError):
    pass

try:
    raw = subprocess.check_output(
        ["git", "tag", "-l", "v*.*.*"],
        text=True,
        stderr=subprocess.DEVNULL,
    )
    for line in raw.splitlines():
        add(line)
except subprocess.CalledProcessError:
    pass

if not tags:
    sys.exit(0)

def key(v):
    return tuple(int(x) for x in v.split("."))

print(sorted(set(tags), key=key)[-1])
PY
}

last_version() {
	local v
	v="$(collect_tag_versions || true)"
	if [ -z "${v}" ]; then
		printf '0.0.0\n'
		return 0
	fi
	printf '%s\n' "${v}"
}

release_notes() {
	local version="$1"
	local last tag_range
	last="$(last_version)"
	if [ "${last}" = "0.0.0" ]; then
		tag_range=""
	else
		tag_range="v${last}..HEAD"
	fi
	python3 - "${version}" "${last}" "${tag_range}" <<'PY'
import subprocess, sys
version, last, tag_range = sys.argv[1:4]
print(f"MMBasic console v{version}")
print()
print("Bare-metal MMBasic for Raspberry Pi. Each zip is a FAT-ready SD-card image")
print("plus `install-sdcard.sh` for Linux.")
print()
print("## Install")
print()
print("```bash")
print(f"unzip mmbasic-console-rpi3-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX   # Pi 3 / 3B+ / 3A+")
print()
print(f"unzip mmbasic-console-pizero2-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX  # Pi Zero 2")
print()
print(f"unzip mmbasic-console-pizero2w-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pizero2w /dev/sdX # Pi Zero 2 W")
print()
print(f"unzip mmbasic-console-pi400-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pi400 /dev/sdX  # Pi 400 / 4B / CM4")
print("```")
print()
print("`--update` refreshes kernel and firmware without wiping BASIC files on C:.")
print()
print("## Artifacts")
print()
print(f"- `mmbasic-console-rpi3-v{version}.zip` — Raspberry Pi 3 / 3B+ / 3A+")
print(f"- `mmbasic-console-pizero2-v{version}.zip` — Raspberry Pi Zero 2 (no onboard WLAN)")
print(f"- `mmbasic-console-pizero2w-v{version}.zip` — Raspberry Pi Zero 2 W (CYW43436)")
print(f"- `mmbasic-console-pi400-v{version}.zip` — Raspberry Pi 400 (also Pi 4B / CM4)")
print("- `install-sdcard.sh` — same installer, also inside each zip")
print()
if tag_range:
    print(f"## Changes since v{last}")
    print()
    log = subprocess.check_output(
        ["git", "log", "--pretty=format:- %s", tag_range],
        text=True,
    ).strip()
    print(log or "- (no extra commits)")
else:
    print("## Changes")
    print()
    print("- Initial release.")
PY
}

assert_zip_has_installer() {
	local zip="$1" listing
	# Capture first: a `unzip | grep -q` pipeline can SIGPIPE unzip under
	# `set -o pipefail` and fail the assertion for a zip that is fine.
	listing="$(unzip -Z1 "${zip}")" || die "${zip} is not readable"
	grep -Fxq 'install-sdcard.sh' <<<"${listing}" \
		|| die "${zip} is missing install-sdcard.sh"
}

publish() {
	local version="$1"
	local tag rpi3 pizero2 pizero2w pi400 installer notes
	version="$(normalize_version "${version}")"
	tag="v${version}"
	rpi3="${DIST}/mmbasic-console-rpi3-v${version}.zip"
	pizero2="${DIST}/mmbasic-console-pizero2-v${version}.zip"
	pizero2w="${DIST}/mmbasic-console-pizero2w-v${version}.zip"
	pi400="${DIST}/mmbasic-console-pi400-v${version}.zip"
	installer="${REPO_ROOT}/scripts/install-sdcard.sh"

	[ -x "${installer}" ] || die "missing ${installer}"
	command -v gh >/dev/null 2>&1 || die "gh is not on PATH"
	command -v git >/dev/null 2>&1 || die "git is not on PATH"

	if gh release view "${tag}" >/dev/null 2>&1; then
		die "GitHub release ${tag} already exists"
	fi
	if git rev-parse "${tag}" >/dev/null 2>&1; then
		die "git tag ${tag} already exists"
	fi

	log "Building hardware zips for ${tag}"
	VERSION="${version}" bash "${REPO_ROOT}/scripts/package-release.sh"
	[ -f "${rpi3}" ] || die "missing ${rpi3}"
	[ -f "${pizero2}" ] || die "missing ${pizero2}"
	[ -f "${pizero2w}" ] || die "missing ${pizero2w}"
	[ -f "${pi400}" ] || die "missing ${pi400}"
	assert_zip_has_installer "${rpi3}"
	assert_zip_has_installer "${pizero2}"
	assert_zip_has_installer "${pizero2w}"
	assert_zip_has_installer "${pi400}"

	notes="$(release_notes "${version}")"
	log "Creating annotated tag ${tag}"
	git tag -a "${tag}" -m "MMBasic console ${tag}"
	log "Pushing ${tag}"
	git push origin "${tag}"

	log "Creating GitHub release ${tag}"
	gh release create "${tag}" \
		--title "MMBasic console ${tag}" \
		--notes "${notes}" \
		"${rpi3}" \
		"${pizero2}" \
		"${pizero2w}" \
		"${pi400}" \
		"${installer}"

	log "Published ${tag}"
	gh release view "${tag}"
}

cmd="${1:-}"
case "${cmd}" in
	-h|--help)
		usage
		exit 0
		;;
	"")
		usage >&2
		exit 2
		;;
	last-version)
		last_version
		;;
	next-minor)
		bump_version minor "$(last_version)"
		;;
	next-patch)
		bump_version patch "$(last_version)"
		;;
	next-major)
		bump_version major "$(last_version)"
		;;
	publish)
		[ $# -eq 2 ] || die "publish needs VERSION (see --help)"
		publish "$2"
		;;
	release-notes)
		[ $# -eq 2 ] || die "release-notes needs VERSION (see --help)"
		release_notes "$(normalize_version "$2")"
		;;
	*)
		die "unknown command: ${cmd} (see --help)"
		;;
esac
