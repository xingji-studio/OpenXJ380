#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <image-root>" >&2
    exit 2
fi

root=$1

mkdir -p \
    "$root/EFI/BOOT" \
    "$root/apps/system" \
    "$root/dev" \
    "$root/etc" \
    "$root/mnt" \
    "$root/mod" \
    "$root/proc" \
    "$root/run" \
    "$root/sys" \
    "$root/system/config" \
    "$root/system/resources" \
    "$root/tmp"

chmod 1777 "$root/tmp"
printf "nameserver 223.5.5.5\n" > "$root/etc/resolv.conf"

cat > "$root/etc/os-release" <<'EOF'
NAME="XJ380 OS"
PRETTY_NAME="XJ380 Kernel Test Image"
ID=xj380
VERSION_ID="1.0"
HOME_URL="https://www.xingjisoft.com/os/xj380"
DOCUMENTATION_URL="https://www.xingjisoft.com/os/xj380"
LOGO=xj380
EOF
