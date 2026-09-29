#!/bin/sh
set -eu
BASE=/root/tests/sys_devices
mkdir -p "$BASE"
cleanup()
{
	umount "$BASE" >/dev/null 2>&1 || true
	rmdir "$BASE" >/dev/null 2>&1 || true
}
trap cleanup EXIT
mount -t sysfs sysfs "$BASE"
count=0
for device in "$BASE"/bus/pci/devices/*; do
	[ -L "$device" ]
	name=${device##*/}
	[ -r "$BASE/devices/pci0000:00/$name/vendor" ]
	[ "$(cat "$device/vendor")" = "$(cat "$BASE/devices/pci0000:00/$name/vendor")" ]
	[ -L "$device/subsystem" ]
	if [ -L "$device/driver" ]; then
		[ -L "$device/driver/$name" ]
		[ "$(cat "$device/device")" = "$(cat "$device/driver/$name/device")" ]
		grep '^DRIVER=' "$device/uevent" >/dev/null
	fi
	count=$((count + 1))
done
[ "$count" -gt 0 ]
[ -d "$BASE/bus/pci/drivers/ata-ide" ]
[ -d "$BASE/bus/pci/drivers/virtio_gpu" ]
# A second mount exports the same inventory without hardware initialization.
umount "$BASE"
mount -t sysfs sysfs "$BASE"
after=0
for device in "$BASE"/bus/pci/devices/*; do
	[ -L "$device" ]
	after=$((after + 1))
done
[ "$count" -eq "$after" ]

[ -d "$BASE/bus/serio/drivers/ps2-keyboard" ]
[ -d "$BASE/bus/serio/drivers/ps2-mouse" ]
for device in "$BASE"/bus/serio/devices/*; do
	[ -e "$device" ] || continue
	[ -L "$device" ]
	name=${device##*/}
	[ -r "$BASE/devices/platform/i8042/$name/description" ]
	[ -L "$device/subsystem" ]
	if [ -L "$device/driver" ]; then
		[ -L "$device/driver/$name" ]
	fi
done
