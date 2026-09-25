#!/usr/bin/env bash
# Build the native SDL binary and package it as a self-contained, signed
# macOS `.app` bundle. By default the bundle is universal (arm64 + x86_64):
# each slice is compiled with -arch and the two executables are lipo'd, so the
# app runs natively on Apple Silicon and Intel Macs alike.
#
# Output: dist/mmcore.app and dist/mmcore-macos-universal.zip
# Requires: macOS with Xcode command line tools and python3. SDL2 is downloaded
# (universal) from the official libsdl.org release and cached under .cache/, so
# Homebrew SDL2 is not needed.
#
# The bundle is portable: SDL2 (the only non-system dependency) is copied into
# Contents/Frameworks and the executable's install names are rewritten to load
# it from `@rpath`.
#
# Environment:
#   MACOS_ARCHES        architectures to build/lipo (default "arm64 x86_64").
#                       Set e.g. MACOS_ARCHES=arm64 for a single-slice build.
#   SDL2_VERSION        official SDL2 release to fetch (default 2.32.6).
#   SDL2_CACHE          download/extract cache dir (default .cache/sdl2-<ver>).
#   SIGN_IDENTITY       codesign identity. Defaults to a "Developer ID
#                       Application" certificate for Marnix Kok, else any
#                       "Apple Development" one, else ad-hoc ("-").
#   NOTARY_PROFILE      `notarytool` keychain profile (see
#                       `xcrun notarytool store-credentials`). When set, the
#                       bundle is notarized and stapled; requires a Developer ID
#                       identity. Defaults to "mmcore-notary" for release
#                       builds (MMCORE_REQUIRE_NOTARY=1).
#   NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD
#                       Notarize with App Store Connect credentials directly
#                       instead of a keychain profile. Useful in non-interactive
#                       sessions where notarytool cannot read the keychain
#                       ("User interaction is not allowed"). When all three are
#                       set they take precedence over NOTARY_PROFILE.
#   MMCORE_REQUIRE_NOTARY=1
#                       Release build: notarization is mandatory. The bundle is
#                       notarized and stapled, and the script fails if either
#                       step (or validation) does not succeed, so an
#                       unnotarized asset cannot ship unnoticed.
#   MMCORE_SKIP_SIGN=1  skip the real identity and ad-hoc sign the bundle
#                       (local smoke tests only; not distributable).
#   VERSION             override the bundle version (default: git describe).
#
# Signing locally with an "Apple Development" certificate is fine for this Mac;
# distributing to others needs a Developer ID + notarization to avoid Gatekeeper
# warnings.
set -euo pipefail

case "$(uname -s)" in
	Darwin) ;;
	*)
		printf 'package-macos-app: macOS only (found %s)\n' "$(uname -s)" >&2
		exit 1
		;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${DIST:-${REPO_ROOT}/dist}"
BIN="${REPO_ROOT}/native/mmcore"
APP_NAME="mmcore"
APP="${DIST}/${APP_NAME}.app"
EXE_NAME="mmcore"
BUNDLE_ID="com.marnixk.mmcore"
MACOS_ARCHES="${MACOS_ARCHES:-arm64 x86_64}"
SDL2_VERSION="${SDL2_VERSION:-2.32.6}"
SDL2_CACHE="${SDL2_CACHE:-${REPO_ROOT}/.cache/sdl2-${SDL2_VERSION}}"
SDL2_STAGE="${SDL2_CACHE}/libSDL2-2.0.0.dylib"
SDL2_INCLUDE="${SDL2_CACHE}/include"

# "universal" when more than one slice (the default), else the arch name, so
# the asset name matches what is actually inside the zip.
if [ "$(printf '%s\n' ${MACOS_ARCHES} | wc -l | tr -d ' ')" -gt 1 ]; then
	MACOS_ARCH_LABEL="${MACOS_ARCH_LABEL:-universal}"
else
	MACOS_ARCH_LABEL="${MACOS_ARCH_LABEL:-$(printf '%s' ${MACOS_ARCHES})}"
fi
OUT="${DIST}/${APP_NAME}-macos-${MACOS_ARCH_LABEL}.zip"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'package-macos-app: %s\n' "$*" >&2
	exit 1
}

# True when the Mach-O file contains the given architecture slice.
has_arch() {
	case " $(lipo -archs "$1" 2>/dev/null) " in
	*" $2 "*) return 0 ;;
	*) return 1 ;;
	esac
}

