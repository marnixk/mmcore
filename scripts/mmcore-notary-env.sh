# Load App Store Connect notary credentials for non-interactive macOS releases.
# Safe to source repeatedly: skips when NOTARY_APPLE_ID, NOTARY_TEAM_ID, and
# NOTARY_PASSWORD are already set in the environment.
#
# File (optional): ~/.config/mmcore/notary.env
# Expected exports: NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD
if [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${NOTARY_TEAM_ID:-}" ] \
	&& [ -n "${NOTARY_PASSWORD:-}" ]; then
	:
elif [ -r "${HOME}/.config/mmcore/notary.env" ]; then
	set -a
	# shellcheck source=/dev/null
	. "${HOME}/.config/mmcore/notary.env"
	set +a
fi
