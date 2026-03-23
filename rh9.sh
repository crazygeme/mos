#!/bin/bash
# mak
_ramsize="512"
diskfile="rh9.qcow2"
kernel_file="out/kernel"
_debug=""
_window=$([ "$(uname)" == "Linux" ] && echo "gtk" || echo "cocoa")
_verbose=""
_profile=""
_logtofile="stdio"
_vga="-vga std"
_power="-device isa-debug-exit,iobase=0xf4,iosize=0x04"
_kvm=""
_init=""
_test=""
_netdev="user,id=net0"

for arg in $@
do
if [ "$arg" == "test" ]; then
	_test="test"
	kernel_file=out/kernel-test
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
elif [ "$arg" == "tap" ]; then
	_netdev="bridge,id=net0,br=br0"
elif [ "$arg" == "bash" ]; then
	_init="/bin/bash"
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
	echo -e "\t verbose: run with serial log"
	echo -e "\t kvm: enable kvm"
	echo -e "\t tap: bridge VM onto real LAN via br0 (gets real DHCP address)"
	exit
fi
done

if [ "$_window" == "curses" ]; then
	_logtofile="file:out/krn.log"
fi

# ── Bridge state (shared between setup and teardown) ──────────────────────────
_bridge_was_setup=0
_host_nic=""
_host_ip=""
_host_gw=""

setup_bridge() {
	# Detect the NIC that carries the default route
	_host_nic=$(ip route show default | awk '/default/ { print $5; exit }')
	if [ -z "$_host_nic" ]; then
		echo "Error: cannot detect default NIC for bridge setup" >&2
		exit 1
	fi
	echo "tap: using host NIC '$_host_nic' for bridge br0"

	# Create br0 and attach the host NIC (idempotent)
	if ! ip link show br0 &>/dev/null; then
		sudo ip link add br0 type bridge
	fi
	if ! ip link show "$_host_nic" | grep -q "master br0"; then
		# Move the host IP/routes to the bridge before enslaving the NIC
		_host_ip=$(ip -4 addr show "$_host_nic" | awk '/inet / { print $2; exit }')
		_host_gw=$(ip route show default dev "$_host_nic" | awk '{ print $3; exit }')

		sudo ip addr flush dev "$_host_nic"
		sudo ip link set "$_host_nic" master br0
		sudo ip link set br0 up

		# Restore connectivity on the bridge
		if [ -n "$_host_ip" ]; then
			sudo ip addr add "$_host_ip" dev br0
		fi
		if [ -n "$_host_gw" ]; then
			sudo ip route add default via "$_host_gw" dev br0 || true
		fi
	else
		# NIC already enslaved; capture current bridge IP/gw for teardown
		_host_ip=$(ip -4 addr show br0 | awk '/inet / { print $2; exit }')
		_host_gw=$(ip route show default dev br0 | awk '{ print $3; exit }')
		sudo ip link set br0 up
	fi

	# Allow qemu-bridge-helper to use br0
	_bridge_conf="/etc/qemu/bridge.conf"
	if ! grep -qs "allow br0" "$_bridge_conf" 2>/dev/null; then
		sudo mkdir -p /etc/qemu
		echo "allow br0" | sudo tee -a "$_bridge_conf" > /dev/null
		echo "tap: added 'allow br0' to $_bridge_conf"
	fi

	# Ensure qemu-bridge-helper is setuid so we don't need to run qemu as root
	_helper=$(command -v qemu-bridge-helper 2>/dev/null \
	          || echo /usr/lib/qemu/qemu-bridge-helper)
	if [ -f "$_helper" ] && [ ! -u "$_helper" ]; then
		sudo chmod u+s "$_helper"
		echo "tap: set setuid on $_helper"
	fi

	_bridge_was_setup=1
	trap 'cleanup_bridge' EXIT INT TERM
}

cleanup_bridge() {
	[ "$_bridge_was_setup" -eq 0 ] && return
	echo "tap: tearing down bridge br0"

	# Bring bridge down and release the NIC from it
	sudo ip link set br0 down 2>/dev/null
	sudo ip link set "$_host_nic" nomaster 2>/dev/null

	# Restore the host IP and gateway on the physical NIC
	if [ -n "$_host_ip" ]; then
		sudo ip addr flush dev "$_host_nic" 2>/dev/null
		sudo ip addr add "$_host_ip" dev "$_host_nic"
	fi
	sudo ip link set "$_host_nic" up
	if [ -n "$_host_gw" ]; then
		sudo ip route add default via "$_host_gw" dev "$_host_nic" 2>/dev/null || true
	fi

	# Delete the bridge
	sudo ip link delete br0 type bridge 2>/dev/null

	echo "tap: br0 removed, '$_host_nic' restored"
}

# ── Bridge setup (only when tap flag is set) ──────────────────────────────────
if [ "$_netdev" = "bridge,id=net0,br=br0" ]; then
	setup_bridge
fi

make -s -j8 $_test || { echo "Error: build failed" >&2; exit 1; }

if [ ! -f "$diskfile" ]; then
	unzip redhat9.img.zip || { echo "Error: failed to extract disk image" >&2; exit 1; }
fi


qemu-system-i386 -cpu coreduo \
	-display $_window \
	-m $_ramsize \
	-drive file="$diskfile",format=qcow2 \
	-kernel $kernel_file \
	-append "$_verbose $_profile init=$_init" \
	-serial $_logtofile \
	$_vga \
	$_power \
	$_kvm \
	$_debug \
	-netdev $_netdev \
	-device e1000,netdev=net0,mac=52:54:00:12:34:56


