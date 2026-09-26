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
  dist/mmcore-console-rpi3-vVERSION.zip
  dist/mmcore-console-pizero2-vVERSION.zip
  dist/mmcore-console-pizero2w-vVERSION.zip
  dist/mmcore-console-pi400-vVERSION.zip
  scripts/install-sdcard.sh

On macOS it also builds and attaches the universal (arm64 + x86_64) app bundle:
  dist/mmcore-macos-universal.zip   (scripts/package-macos-app.sh)
The bundle is notarized + stapled; a failed notarization aborts the release.

The Windows zip is built by CI (.github/workflows/windows.yml) when the release
is published and attached as:
  dist/mmcore-windows-x86_64.zip
Release-event builds require an Authenticode signature: the workflow sets
MMCORE_REQUIRE_WIN_SIGN=1 and supplies the WIN_SIGN_* signing secrets, so an
unsigned Windows zip cannot ship unnoticed.

Environment:
  SIGN_IDENTITY=...     codesign identity for the app bundle
  NOTARY_PROFILE=...    notarytool profile to notarize the app bundle
                        (default: mmcore-notary). The macOS asset is always
                        notarized + stapled and the release fails if that does
                        not succeed, so an unnotarized app cannot ship.
  MMCORE_SKIP_WIN_SIGN=1  local smoke tests: ship an unsigned Windows build
                          (never set this on a release)

Each zip also contains install-sdcard.sh so a consumer can:

  unzip mmcore-console-rpi3-vVERSION.zip
  sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX

  unzip mmcore-console-pizero2-vVERSION.zip
  sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX

  unzip mmcore-console-pizero2w-vVERSION.zip
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

# Push a tag to origin, falling back to the repo's HTTPS URL when the
# configured remote is an SSH alias the environment cannot resolve (for
# example `marnixk.github.com`). gh is already required by publish.
push_tag() {
	local tag="$1" url
	if git push origin "${tag}"; then
		return 0
	fi
	if ! command -v gh >/dev/null 2>&1; then
		die "git push origin ${tag} failed and gh is unavailable for a fallback"
	fi
	url="$(gh repo view --json url -q .url 2>/dev/null || true)"
	[ -n "${url}" ] || die "git push origin ${tag} failed and the repository URL is unknown"
	log "git push origin failed; retrying ${tag} via ${url}.git"
	if ! git push "${url}.git" "refs/tags/${tag}"; then
		die "could not push ${tag} to origin or ${url}.git"
	fi
}

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
	local macos_asset="${2:-}"
	local last tag_range
	last="$(last_version)"
	if [ "${last}" = "0.0.0" ]; then
		tag_range=""
	else
		tag_range="v${last}..HEAD"
	fi
	python3 - "${version}" "${last}" "${tag_range}" "${macos_asset}" <<'PY'
import os, subprocess, sys
version, last, tag_range, macos_asset = sys.argv[1:5]
macos_name = os.path.basename(macos_asset) if macos_asset else ""
print(f"mmcore v{version}")
print()
print("Bare-metal mmcore for Raspberry Pi. Each zip is a FAT-ready SD-card image")
print("plus `install-sdcard.sh` for Linux, and native desktop builds are attached:")
print("a Linux AppImage, a Windows x86_64 zip, and a universal macOS app."
      if macos_name else
      "a Linux AppImage and a Windows x86_64 zip.")
