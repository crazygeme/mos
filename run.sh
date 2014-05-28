#!/bin/bash
# mak
if [ "$2" == "" ]; then
	diskfile="ffs.img"
else
	diskfile="$2"
fi

cd user
make clean
make
cp ../README.md bin/
cd ..
if [ "$diskfile" == "ffs.img" ]; then
	if [ -e "$diskfile" ]; then
		rm "$diskfile"
	fi
	cp rootfs.img $diskfile
	cd ffstool
	if [ ! -e ffstool ]; then
		make
	fi
	echo "format ffs.img"
	./ffstool format
	echo "done"
	cd ..
fi
echo "begin enum"
#sudo mount -o loop rootfs.img mnt
qemu-system-i386 -no-kvm -m 256 -hda "$diskfile" -kernel $1
#qemu-system-i386 -no-kvm -m 256 -drive file="$diskfile",index=0,media=disk,if=virtio,cache=unsafe -kernel $1
#sudo umount -l mnt

