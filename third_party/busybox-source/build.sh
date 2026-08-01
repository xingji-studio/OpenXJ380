#!/bin/sh
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output=${1:-$here/busybox-1.31.1-amd64}
jobs=${JOBS:-2}
work=$(mktemp -d)
trap 'find "$work" -depth -delete' EXIT HUP INT TERM

tar -xf "$here/busybox-1.31.1.tar.bz2" -C "$work"
source_dir="$work/busybox-1.31.1"
patch -s -d "$source_dir" -p1 < "$here/0001-date-use-clock-settime.patch"
cp "$here/busybox-1.31.1.config" "$source_dir/.config"

export KBUILD_BUILD_TIMESTAMP="2026-08-01 00:00:00 UTC"
export KBUILD_BUILD_USER=xj380
export KBUILD_BUILD_HOST=reproducible
export KCONFIG_NOTIMESTAMP=1
yes '' | make -s -C "$source_dir" oldconfig >/dev/null
make -s -C "$source_dir" -j"$jobs"
cp "$source_dir/busybox" "$output"
