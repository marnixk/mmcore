# Start mmcore on the framebuffer console (tty1). Ctrl+Alt+F2 reaches a shell
# on tty2 (Ctrl+Alt+F1 returns to mmcore); the serial console on ttyS0 also has
# an autologin root shell.
#
mmcore_tty="$(tty 2>/dev/null)"

# #923: tell the user on tty2/serial how to install or update, since mmcore
# itself only paints tty1. tty1 must stay quiet before `exec mmcore` (#863), so
# guard on the tty rather than using /etc/motd, which agetty's login prints on
# every console.
if [ "${mmcore_tty}" != "/dev/tty1" ]; then
	printf '%s\n' 'mmcore live media. Install to disk: mmcore-install   Update: mmcore-update'
fi

if [ "${mmcore_tty}" = "/dev/tty1" ]; then
	export SDL_VIDEODRIVER=kmsdrm
	# Keep mmcore's stderr for boot diagnostics (the QEMU smoke test greps it
	# for "could not open SDL window").
	mkdir -p /tmp 2>/dev/null || true

	# The early boot splash (#902) has served its purpose: stop repainting the
	# framebuffer so mmcore's first KMS frame is not overwritten. Kill the pid
	# mmcore-splash-start recorded: BusyBox pkill -x cannot match the helper's
	# absolute argv[0], so a name match silently leaves it running (#921). The
	# anchored -f fallback covers a stale or missing pidfile.
	if [ -f /run/mmcore-splash.pid ]; then
		mmcore_splash_pid="$(cat /run/mmcore-splash.pid)"
		kill "${mmcore_splash_pid}" 2>/dev/null || true
		rm -f /run/mmcore-splash.pid
		# #1032: wait for the helper to actually close /dev/fb0 before
		# mmcore takes DRM master. Its loop notices SIGTERM within 100 ms;
		# the 1 s grace is the bound, and SIGKILL escalates a wedged helper
		# so it can never hold the framebuffer across the modeset.
		sleep 1
		kill -9 "${mmcore_splash_pid}" 2>/dev/null || true
	fi
	pkill -f '^/usr/local/bin/mmcore-splash' 2>/dev/null || true

	# Hard-disk install (#891): the installed GRUB entry names the two
	# partitions on the kernel command line. Mount MMCORE-SYS as C: and
	# MMCORE-DATA as D:, then run the binary from the system partition so
	# mmcore-update can replace it in place.
	mmcore_sys_dev=""
	mmcore_data_dev=""
	for mmcore_arg in $(cat /proc/cmdline 2>/dev/null); do
		case "${mmcore_arg}" in
			mmcore.sys=*) mmcore_sys_dev="${mmcore_arg#mmcore.sys=}" ;;
			mmcore.data=*) mmcore_data_dev="${mmcore_arg#mmcore.data=}" ;;
		esac
	done
	if [ -n "${mmcore_sys_dev}" ]; then
		mkdir -p /media/mmcore-sys /media/mmcore-data 2>/dev/null || true

		# #1034: a slow-to-probe block device (flaky eMMC/SD, a spun-down
		# disk, USB enumeration) must not stall tty1 before `exec mmcore`.
		# Bound every label lookup and mount with BusyBox `timeout`; on the
		# cap, log to the serial console and fall through to the live path /
		# the next step instead of blocking the login shell. Default 5 s,
		# override with MMCORE_PROBE_TIMEOUT.
		mmcore_probe_timeout="${MMCORE_PROBE_TIMEOUT:-5}"
		mmcore_probe() {
			mmcore_probe_rc=0
			timeout "${mmcore_probe_timeout}" "$@" 2>/dev/null || mmcore_probe_rc=$?
			case "${mmcore_probe_rc}" in
				124|137|143)
					printf '%s\n' "mmcore: ${mmcore_probe_timeout}s probe timeout: $*" >/dev/console 2>/dev/null || true
					return 124
					;;
			esac
			return "${mmcore_probe_rc}"
		}

		case "${mmcore_sys_dev}" in
			LABEL=*) mmcore_sys_dev="$(mmcore_probe blkid -L "${mmcore_sys_dev#LABEL=}" 2>/dev/null)" ;;
		esac
		case "${mmcore_data_dev}" in
			LABEL=*) mmcore_data_dev="$(mmcore_probe blkid -L "${mmcore_data_dev#LABEL=}" 2>/dev/null)" ;;
		esac
		if [ -n "${mmcore_sys_dev}" ]; then
			mmcore_probe mount -o rw "${mmcore_sys_dev}" /media/mmcore-sys 2>/dev/null || true
		fi
		if [ -n "${mmcore_data_dev}" ]; then
			mmcore_probe mount -o rw "${mmcore_data_dev}" /media/mmcore-data 2>/dev/null || true
		fi
		if [ -x /media/mmcore-sys/mmcore ]; then
			export MMB_DRIVE_ROOT=/media/mmcore-sys
			exec /media/mmcore-sys/mmcore --drive /media/mmcore-data 2>/tmp/mmcore.stderr
		fi
	fi

	# Live session: a partition labelled MMCORE is the persistent C: (#835).
	if [ -x /usr/local/bin/mmcore ]; then
		if [ -d /media/mmcore ]; then
			export MMB_DRIVE_ROOT=/media/mmcore
		fi
		exec /usr/local/bin/mmcore 2>/tmp/mmcore.stderr
	fi
fi
