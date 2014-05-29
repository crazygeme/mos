#!/bin/bash
# mak
diskfile="ffs.img"
_rebuild="0"
_debug="0"
_format="0"
_curses=""

if [ ! -e "kernel" ]; then
	_rebuild="1"
fi

if [ ! -e "ffstool/ffstool" ]; then
	_rebuild="1"
fi

if [ ! -e "user/bin/run" ]; then
	_rebuild="1"
fi

if [ ! -e "ffs.img" ]; then
	_format="1"
fi

for arg in "$*"
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
else
	echo "usage:"
	echo "./run.sh param1 param2 param2 ..."
	echo "param:"
	echo -e "\t rebuild: rebuild project before running"
	echo -e "\t debug: wait for gdb before running"
	echo -e "\t curses: use current console as vm console instead of opening a new window"
	echo -e "\t format: format disk, and copy all bins under user/bin into vm /bin  before running"
	exit
fi


done

if [ "$_rebuild" == "1" ]; then
	cd ffstool
	make clean
	make
	cd ..

	cd user
	make clean
	make
	cp ../note bin/
	cd ..
	
	make
fi

if [ "$_format" == "1" ]; then
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

if [ "$_debug" == "0" ]; then
	qemu-system-i386 -no-kvm $_curses -m 256 -hda "$diskfile" -kernel kernel
else		
	qemu-system-i386 -no-kvm -no-reboot -m 256 -hda "$diskfile" -kernel kernel -gdb tcp::8888 -S
fi
