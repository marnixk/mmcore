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
		kill "$(cat /run/mmcore-splash.pid)" 2>/dev/null || true
		rm -f /run/mmcore-splash.pid
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
		case "${mmcore_sys_dev}" in
			LABEL=*) mmcore_sys_dev="$(blkid -L "${mmcore_sys_dev#LABEL=}" 2>/dev/null)" ;;
		esac
		case "${mmcore_data_dev}" in
			LABEL=*) mmcore_data_dev="$(blkid -L "${mmcore_data_dev#LABEL=}" 2>/dev/null)" ;;
		esac
		if [ -n "${mmcore_sys_dev}" ]; then
			mount -o rw "${mmcore_sys_dev}" /media/mmcore-sys 2>/dev/null || true
		fi
		if [ -n "${mmcore_data_dev}" ]; then
			mount -o rw "${mmcore_data_dev}" /media/mmcore-data 2>/dev/null || true
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
