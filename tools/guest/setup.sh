#!/bin/bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
MNT="$REPO_ROOT/out/mnt"
KERNEL_FILE="${1:-}"

cd "$REPO_ROOT"

_disk_mounted=0

cleanup_disk() {
	if [ "$_disk_mounted" -eq 0 ]; then
		return
	fi
	tools/umountdisk.sh
	_disk_mounted=0
}

trap cleanup_disk EXIT INT TERM

tools/mountdisk.sh
_disk_mounted=1

echo "Copy configure files"
sudo mkdir -p "$MNT"
tar -C "$SCRIPT_DIR" --exclude='./setup.sh' -cf - . | sudo tar --no-same-owner -C "$MNT" -xf -

if [ -n "$KERNEL_FILE" ]; then
	if [ ! -f "$KERNEL_FILE" ]; then
		echo "Error: kernel file '$KERNEL_FILE' not found" >&2
		exit 1
	fi
	sudo mkdir -p "$MNT/boot"
	echo "Copy mos kernel into $MNT/boot/mos"
	sudo cp "$KERNEL_FILE" "$MNT/boot/mos"
fi

sync
sleep 0.2
cleanup_disk
