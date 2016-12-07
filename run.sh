#!/bin/bash
# mak
_ramsize="512"
diskfile="rootfs.img"
_rebuild="0"
_debug="0"
_curses=""
_logtofile="stdio"
_test="0"
_test_arg=""
_vga="-vga std"
if [ ! -e "kernel" ]; then
	_rebuild="1"
fi

_test_arg_need="0"

for arg in $@
do
if [ "$_test_arg_need" == "1" ]; then
	_test_arg="$_test_arg $arg"
	_test_arg_need="0"
elif [ "$arg" == "rebuild" ]; then
	_rebuild="1"
elif [ "$arg" == "debug" ]; then
	_debug="1"
elif [ "$arg" == "curses" ]; then
	_curses="-curses"
	_vga=""
elif [ "$arg" == "" ]; then
	_rebuild="0"
	_debug="0"
elif [ "$arg" == "logtofile" ]; then
	_logtofile="file:krn.log"
elif [ "$arg" == "test" ]; then
	_test="1"
	_test_arg_need="1"
else
	echo "usage:"
	echo "./run.sh param1 param2 param2 ..."
	echo "param:"
	echo -e "\t rebuild: rebuild project before running"
	echo -e "\t debug: wait for gdb before running"
	echo -e "\t curses: use current console as vm console instead of opening a new window"
	echo -e "\t logtofile: write kernel log to file \"krn.log\" instead of stdio"
	echo -e "\t testall: run all tests"
	exit
fi
done

if [ "$_curses" == "-curses" ]; then
	_logtofile="file:krn.log"
fi

if [ "$_rebuild" == "1" ]; then
	make
fi


echo "begin enum"

if [ "$_debug" == "0" ]; then
	if [ "$_test" == "1" ]; then
		qemu-system-i386 -no-kvm $_curses -m $_ramsize -hda "$diskfile" -kernel kernel -append "test $_test_arg" -serial $_logtofile $_vga
	else
		qemu-system-i386 -no-kvm $_curses -m $_ramsize -hda "$diskfile" -kernel kernel -serial $_logtofile $_vga
	fi
else
	if [ "$_test" == "1" ]; then
		qemu-system-i386 -no-kvm $_curses -no-reboot -m $_ramsize -hda "$diskfile" -kernel kernel -append "test $_test_arg" -serial $_logtofile $_vga  -gdb tcp::8888 -S
	else 
		qemu-system-i386 -no-kvm $_curses -no-reboot -m $_ramsize -hda "$diskfile" -kernel kernel -serial $_logtofile $_vga -gdb tcp::8888 -S
	fi
fi

