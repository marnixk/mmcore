#!/usr/bin/env bash
# Build the native SDL binary and package it as a self-contained, signed
# macOS `.app` bundle for Apple Silicon.
#
# Output: dist/mmcore.app and dist/mmcore-macos-arm64.zip
# Requires: macOS with Xcode command line tools, Homebrew SDL2, python3.
#
# The bundle is portable: SDL2 (the only non-system dependency) is copied into
# Contents/Frameworks and the executable's install names are rewritten to load
# it from `@rpath`.
#
# Environment:
#   SIGN_IDENTITY       codesign identity. Defaults to a "Developer ID
#                       Application" certificate for Marnix Kok, else any
#                       "Apple Development" one, else ad-hoc ("-").
#   NOTARY_PROFILE      `notarytool` keychain profile (see
#                       `xcrun notarytool store-credentials`). When set, the
#                       bundle is notarized and stapled; requires a Developer ID
#                       identity.
#   MMCORE_SKIP_SIGN=1  build an unsigned bundle (local smoke tests only).
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
BIN="${REPO_ROOT}/linux/mmbasic-sdl"
APP_NAME="mmcore"
APP="${DIST}/${APP_NAME}.app"
EXE_NAME="mmbasic-sdl"
BUNDLE_ID="com.marnixk.mmcore"
OUT="${DIST}/${APP_NAME}-macos-arm64.zip"

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() {
	printf 'package-macos-app: %s\n' "$*" >&2
	exit 1
}

RAW_VERSION="${VERSION:-$(git -C "${REPO_ROOT}" describe --tags --always 2>/dev/null || echo 0.0.0)}"
RAW_VERSION="${RAW_VERSION#v}"
SHORT_VERSION="$(printf '%s' "${RAW_VERSION}" | sed -n 's/^\([0-9][0-9]*\(\.[0-9][0-9]*\)\{0,2\}\).*/\1/p')"
[ -n "${SHORT_VERSION}" ] || SHORT_VERSION="0.0.0"

log "Version ${RAW_VERSION} (bundle ${SHORT_VERSION})"
log "Building native SDL binary"
"${REPO_ROOT}/scripts/build-linux.sh"
[ -x "${BIN}" ] || die "${BIN} not built (SDL2 dev headers missing?)"

log "Staging ${APP}"
rm -rf "${APP}"
mkdir -p "${APP}/Contents/MacOS" "${APP}/Contents/Frameworks" \
	"${APP}/Contents/Resources"
cp "${BIN}" "${APP}/Contents/MacOS/${EXE_NAME}"
chmod u+w "${APP}/Contents/MacOS/${EXE_NAME}"

log "Generating app icon"
ICONSET="${DIST}/${APP_NAME}.iconset"
rm -rf "${ICONSET}"
mkdir -p "${ICONSET}"
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_16x16.png" 16
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_16x16@2x.png" 32
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_32x32.png" 32
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_32x32@2x.png" 64
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_128x128.png" 128
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_128x128@2x.png" 256
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_256x256.png" 256
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_256x256@2x.png" 512
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_512x512.png" 512
python3 "${REPO_ROOT}/scripts/gen-appicon.py" "${ICONSET}/icon_512x512@2x.png" 1024
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
	done < <(otool -L "${target}" | tail -n +2 | awk '{print $1}')
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

if [ "${MMCORE_SKIP_SIGN:-}" = "1" ]; then
	log "Skipping codesign (MMCORE_SKIP_SIGN=1)"
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

	if [ -n "${NOTARY_PROFILE:-}" ]; then
		log "Notarizing with profile ${NOTARY_PROFILE}"
		local_zip="${DIST}/.${APP_NAME}-notarize.zip"
		ditto -c -k --keepParent "${APP}" "${local_zip}"
		xcrun notarytool submit "${local_zip}" \
			--keychain-profile "${NOTARY_PROFILE}" --wait
		rm -f "${local_zip}"
		xcrun stapler staple "${APP}"
	fi
fi

log "Packing ${OUT}"
rm -f "${OUT}"
# --norsrc drops extended attributes so the zip has no AppleDouble ._* cruft;
# the code signature lives in Contents/_CodeSignature, not an xattr.
ditto -c -k --norsrc --keepParent "${APP}" "${OUT}"

log "Built ${OUT}"
ls -la "${OUT}"
