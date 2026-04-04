#!/bin/bash
# copy_man_db.sh — copy man database from one disk image to another
# Usage: ./copy_man_db.sh <src_image> <dst_image>
# Format is detected from extension: .img=raw, .qcow2=qcow2

set -e

SRC="$1"
DST="$2"

if [ -z "$SRC" ] || [ -z "$DST" ]; then
    echo "Usage: $0 <src_image> <dst_image>"
    echo "  Supported formats: .img (raw), .qcow2 (qcow2)"
    exit 1
fi

if [ ! -f "$SRC" ]; then echo "Error: source '$SRC' not found"; exit 1; fi
if [ ! -f "$DST" ]; then echo "Error: destination '$DST' not found"; exit 1; fi

detect_format() {
    case "${1##*.}" in
        img)   echo "raw" ;;
        qcow2) echo "qcow2" ;;
        *)     echo "Error: unknown format for '$1' (expected .img or .qcow2)" >&2; exit 1 ;;
    esac
}

SRC_FMT=$(detect_format "$SRC")
DST_FMT=$(detect_format "$DST")

SRC_NBD="/dev/nbd0"
DST_NBD="/dev/nbd1"
SRC_MNT=$(mktemp -d)
DST_MNT=$(mktemp -d)

cleanup() {
    echo "Cleaning up..."
    sudo umount "$DST_MNT" 2>/dev/null || true
    sudo umount "$SRC_MNT" 2>/dev/null || true
    sudo qemu-nbd -d "$DST_NBD" 2>/dev/null || true
    sudo qemu-nbd -d "$SRC_NBD" 2>/dev/null || true
    rmdir "$SRC_MNT" "$DST_MNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "Source: $SRC ($SRC_FMT)"
echo "Destination: $DST ($DST_FMT)"

sudo modprobe nbd max_part=8

echo "Connecting $SRC_NBD..."
sudo qemu-nbd -r -f "$SRC_FMT" -c "$SRC_NBD" "$SRC"

echo "Connecting $DST_NBD..."
sudo qemu-nbd -f "$DST_FMT" -c "$DST_NBD" "$DST"

wait_part() {
    local dev="$1"
    for i in $(seq 15); do
        [ -b "${dev}p1" ] && return 0
        sleep 0.3
    done
    echo "Error: ${dev}p1 did not appear" >&2
    exit 1
}

wait_part "$SRC_NBD"
wait_part "$DST_NBD"

sudo mount -o ro "${SRC_NBD}p1" "$SRC_MNT"
sudo mount "${DST_NBD}p1" "$DST_MNT"

# man database paths on RH9
MAN_PATHS=(
    "var/cache/man"
    "usr/share/man"
)

for rel in "${MAN_PATHS[@]}"; do
    src_path="$SRC_MNT/$rel"
    dst_path="$DST_MNT/$rel"
    if [ -d "$src_path" ]; then
        echo "Copying /$rel ..."
        sudo mkdir -p "$dst_path"
        sudo rsync -a --delete "$src_path/" "$dst_path/"
    else
        echo "Skipping /$rel (not found in source)"
    fi
done

echo "Done."
