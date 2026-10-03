#!/bin/sh
case "$3" in
close)
	/etc/acpi/mmcore-sync-storage.sh
	echo mem > /sys/power/state
	;;
esac
