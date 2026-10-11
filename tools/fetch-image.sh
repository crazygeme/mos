#!/bin/bash

set -euo pipefail

diskfile="${1:?Usage: fetch-image.sh DISK.qcow2}"
[ -f "$diskfile" ] && exit 0

diskname=$(basename "$diskfile")
if [[ "$diskname" != *.qcow2 || "$diskname" == *[!a-zA-Z0-9._-]* ]]; then
	echo "Error: expected a disk name such as rh9.qcow2" >&2
	exit 1
fi
archive="${diskname%.qcow2}.img.zip"
diskdir=$(dirname "$diskfile")
mkdir -p "$diskdir"
workdir=$(mktemp -d "$diskdir/.image-download.XXXXXX")
trap 'rm -rf "$workdir"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for command in curl unzip; do
	command -v "$command" >/dev/null || { echo "Error: $command is required to fetch the disk image" >&2; exit 1; }
done
if command -v sha256sum >/dev/null; then
	hash_command=sha256sum
elif command -v shasum >/dev/null; then
	hash_command=shasum
else
	echo "Error: sha256sum or shasum is required" >&2
	exit 1
fi

hash_file() {
	if [ "$hash_command" = shasum ]; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		sha256sum "$1" | awk '{print $1}'
	fi
}

primary="https://github.com/crazygeme/mos/releases/download/system-images"
# Override with whitespace-separated base URLs for another storage provider.
sources="${MOS_IMAGE_BASE_URLS:-https://ghfast.top/$primary $primary}"
expected=""

# Every image uses a checksum sidecar published alongside the archive.
for base in $sources; do
	if curl --fail --location --connect-timeout 15 --max-time 60 --retry 2 \
		--output "$workdir/checksum" "${base%/}/$archive.sha256"; then
		expected=$(awk 'NR == 1 {print $1}' "$workdir/checksum")
		if [[ "$expected" =~ ^[a-fA-F0-9]{64}$ ]]; then
			expected=$(printf '%s' "$expected" | tr 'A-F' 'a-f')
			break
		fi
		expected=""
	fi
done
[ -n "$expected" ] || { echo "Error: could not retrieve checksum for $archive" >&2; exit 1; }

archive_path="$diskdir/$archive"
if [ ! -f "$archive_path" ] || [ "$(hash_file "$archive_path")" != "$expected" ]; then
	downloaded=0
	for base in $sources; do
		echo "Downloading ${base%/}/$archive"
		if curl --fail --location --connect-timeout 15 --speed-limit 1024 --speed-time 60 \
			--max-time 7200 --retry 2 --output "$workdir/$archive" "${base%/}/$archive"; then
			if [ "$(hash_file "$workdir/$archive")" = "$expected" ]; then
				mv "$workdir/$archive" "$archive_path"
				downloaded=1
				break
			fi
			echo "Error: checksum mismatch for $archive from $base" >&2
		fi
	done
	[ "$downloaded" -eq 1 ] || { echo "Error: failed to download $archive from all sources" >&2; exit 1; }
fi

echo "Extracting $diskname from $archive"
unzip -p "$archive_path" "$diskname" > "$workdir/$diskname" || { echo "Error: failed to extract $diskname" >&2; exit 1; }
[ -s "$workdir/$diskname" ] || { echo "Error: archive contains no usable $diskname" >&2; exit 1; }
mv "$workdir/$diskname" "$diskfile"
