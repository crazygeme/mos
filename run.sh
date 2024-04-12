#!/bin/bash
# mak
_ramsize="48"
diskfile="ffs.img"
_rebuild="0"
_debug="0"
_curses=""
_verbose=""
_logtofile="stdio"
_test="0"
_test_arg=""
_vga="-vga std"
_power="-device isa-debug-exit,iobase=0xf4,iosize=0x04"
_kvm=""

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
elif [ "$arg" == "verbose" ]; then
	_verbose="verbose"
elif [ "$arg" == "curses" ]; then
	_curses="-curses"
	_vga=""
elif [ "$arg" == "kvm" ]; then
	_kvm="-enable-kvm"
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
	echo -e "\t test: run with debug information"
	echo -e "\t verbose: run with serial log"
	exit
fi
done

if [ "$_curses" == "-curses" ]; then
	_logtofile="file:krn.log"
fi

if [ "$_rebuild" == "1" ]; then
	make
fi

if [ -f out/linux/x86/make/kernel ]; then
	kernel_file=out/linux/x86/make/kernel
elif [ -f out/linux/x64/release/kernel ]; then
	kernel_file=out/linux/x64/release/kernel
elif [ -f out/linux/x64/debug/kernel ]; then
	kernel_file=out/linux/x64/debug/kernel
elif [ -f out/linux/x86/release/kernel ]; then
	kernel_file=out/linux/x86/release/kernel
elif [ -f out/linux/x86/debug/kernel ]; then
	kernel_file=out/linux/x86/debug/kernel
fi


echo "begin enum $kernel_file"

if [ "$_debug" == "0" ]; then
	if [ "$_test" == "1" ]; then
		qemu-system-i386 $_curses -m $_ramsize -hda "$diskfile" -kernel $kernel_file -append "$_verbose test $_test_arg" -serial $_logtofile $_vga $_power $_kvm
	else
		qemu-system-i386 $_curses -m $_ramsize -hda "$diskfile" -kernel $kernel_file -append "$_verbose" -serial $_logtofile $_vga $_power $_kvm
	fi
else
	if [ "$_test" == "1" ]; then
		qemu-system-i386 $_curses -no-reboot -m $_ramsize -hda "$diskfile" -kernel $kernel_file -append "$_verbose test $_test_arg" -serial $_logtofile $_vga $_power $_kvm -gdb tcp::8888 -S
	else 
		qemu-system-i386 $_curses -no-reboot -m $_ramsize -hda "$diskfile" -kernel $kernel_file -append "$_verbose" -serial $_logtofile $_vga $_power $_kvm  -gdb tcp::8888 -S
	fi
fi