# Fetch the official (universal) SDL2 release once and cache a normalized
# plain dylib plus its headers. Using libsdl.org instead of Homebrew keeps the
# x86_64 slice available on an Apple Silicon host (Homebrew only ships the
# host arch). The dylib id is set to the absolute cache path so the bundle
# step can find and rewrite it like any other external dependency.
ensure_sdl2() {
	local missing=0 arch
	if [ -f "${SDL2_STAGE}" ] && [ -d "${SDL2_INCLUDE}/SDL2" ]; then
		for arch in ${MACOS_ARCHES}; do
			has_arch "${SDL2_STAGE}" "${arch}" || missing=1
		done
		if [ "${missing}" = "0" ]; then
			log "Reusing cached SDL2 ${SDL2_VERSION} (${SDL2_CACHE})"
			return 0
		fi
	fi

	log "Fetching universal SDL2 ${SDL2_VERSION}"
	mkdir -p "${SDL2_CACHE}"
	local dmg="${SDL2_CACHE}/SDL2-${SDL2_VERSION}.dmg"
	local mnt
	mnt="$(mktemp -d "${TMPDIR:-/tmp}/mmcore-sdl2.XXXXXX")"
	curl -fSL --retry 3 -o "${dmg}" \
		"https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.dmg"
	hdiutil attach -nobrowse -readonly -mountpoint "${mnt}" "${dmg}" >/dev/null

	local fw="${mnt}/SDL2.framework/Versions/A"
	[ -f "${fw}/SDL2" ] || die "SDL2.framework missing from downloaded dmg"

	# The framework umbrella headers include <SDL2/...>, so stage them under
	# an "SDL2" directory and keep that directory's parent on the -I path too.
	rm -rf "${SDL2_INCLUDE}"
	mkdir -p "${SDL2_INCLUDE}/SDL2"
	cp -R "${fw}/Headers/." "${SDL2_INCLUDE}/SDL2/"
	cp "${fw}/SDL2" "${SDL2_STAGE}.tmp"
	hdiutil detach "${mnt}" >/dev/null
	rm -rf "${mnt}" "${dmg}"

	chmod u+w "${SDL2_STAGE}.tmp"
	for arch in ${MACOS_ARCHES}; do
		has_arch "${SDL2_STAGE}.tmp" "${arch}" \
			|| die "downloaded SDL2 lacks ${arch} (need libsdl.org universal dmg)"
	done
	install_name_tool -id "${SDL2_STAGE}" "${SDL2_STAGE}.tmp"
	mv -f "${SDL2_STAGE}.tmp" "${SDL2_STAGE}"
}

# Build the SDL slice for each architecture and lipo them into native/mmcore.
build_universal_binary() {
	ensure_sdl2
	log "Building native SDL binary (${MACOS_ARCHES})"
	# install_name_tool invalidates SDL2's original signature and Apple
	# Silicon refuses to load an invalidly-signed dylib; ad-hoc sign the
	# cached copy so the slices (and native/mmcore) can run in-tree.
	codesign --force --sign - "${SDL2_STAGE}"

	local sd_cflags="-I${SDL2_INCLUDE}/SDL2 -I${SDL2_INCLUDE} -D_THREAD_SAFE"
	local sd_libs="${SDL2_STAGE}"
	local slices=() arch slice
	for arch in ${MACOS_ARCHES}; do
		make -C "${REPO_ROOT}/native" sdl \
			MACOS_ARCH="${arch}" \
			SDL_CFLAGS="${sd_cflags}" SDL_LIBS="${sd_libs}"
		slice="${REPO_ROOT}/native/mmcore-${arch}"
		[ -x "${slice}" ] || die "${slice} was not built"
		slices+=("${slice}")
	done

	rm -f "${BIN}"
	if [ "${#slices[@]}" -gt 1 ]; then
		log "Lipo'ing ${MACOS_ARCHES} -> ${BIN}"
		lipo -create "${slices[@]}" -output "${BIN}"
	else
		cp "${slices[0]}" "${BIN}"
	fi
	[ -x "${BIN}" ] || die "${BIN} was not built"
	# lipo invalidates the per-slice linker signature and Apple Silicon
	# refuses to exec an arm64 binary with an invalid signature. Ad-hoc sign
	# the in-tree artifact so native/mmcore stays runnable by tests.
	codesign --force --sign - "${BIN}"
}