print()
print("## Install")
print()
print("```bash")
print(f"unzip mmcore-console-rpi3-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model rpi3 /dev/sdX   # Pi 3 / 3B+ / 3A+")
print()
print(f"unzip mmcore-console-pizero2-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pizero2 /dev/sdX  # Pi Zero 2")
print()
print(f"unzip mmcore-console-pizero2w-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pizero2w /dev/sdX # Pi Zero 2 W")
print()
print(f"unzip mmcore-console-pi400-v{version}.zip")
print("sudo ./install-sdcard.sh --bootstrap --model pi400 /dev/sdX  # Pi 400 / 4B / CM4")
print("```")
print()
print("`--update` refreshes kernel and firmware without wiping BASIC files on C:.")
print()
print("## Linux native (AppImage)")
print()
print("Run the desktop build on a Linux x86_64 host (SDL2 window; type at the prompt):")
print()
print("```bash")
print("chmod +x mmcore-x86_64.AppImage")
print("./mmcore-x86_64.AppImage")
print("```")
print()
print("## Windows native (x86_64)")
print()
print("Unzip and run `mmcore.exe` from Explorer or a terminal; keep the")
print("bundled `SDL2.dll` next to it (the SDL2 window shows output and keyboard")
print("input). The executable is Authenticode-signed for release builds.")
print()
print("```powershell")
print("Expand-Archive mmcore-windows-x86_64.zip .")
print(".\\mmcore-windows-x86_64\\mmcore.exe")
print("```")
print()
if macos_name:
    print("## macOS native (universal: Apple Silicon + Intel)")
    print()
    print("Unzip and drag `mmcore.app` to Applications, then launch it from Finder")
    print("(the SDL2 window shows the prompt and keyboard input):")
    print()
    print("```bash")
    print(f"unzip {macos_name}")
    print("open mmcore.app")
    print("```")
    print()
print("## Artifacts")
print()
print(f"- `mmcore-console-rpi3-v{version}.zip` — Raspberry Pi 3 / 3B+ / 3A+")
print(f"- `mmcore-console-pizero2-v{version}.zip` — Raspberry Pi Zero 2 (no onboard WLAN)")
print(f"- `mmcore-console-pizero2w-v{version}.zip` — Raspberry Pi Zero 2 W (CYW43436)")
print(f"- `mmcore-console-pi400-v{version}.zip` — Raspberry Pi 400 (also Pi 4B / CM4)")
print("- `install-sdcard.sh` — same installer, also inside each zip")
print("- `mmcore-x86_64.AppImage` — Linux native SDL2 desktop build")
print("- `mmcore-windows-x86_64.zip` — Windows x86_64 native SDL2 build")
if macos_name:
    print(f"- `{macos_name}` — macOS universal app bundle (mmcore.app)")
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
	local tag rpi3 pizero2 pizero2w pi400 installer notes macos
	local macos_arg=()
	version="$(normalize_version "${version}")"
	tag="v${version}"
	rpi3="${DIST}/mmcore-console-rpi3-v${version}.zip"
	pizero2="${DIST}/mmcore-console-pizero2-v${version}.zip"
	pizero2w="${DIST}/mmcore-console-pizero2w-v${version}.zip"
	pi400="${DIST}/mmcore-console-pi400-v${version}.zip"
	installer="${REPO_ROOT}/scripts/install-sdcard.sh"
	macos="${DIST}/mmcore-macos-universal.zip"

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

	if [ "$(uname -s)" = "Darwin" ]; then
		log "Building macOS app bundle for ${tag}"
		MMCORE_REQUIRE_NOTARY=1 VERSION="${version}" \
			bash "${REPO_ROOT}/scripts/package-macos-app.sh"
	fi
	if [ -f "${macos}" ]; then
		macos_arg=("${macos}")
	else
		log "No ${macos} — publishing without the macOS app"
	fi

	notes="$(release_notes "${version}" ${macos_arg[@]+"${macos_arg[@]}"})"
	log "Creating annotated tag ${tag}"
	git tag -a "${tag}" -m "mmcore ${tag}"
	log "Pushing ${tag}"
	push_tag "${tag}"

	log "Creating GitHub release ${tag}"
	gh release create "${tag}" \
		--title "mmcore ${tag}" \
		--notes "${notes}" \
		"${rpi3}" \
		"${pizero2}" \
		"${pizero2w}" \
		"${pi400}" \
		"${installer}" \
		${macos_arg[@]+"${macos_arg[@]}"}

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
		if [ -f "${DIST}/mmcore-macos-universal.zip" ]; then
			release_notes "$(normalize_version "$2")" "${DIST}/mmcore-macos-universal.zip"
		elif [ -f "${DIST}/mmcore-macos-arm64.zip" ]; then
			release_notes "$(normalize_version "$2")" "${DIST}/mmcore-macos-arm64.zip"
		else
			release_notes "$(normalize_version "$2")"
		fi
		;;
	*)
		die "unknown command: ${cmd} (see --help)"
		;;
esac
