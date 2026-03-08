#!/bin/bash
# mak
_ramsize="512"
diskfile="ffs.img"
_rebuild="0"
_debug=""
_window="gtk"
_verbose=""
_profile=""
_logtofile="stdio"
_vga="-vga std"
_power="-device isa-debug-exit,iobase=0xf4,iosize=0x04"
_kvm=""

for arg in $@
do
if [ "$arg" == "rebuild" ]; then
	_rebuild="1"
elif [ "$arg" == "debug" ]; then
	_debug="-gdb tcp::8888 -S"
elif [ "$arg" == "verbose" ]; then
	_verbose="verbose"
elif [ "$arg" == "profile" ]; then
	_profile="profile"
elif [ "$arg" == "curses" ]; then
	_window="curses"
	_vga=""
elif [ "$arg" == "kvm" ]; then
	_kvm="-enable-kvm"
elif [ "$arg" == "logtofile" ]; then
	_logtofile="file:krn.log"
elif [ "$arg" == "-h" ]; then
	echo "usage:"
	echo "./run.sh param1 param2 param2 ..."
	echo "param:"
	echo -e "\t rebuild: rebuild project before running"
	echo -e "\t debug: wait for gdb before running"
	echo -e "\t curses: use current console as vm console instead of opening a new window"
	echo -e "\t logtofile: write kernel log to file \"krn.log\" instead of stdio"
	echo -e "\t verbose: run with serial log"
	echo -e "\t kvm: enable kvm"
	exit
fi
done

if [ "$_window" == "curses" ]; then
	_logtofile="file:krn.log"
fi

if [ "$_rebuild" == "1" ]; then
	make rebuild
fi

kernel_file=out/kernel
echo "$_verbose $_profile"

qemu-system-i386 -cpu coreduo \
	-display $_window \
	-m $_ramsize \
	-drive file="$diskfile",format=raw \
	-kernel $kernel_file \
	-append "$_verbose $_profile" \
	-serial $_logtofile \
	$_vga \
	$_power \
	$_kvm \
	$_debug