# Notarization credentials: direct App Store Connect credentials win over a
# stored keychain profile so a non-interactive session can still notarize.
if [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${NOTARY_TEAM_ID:-}" ] \
	&& [ -n "${NOTARY_PASSWORD:-}" ]; then
	NOTARY_MODE="direct"
else
	NOTARY_MODE="profile"
fi

# A release build must ship a notarized bundle; validate its requirements before
# spending time on the build so the failure is immediate and obvious.
if [ "${MMCORE_REQUIRE_NOTARY:-}" = "1" ]; then
	[ "${MMCORE_SKIP_SIGN:-}" != "1" ] \
		|| die "MMCORE_REQUIRE_NOTARY=1 conflicts with MMCORE_SKIP_SIGN=1: cannot notarize an unsigned bundle"
	if [ "${NOTARY_MODE}" = "profile" ]; then
		NOTARY_PROFILE="${NOTARY_PROFILE:-mmcore-notary}"
	fi
fi

RAW_VERSION="${VERSION:-$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || echo 0.0.0)}"
RAW_VERSION="${RAW_VERSION#v}"
SHORT_VERSION="$(printf '%s' "${RAW_VERSION}" | sed -n 's/^\([0-9][0-9]*\(\.[0-9][0-9]*\)\{0,2\}\).*/\1/p')"
[ -n "${SHORT_VERSION}" ] || SHORT_VERSION="0.0.0"

log "Version ${RAW_VERSION} (bundle ${SHORT_VERSION})"
build_universal_binary

log "Staging ${APP}"
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Frameworks" \
	"${APP}/Contents/Resources"
cp "${BIN}" "${APP}/Contents/MacOS/${EXE_NAME}"
chmod u+w "${APP}/Contents/MacOS/${EXE_NAME}"

log "Generating app icon"
ICONSET="${DIST}/${APP_NAME}.iconset"
rm -rf "${ICONSET}"
python3 "${REPO_ROOT}/scripts/gen-appicon.py" --iconset "${ICONSET}"
iconutil -c icns "${ICONSET}" -o "${APP}/Contents/Resources/AppIcon.icns"
rm -rf "${ICONSET}"

cat > "${APP}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key>
	<string>${APP_NAME}</string>
	<key>CFBundleDisplayName</key>
	<string>mmcore</string>
	<key>CFBundleIdentifier</key>
	<string>${BUNDLE_ID}</string>
	<key>CFBundleExecutable</key>
	<string>${EXE_NAME}</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>${SHORT_VERSION}</string>
	<key>CFBundleVersion</key>
	<string>${SHORT_VERSION}</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.developer-tools</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
</dict>
</plist>
EOF

# Copy every non-system dylib the binary needs into Frameworks and rewrite the
# load commands. SDL2 only links Apple frameworks, so this is one level deep,
# but recurse anyway so a future dependency is handled too.
bundle_dylibs() {
	local target="$1" dep name dest
	while IFS= read -r dep; do
		case "${dep}" in
		/usr/lib/* | /System/Library/* | @*) continue ;;
		esac
		name="$(basename "${dep}")"
		dest="${APP}/Contents/Frameworks/${name}"
		if [ ! -f "${dest}" ]; then
			cp -f "${dep}" "${dest}"
			chmod u+w "${dest}"
			install_name_tool -id "@rpath/${name}" "${dest}"
			bundle_dylibs "${dest}"
		fi
		install_name_tool -change "${dep}" "@rpath/${name}" "${target}"
		# Only real dependency lines carry "(compatibility version"; this
		# also skips the per-architecture headers that `otool -L` prints for
		# a universal (fat) target.
	done < <(otool -L "${target}" |
		sed -n 's/^[[:space:]]*\([^[:space:]].*\) (compatibility version.*$/\1/p')
}

log "Bundling shared libraries"
bundle_dylibs "${APP}/Contents/MacOS/${EXE_NAME}"
install_name_tool -add_rpath "@executable_path/../Frameworks" \
	"${APP}/Contents/MacOS/${EXE_NAME}" 2>/dev/null || true

detect_identity() {
	local id
	id="$(security find-identity -v -p codesigning 2>/dev/null |
		sed -n 's/.*"\(Developer ID Application: [^"]*\)".*/\1/p' | head -n 1)"
	[ -n "${id}" ] && {
		printf '%s\n' "${id}"
		return
	}
	id="$(security find-identity -v -p codesigning 2>/dev/null |
		sed -n 's/.*"\(Apple Development: [^"]*Marnix Kok[^"]*\)".*/\1/p' | head -n 1)"
	[ -n "${id}" ] && {
		printf '%s\n' "${id}"
		return
	}
	id="$(security find-identity -v -p codesigning 2>/dev/null |
		sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -n 1)"
	[ -n "${id}" ] || id="-"
	printf '%s\n' "${id}"
}

