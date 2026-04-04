#!/bin/bash

set -e

NBD_DEV="/dev/nbd0"
MNT="mnt"

sudo umount "$MNT"
sudo qemu-nbd -d "$NBD_DEV"
echo "Unmounted and disconnected $NBD_DEV"
