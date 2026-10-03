#!/bin/sh
# Flush persistence mounts. Unmount only when the caller passes "umount"
# (poweroff). Closing the lid must leave them mounted: the session resumes,
# and with --read-write /media/mmcore is also the overlay's upper directory.
sync
[ "${1:-}" = "umount" ] || exit 0

mounted() {
	while read -r _src mnt _rest; do
		if [ "$mnt" = "$1" ]; then
			return 0
		fi
	done < /proc/mounts || true
	return 1
}

for mnt in /media/mmcore /media/mmcore-sys /media/mmcore-data; do
	if mounted "${mnt}"; then
		umount "${mnt}" 2>/dev/null || true
	fi
done
sync
