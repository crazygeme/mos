#!/bin/bash

mkdir mnt
sudo mount -t ext2 -o loop,offset=1048576 rootfs.img mnt
