#!/bin/bash
# mak
if [ "$2" == "" ]; then
	diskfile="rootfs.img"
else
	diskfile="$2"
fi

#sudo mount -o loop rootfs.img mnt
qemu-system-i386 -no-kvm -no-reboot -curses -m 256 -hda "$diskfile" -kernel $1 -gdb tcp::1234 -S
#sudo umount -l mnt

