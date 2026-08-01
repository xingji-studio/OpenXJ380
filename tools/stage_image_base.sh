#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <image-root>" >&2
    exit 2
fi

root=$1
bf_dir=${BF_DIR:-Bf}

mkdir -p \
    "$root/EFI/BOOT" \
    "$root/apps/system" \
    "$root/apps/thirdparty" \
    "$root/bin" \
    "$root/boot" \
    "$root/dev" \
    "$root/etc/fastfetch" \
    "$root/etc/gsman/services" \
    "$root/etc/ssl/certs" \
    "$root/etc/xbps.d" \
    "$root/home" \
    "$root/lib" \
    "$root/lib64" \
    "$root/media" \
    "$root/mnt" \
    "$root/mod" \
    "$root/opt" \
    "$root/proc" \
    "$root/root" \
    "$root/run/lock" \
    "$root/sbin" \
    "$root/sys" \
    "$root/system/config" \
    "$root/system/resources" \
    "$root/tmp" \
    "$root/usr/bin" \
    "$root/usr/include" \
    "$root/usr/lib" \
    "$root/usr/libexec" \
    "$root/usr/local/bin" \
    "$root/usr/local/lib" \
    "$root/usr/local/sbin" \
    "$root/usr/sbin" \
    "$root/usr/share/doc" \
    "$root/usr/share/info" \
    "$root/usr/share/lintian/overrides" \
    "$root/usr/share/man/man1" \
    "$root/usr/share/man/man2" \
    "$root/usr/share/man/man3" \
    "$root/usr/share/man/man4" \
    "$root/usr/share/man/man5" \
    "$root/usr/share/man/man6" \
    "$root/usr/share/man/man7" \
    "$root/usr/share/man/man8" \
    "$root/usr/share/terminfo" \
    "$root/users" \
    "$root/var/backups" \
    "$root/var/cache/xbps" \
    "$root/var/db/xbps/keys" \
    "$root/var/db/xbps/metadata" \
    "$root/var/local" \
    "$root/var/lock" \
    "$root/var/log" \
    "$root/var/mail" \
    "$root/var/opt" \
    "$root/var/spool" \
    "$root/var/tmp"

chmod 1777 "$root/tmp" "$root/var/tmp"
printf "nameserver 223.5.5.5\n" > "$root/etc/resolv.conf"

cat > "$root/etc/os-release" <<'EOF'
NAME="XJ380 OS"
PRETTY_NAME="XJ380 Singularity 1.0.0"
ID=xj380
VERSION_ID="1.0"
HOME_URL="https://www.xingjisoft.com/os/xj380"
DOCUMENTATION_URL="https://www.xingjisoft.com/os/xj380"
LOGO=xj380
EOF

if [ -d resources/etc/fastfetch ]; then
    cp -a resources/etc/fastfetch/. "$root/etc/fastfetch/"
fi
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
    cp -a /etc/ssl/certs/ca-certificates.crt "$root/etc/ssl/certs/ca-certificates.crt"
fi
if [ -d "$bf_dir/terminfo" ]; then
    cp -aL "$bf_dir/terminfo/." "$root/usr/share/terminfo/"
fi