NOTARY_PROFILE="${NOTARY_PROFILE:-}"
want_notary=0
if [ "${MMCORE_SKIP_SIGN:-}" = "1" ]; then
	# lipo -create invalidates the per-slice linker signature; Apple Silicon
	# refuses to run an arm64 binary with an invalid signature, so ad-hoc sign
	# anyway. This is not a distributable signature.
	log "Ad-hoc signing (MMCORE_SKIP_SIGN=1)"
	for lib in "${APP}"/Contents/Frameworks/*.dylib; do
		[ -e "${lib}" ] || continue
		codesign --force --sign - "${lib}"
	done
	codesign --force --sign - "${APP}"
else
	IDENTITY="${SIGN_IDENTITY:-$(detect_identity)}"
	log "Signing as ${IDENTITY}"
	TIMESTAMP=()
	[ "${IDENTITY}" != "-" ] && TIMESTAMP=(--timestamp)
	OPTIONS=()
	[ "${IDENTITY}" != "-" ] && OPTIONS=(--options runtime)

	for lib in "${APP}"/Contents/Frameworks/*.dylib; do
		[ -e "${lib}" ] || continue
		codesign --force ${TIMESTAMP[@]+"${TIMESTAMP[@]}"} \
			${OPTIONS[@]+"${OPTIONS[@]}"} --sign "${IDENTITY}" "${lib}"
	done
	codesign --force ${TIMESTAMP[@]+"${TIMESTAMP[@]}"} \
		${OPTIONS[@]+"${OPTIONS[@]}"} --sign "${IDENTITY}" "${APP}"
	codesign --verify --deep --strict --verbose=2 "${APP}"

	if [ "${MMCORE_REQUIRE_NOTARY:-}" = "1" ]; then
		[ "${IDENTITY}" != "-" ] \
			|| die "release builds need a Developer ID identity to notarize; none found (set SIGN_IDENTITY)"
		want_notary=1
	elif [ "${NOTARY_MODE}" = "direct" ] || [ -n "${NOTARY_PROFILE}" ]; then
		want_notary=1
	fi
fi

if [ "${want_notary}" = "1" ]; then
	if [ "${NOTARY_MODE}" = "direct" ]; then
		notary_desc="direct credentials for ${NOTARY_APPLE_ID}"
		notary_args=(--apple-id "${NOTARY_APPLE_ID}" \
			--team-id "${NOTARY_TEAM_ID}" --password "${NOTARY_PASSWORD}")
	else
		notary_desc="profile ${NOTARY_PROFILE}"
		notary_args=(--keychain-profile "${NOTARY_PROFILE}")
	fi
	log "Notarizing with ${notary_desc}"
	local_zip="${DIST}/.${APP_NAME}-notarize.zip"
	ditto -c -k --keepParent "${APP}" "${local_zip}"
	notary_json="$(xcrun notarytool submit "${local_zip}" \
		${notary_args[@]+"${notary_args[@]}"} --wait --output-format json)" || {
		rm -f "${local_zip}"
		die "notarization failed with ${notary_desc}"
	}
	rm -f "${local_zip}"
	notary_status="$(printf '%s' "${notary_json}" |
		python3 -c 'import json, sys; print(json.load(sys.stdin).get("status", ""))')"
	[ "${notary_status}" = "Accepted" ] \
		|| die "notarization with ${notary_desc} was not accepted (status: ${notary_status:-unknown})"
	xcrun stapler staple "${APP}"
	xcrun stapler validate "${APP}"
	log "Notarized and stapled ${APP}"
fi

log "Packing ${OUT}"
rm -f "${OUT}"
# --norsrc drops extended attributes so the zip has no AppleDouble ._* cruft;
# the code signature lives in Contents/_CodeSignature, not an xattr.
ditto -c -k --norsrc --keepParent "${APP}" "${OUT}"

log "Built ${OUT}"
ls -la "${OUT}"
