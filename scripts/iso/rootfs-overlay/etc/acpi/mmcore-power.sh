#!/bin/sh
# Power button policy (#1268).
#
# On the Chromebook this image runs on, the power key sits directly above
# Backspace, so a missed Backspace must not shut the session down. A short
# press is ignored there (storage is still flushed); every other machine keeps
# the clean shutdown, and the EC's long press still hard-powers the machine.
#
# Two delivery paths exist for the key. The ACPI button driver reports it both
# as a KEY_POWER input event (the "Power Button" input device, and
# cros_ec_buttons on a ChromeOS EC) and as the acpid netlink event this script
# runs from. The live image has neither systemd-logind nor ChromeOS powerd, and
# acpid is started without --input, so nothing consumes the input event; acpid
# is the only path that can power the machine off. Gating it here therefore
# covers both the ACPI fixed power button and the ChromeOS EC (which raises
# PWRBTN# to the PCH or exposes GOOG0007 buttons).
#
# Policy: /etc/mmcore/power.conf's MMCORE_POWER_KEY is `auto` (default),
# `ignore` or `poweroff`. `auto` ignores the key only on a Chromebook, detected
# from DMI (sys_vendor/board_vendor Google, excluding Google Compute Engine) or,
# on ARM, the device-tree compatible list. See docs/framebuffer-and-iso.md.
#
# Overrides (also used by tests): MMCORE_SYS, MMCORE_POWER_CONF,
# MMCORE_SYNC_STORAGE, MMCORE_POWEROFF.
set -u

SYS="${MMCORE_SYS:-/sys}"
CONF="${MMCORE_POWER_CONF:-/etc/mmcore/power.conf}"
SYNC="${MMCORE_SYNC_STORAGE:-/etc/acpi/mmcore-sync-storage.sh}"
POWEROFF="${MMCORE_POWEROFF:-/sbin/poweroff}"

log() {
	[ -w /dev/console ] || return 0
	printf 'mmcore-power: %s\n' "$*" > /dev/console 2>/dev/null || true
}

read_dmi() {
	[ -r "${SYS}/class/dmi/id/$1" ] || return 0
	tr -d '\n' < "${SYS}/class/dmi/id/$1"
}

is_chromebook() {
	vendor="$(read_dmi sys_vendor | tr 'A-Z' 'a-z')"
	product="$(read_dmi product_name | tr 'A-Z' 'a-z')"
	board_vendor="$(read_dmi board_vendor | tr 'A-Z' 'a-z')"

	# Google's cloud VMs also report sys_vendor=Google.
	case "${product}" in
	*"compute engine"*) return 1 ;;
	esac
	case "${vendor} ${board_vendor}" in
	*google*) return 0 ;;
	esac

	# ARM Chromebooks have no DMI; the DT compatible list names Google.
	compatible="${SYS}/firmware/devicetree/base/compatible"
	if [ -r "${compatible}" ] &&
		LC_ALL=C grep -qiE 'google,|chromebook' "${compatible}"; then
		return 0
	fi
	return 1
}

MMCORE_POWER_KEY=auto
[ -r "${CONF}" ] && . "${CONF}"

policy="${MMCORE_POWER_KEY}"
case "${policy}" in
auto|"")
	if is_chromebook; then
		policy=ignore
	else
		policy=poweroff
	fi
	;;
ignore|poweroff) ;;
*)
	log "unknown MMCORE_POWER_KEY='${policy}'; powering off"
	policy=poweroff
	;;
esac

if [ "${policy}" = ignore ]; then
	# The session continues, so flush storage but keep the volumes mounted.
	"${SYNC}"
	log "power button ignored (Chromebook policy)"
	exit 0
fi

"${SYNC}" umount
exec "${POWEROFF}"
