#!/bin/bash

set -euo pipefail

SRC_IMAGE="${1:-}"
DST_IMAGE="${2:-}"
SRC_FILE_PATH="${3:-}"

usage() {
    echo "Usage: $0 <src_image> <dst_image> <src_file_path>"
    echo "  Example: $0 rh9.qcow2 dst.img /etc/passwd"
    echo "  Supported formats: .img (raw), .qcow2 (qcow2)"
}

if [ -z "$SRC_IMAGE" ] || [ -z "$DST_IMAGE" ] || [ -z "$SRC_FILE_PATH" ]; then
    usage
    exit 1
fi

if [ ! -f "$SRC_IMAGE" ]; then
    echo "Error: source image '$SRC_IMAGE' not found" >&2
    exit 1
fi

if [ ! -f "$DST_IMAGE" ]; then
    echo "Error: destination image '$DST_IMAGE' not found" >&2
    exit 1
fi

case "$SRC_FILE_PATH" in
    /*) ;;
    *)
        echo "Error: source file path must be absolute, got '$SRC_FILE_PATH'" >&2
        exit 1
        ;;
esac

detect_format() {
    case "${1##*.}" in
        qcow2) echo "qcow2" ;;
        img) echo "raw" ;;
        *)
            echo "Error: unknown image format for '$1' (expected .img or .qcow2)" >&2
            exit 1
            ;;
    esac
}

wait_part() {
    local dev="$1"
    for _ in $(seq 20); do
        if [ -b "${dev}p1" ]; then
            return 0
        fi
        sleep 0.3
    done
    echo "Error: ${dev}p1 did not appear" >&2
    exit 1
}

SRC_FMT="$(detect_format "$SRC_IMAGE")"
DST_FMT="$(detect_format "$DST_IMAGE")"

SRC_NBD="/dev/nbd0"
DST_NBD="/dev/nbd1"
SRC_MNT="$(mktemp -d)"
DST_MNT="$(mktemp -d)"
SRC_MOUNTED=0
DST_MOUNTED=0
SRC_CONNECTED=0
DST_CONNECTED=0
IN_CLEANUP=0

cleanup() {
    if [ "$IN_CLEANUP" -eq 1 ]; then
        return
    fi
    IN_CLEANUP=1

    if [ "$DST_MOUNTED" -eq 1 ]; then
        sudo umount "$DST_MNT" 2>/dev/null || true
    fi
    if [ "$SRC_MOUNTED" -eq 1 ]; then
        sudo umount "$SRC_MNT" 2>/dev/null || true
    fi
    if [ "$DST_CONNECTED" -eq 1 ]; then
        sudo qemu-nbd -d "$DST_NBD" 2>/dev/null || true
    fi
    if [ "$SRC_CONNECTED" -eq 1 ]; then
        sudo qemu-nbd -d "$SRC_NBD" 2>/dev/null || true
    fi
    rmdir "$DST_MNT" "$SRC_MNT" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "Source image: $SRC_IMAGE ($SRC_FMT)"
echo "Destination image: $DST_IMAGE ($DST_FMT)"
echo "File to copy: $SRC_FILE_PATH"

sudo modprobe nbd max_part=8

sudo qemu-nbd -r -f "$SRC_FMT" -c "$SRC_NBD" "$SRC_IMAGE"
SRC_CONNECTED=1
sudo qemu-nbd -f "$DST_FMT" -c "$DST_NBD" "$DST_IMAGE"
DST_CONNECTED=1

wait_part "$SRC_NBD"
wait_part "$DST_NBD"

sudo mount -o ro "${SRC_NBD}p1" "$SRC_MNT"
SRC_MOUNTED=1
sudo mount "${DST_NBD}p1" "$DST_MNT"
DST_MOUNTED=1

SRC_HOST_PATH="$SRC_MNT$SRC_FILE_PATH"
DST_HOST_PATH="$DST_MNT$SRC_FILE_PATH"
DST_DIR="$(dirname "$DST_HOST_PATH")"

if [ ! -f "$SRC_HOST_PATH" ]; then
    echo "Error: source file '$SRC_FILE_PATH' not found in '$SRC_IMAGE'" >&2
    exit 1
fi

sudo mkdir -p "$DST_DIR"
sudo cp -a "$SRC_HOST_PATH" "$DST_HOST_PATH"

echo "Copied '$SRC_FILE_PATH' from '$SRC_IMAGE' to '$DST_IMAGE'"
