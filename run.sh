#!/bin/bash
# mak
_ramsize="512"
diskfile="rh9.qcow2"
kernel_file="out/kernel"
_debug=""
_window=$([ "$(uname)" == "Linux" ] && echo "gtk" || echo "cocoa")
_verbose=""
_logtofile="stdio"
_vga="-vga vmware"
_power="-device isa-debug-exit,iobase=0xf4,iosize=0x04"
_kvm=""
_bash=""
_test=""
_priviledge=""
_IS_MACOS=$([ "$(uname)" == "Darwin" ] && echo "1" || echo "0")
if [ "$_IS_MACOS" -eq 1 ]; then
	_netdev="vmnet-shared,id=net0"
	_priviledge="sudo"
else
	_netdev="tap,id=net0,ifname=tap0,script=no,downscript=no"
fi

for arg in $@
do
if [ "$arg" == "test" ]; then
	_test="test"
	kernel_file=out/kernel-test
elif [ "$arg" == "debug" ]; then
	_debug="-gdb tcp::8888 -S"
elif [ "$arg" == "verbose" ]; then
	_verbose="verbose"
elif [ "$arg" == "verbose=0" ]; then
	_verbose="verbose=0"
elif [ "$arg" == "verbose=1" ]; then
	_verbose="verbose=1"
elif [ "$arg" == "verbose=2" ]; then
	_verbose="verbose=2"
elif [ "$arg" == "profile" ]; then
	kernel_file="out/kernel.dbg"
	_debug="-monitor unix:/tmp/qemu-profiler.sock,server,nowait"
elif [ "$arg" == "curses" ]; then
	_window="curses"
	_vga=""
elif [ "$arg" == "kvm" ]; then
	_kvm="-enable-kvm"
elif [ "$arg" == "bash" ]; then
	_bash="bash"
elif [ "$arg" == "logtofile" ]; then
	_logtofile="file:out/krn.log"
elif [ "$arg" == "-h" ]; then
	echo "usage:"
	echo "./run.sh param1 param2 param2 ..."
	echo "param:"
	echo -e "\t test: build and run the test kernel (out/kernel-test)"
	echo -e "\t debug: wait for gdb before running"
	echo -e "\t curses: use current console as vm console instead of opening a new window"
	echo -e "\t logtofile: write kernel log to file \"krn.log\" instead of stdio"
	echo -e "\t verbose: run with focused diagnostic logging (level 2)"
	echo -e "\t verbose=0: disable verbose logging"
	echo -e "\t verbose=1: run with full syscall trace logging"
	echo -e "\t verbose=2: run with focused diagnostic logging"
	echo -e "\t kvm: enable kvm"
	exit
fi
done

if [ "$_window" == "curses" ]; then
	_logtofile="file:out/krn.log"
fi

if [ "$_test" == "test" ]; then
	_netdev="user,id=net0"
	_priviledge=""
fi

# ── TAP/NAT state (shared between setup and teardown) ─────────────────────────
_tap_was_setup=0
_tap_dnsmasq_pid=""
_TAP_IF="tap0"
_TAP_GW="10.0.5.1"
_TAP_NET="10.0.5.0/24"
_TAP_RANGE="10.0.5.2,10.0.5.20,1h"

setup_nat() {
	if [ "$_IS_MACOS" -eq 1 ]; then
		# macOS: vmnet-shared provides TAP + DHCP + NAT automatically; nothing to set up.
		echo "tap: using vmnet-shared (macOS) — no host setup required"
		_tap_was_setup=1
		trap 'cleanup_nat' EXIT INT TERM
		return
	fi

	# Linux: Create the TAP interface owned by the current user (no root for QEMU)
	sudo ip tuntap add "$_TAP_IF" mode tap user "$USER"
	sudo ip addr add "$_TAP_GW/24" brd + dev "$_TAP_IF"
	sudo ip link set "$_TAP_IF" up
	echo "tap: $_TAP_IF up ($_TAP_GW/24)"

	# Enable IPv4 forwarding and masquerade outbound traffic
	sudo sysctl -qw net.ipv4.ip_forward=1
	sudo iptables -t nat -A POSTROUTING -s "$_TAP_NET" -j MASQUERADE
	echo "tap: NAT masquerade for $_TAP_NET"

	# Start dnsmasq to hand the VM a DHCP lease
	sudo dnsmasq \
		--interface="$_TAP_IF" \
		--bind-interfaces \
		--dhcp-range="$_TAP_RANGE" \
		--except-interface=lo \
		--pid-file=/tmp/mos-dnsmasq.pid \
		--log-facility=/dev/null
	_tap_dnsmasq_pid=$(cat /tmp/mos-dnsmasq.pid 2>/dev/null)
	echo "tap: dnsmasq started (pid $_tap_dnsmasq_pid)"

	_tap_was_setup=1
	trap 'cleanup_nat' EXIT INT TERM
}

cleanup_nat() {
	[ "$_tap_was_setup" -eq 0 ] && return

	if [ "$_IS_MACOS" -eq 1 ]; then
		return  # vmnet-shared tears itself down when QEMU exits
	fi

	echo "tap: tearing down NAT"

	# Stop dnsmasq
	if [ -n "$_tap_dnsmasq_pid" ]; then
		sudo kill "$_tap_dnsmasq_pid" 2>/dev/null
	fi
	sudo rm -f /tmp/mos-dnsmasq.pid

	# Remove iptables masquerade rule
	sudo iptables -t nat -D POSTROUTING -s "$_TAP_NET" -j MASQUERADE 2>/dev/null

	# Remove the TAP interface
	sudo ip link set "$_TAP_IF" down 2>/dev/null
	sudo ip tuntap del "$_TAP_IF" mode tap 2>/dev/null

	echo "tap: $_TAP_IF removed"
}

if [ "$_test" != "test" ]; then
	setup_nat
fi

make -s -j8 $_test || { echo "Error: build failed" >&2; exit 1; }

if [ ! -f "$diskfile" ]; then
	unzip redhat9.img.zip || { echo "Error: failed to extract disk image" >&2; exit 1; }
fi


$_priviledge qemu-system-i386 -cpu coreduo \
	-display $_window \
	-m $_ramsize \
	-drive file="$diskfile",format=qcow2,if=ide,index=0,media=disk \
	-kernel $kernel_file \
	-append "$_verbose $_bash $_test" \
	-serial $_logtofile \
	$_vga \
	$_power \
	$_kvm \
	$_debug \
	-no-reboot \
	-rtc base=localtime \
	-netdev $_netdev \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56

rc=$?
if [ "$_test" == "test" ] && [ $((rc & 1)) -eq 1 ]; then
	exit $(((rc - 1) / 2))
fi
exit "$rc"
