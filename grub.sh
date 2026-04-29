#!/bin/bash

set -e

_ramsize="2048"
diskfile="rh9.qcow2"
_build="release"
_window=$([ "$(uname)" == "Linux" ] && echo "gtk" || echo "cocoa")
_logtofile="stdio"
_priviledge=""
_IS_MACOS=$([ "$(uname)" == "Darwin" ] && echo "1" || echo "0")
if [ "$_IS_MACOS" -eq 1 ]; then
	_netdev="vmnet-shared,id=net0"
	_priviledge="sudo"
else
	_netdev="tap,id=net0,ifname=tap0,script=no,downscript=no"
fi

for arg in "$@"
do
if [ "$arg" == "logtofile" ]; then
	_logtofile="pending"
elif [ "$arg" == "-h" ]; then
	echo "usage:"
	echo "./run-grub.sh param1 param2 ..."
	echo "param:"
	echo -e "\t kvm: enable kvm"
	echo -e "\t logtofile: write kernel log to out/x86/release/krn.log instead of stdio"
	exit
else
	echo "Error: unsupported argument '$arg'" >&2
	echo "This script always uses the release kernel and boots through GRUB." >&2
	exit 1
fi
done

_outdir="out/x86/$_build"
kernel_file="$_outdir/kernel"

if [ "$_logtofile" == "pending" ]; then
	_logtofile="file:$_outdir/krn.log"
fi

# TAP/NAT state shared between setup and teardown.
_tap_was_setup=0
_tap_dnsmasq_pid=""
_TAP_IF="tap0"
_TAP_GW="10.0.5.1"
_TAP_NET="10.0.5.0/24"
_TAP_RANGE="10.0.5.2,10.0.5.20,1h"

setup_nat() {
	if [ "$_IS_MACOS" -eq 1 ]; then
		echo "tap: using vmnet-shared (macOS) - no host setup required"
		_tap_was_setup=1
		trap 'cleanup_nat' EXIT INT TERM
		return
	fi

	sudo ip tuntap add "$_TAP_IF" mode tap user "$USER"
	sudo ip addr add "$_TAP_GW/24" brd + dev "$_TAP_IF"
	sudo ip link set "$_TAP_IF" up
	echo "tap: $_TAP_IF up ($_TAP_GW/24)"

	sudo sysctl -qw net.ipv4.ip_forward=1
	sudo iptables -t nat -A POSTROUTING -s "$_TAP_NET" -j MASQUERADE
	echo "tap: NAT masquerade for $_TAP_NET"

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
		return
	fi

	echo "tap: tearing down NAT"

	if [ -n "$_tap_dnsmasq_pid" ]; then
		sudo kill "$_tap_dnsmasq_pid" 2>/dev/null
	fi
	sudo rm -f /tmp/mos-dnsmasq.pid

	sudo iptables -t nat -D POSTROUTING -s "$_TAP_NET" -j MASQUERADE 2>/dev/null
	sudo ip link set "$_TAP_IF" down 2>/dev/null
	sudo ip tuntap del "$_TAP_IF" mode tap 2>/dev/null

	echo "tap: $_TAP_IF removed"
}

_disk_mounted=0

cleanup_disk() {
	[ "$_disk_mounted" -eq 0 ] && return
	tools/umountdisk.sh
	_disk_mounted=0
}

trap 'cleanup_disk; cleanup_nat' EXIT INT TERM

make -s -j8 BUILD="$_build" || { echo "Error: build failed" >&2; exit 1; }

if [ ! -f "$diskfile" ]; then
	unzip redhat9.img.zip || { echo "Error: failed to extract disk image" >&2; exit 1; }
fi

tools/mountdisk.sh
_disk_mounted=1
sudo mkdir -p out/mnt/boot
sudo cp "$kernel_file" out/mnt/boot/mos
sync
cleanup_disk

setup_nat

$_priviledge qemu-system-i386 -cpu coreduo \
	-display $_window \
	-m $_ramsize \
	-drive file="$diskfile",format=qcow2,if=ide,index=0,media=disk \
	-serial $_logtofile \
	-vga vmware\
	-device isa-debug-exit,iobase=0xf4,iosize=0x04\
	-enable-kvm\
	-rtc base=localtime \
	-netdev $_netdev \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56

exit "$?"
