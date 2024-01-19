#!/bin/bash

e=true

trap ctrl_c INT

function ctrl_c(){
	e=false
}

if [ -e gdb.log ]; then
	rm gdb.log
fi

echo -e "Please start your os instance, then press any key to continue"
read tmp
qemu_process=$(ps -C qemu-system-i386 -o pid --no-headers)
if [ "$qemu_process" == "" ];then
	echo -e "No os running! quit"
	exit
fi

gdb -x ./tools/gdb_trigger &>/dev/null

echo -e "Press any key to start profiling, press ctrl-c to stop and show results"
read tmp

while $e
do
	gdb -x ./tools/gdb_prob &>/dev/null
done

echo
echo
echo "======================================="
./tools/analyze.py gdb.log
