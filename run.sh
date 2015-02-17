#!/bin/bash
# mak
diskfile="rootfs.img"
_rebuild="0"
_debug="0"
_format="0"
_curses=""
_logtofile="stdio"

if [ ! -e "kernel" ]; then
	_rebuild="1"
fi

#if [ ! -e "ffstool/ffstool" ]; then
#	_rebuild="1"
#fi

if [ ! -e "user/bin/run" ]; then
	_rebuild="1"
fi

if [ ! -e $diskfile ]; then
	_format="1"
fi

for arg in $@
do
if [ "$arg" == "rebuild" ]; then
	_rebuild="1"
elif [ "$arg" == "debug" ]; then
	_debug="1"
elif [ "$arg" == "curses" ]; then
	_curses="-curses"
elif [ "$arg" == "format" ]; then
	_format="1"
elif [ "$arg" == "" ]; then
	_rebuild="0"
	_debug="0"
elif [ "$arg" == "logtofile" ]; then
	_logtofile="file:krn.log"
else
	echo "usage:"
	echo "./run.sh param1 param2 param2 ..."
	echo "param:"
	echo -e "\t rebuild: rebuild project before running"
	echo -e "\t debug: wait for gdb before running"
	echo -e "\t curses: use current console as vm console instead of opening a new window"
	echo -e "\t format: format disk, and copy all bins under user/bin into vm /bin  before running"
	echo -e "\t logtofile: write kernel log to file \"krn.log\" instead of stdio"
	exit
fi
done

if [ "$_curses" == "-curses" ]; then
	_logtofile="file:krn.log"
fi

if [ "$_rebuild" == "1" ]; then
	cd user
	make clean
	make
	cp ../note bin/
	cd ..
	
	make
fi

if [ "$_format" == "1" ]; then
	if [ "$(uname)" == "linux" ]; then
		mkdir mnt
		sudo mount -t ext2 -o loop,offset=1048576 rootfs.img mnt
		cp -f user/bin/* mnt/bin/
		cp -f user/lib/* mnt/lib/
		cp -f user/stable/bin/* mnt/bin/
		cp -f iser/stable/lib/* mnt/lib/
		sudo umount mnt
		rm -rf mnt
	else
		echo "Only support on linux"
	fi
fi


echo "begin enum"

if [ "$_debug" == "0" ]; then
	qemu-system-i386 -no-kvm $_curses -m 256 -hda "$diskfile" -kernel kernel -serial $_logtofile -vga std
else		
	qemu-system-i386 -no-kvm $_curses -no-reboot -m 256 -hda "$diskfile" -kernel kernel -serial $_logtofile -vga std -gdb tcp::8888 -S
fi

