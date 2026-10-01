#!/bin/sh
# Flush persistence mounts before ACPI or BASIC shutdown.
sync
for mnt in /media/mmcore /media/mmcore-sys /media/mmcore-data; do
	if mountpoint -q "${mnt}" 2>/dev/null; then
		umount "${mnt}" 2>/dev/null || true
	fi
done
sync
