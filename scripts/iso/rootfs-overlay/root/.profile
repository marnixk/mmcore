# Start mmcore on the framebuffer console (tty1). Alt+F2 reaches a shell on
# tty2; the serial console on ttyS0 also has an autologin root shell.
if [ "$(tty 2>/dev/null)" = "/dev/tty1" ] && [ -x /usr/local/bin/mmcore ]; then
	export SDL_VIDEODRIVER=kmsdrm
	# Persistent BASIC files live on the MMCORE partition (#835).
	if [ -d /media/mmcore ]; then
		export MMB_DRIVE_ROOT=/media/mmcore
	fi
	# Keep mmcore's stderr for boot diagnostics (the QEMU smoke test greps it
	# for "could not open SDL window").
	mkdir -p /tmp 2>/dev/null || true
	exec /usr/local/bin/mmcore 2>/tmp/mmcore.stderr
fi
