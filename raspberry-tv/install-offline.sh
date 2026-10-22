#!/bin/sh
# Install the first Wignet TV service directly onto an unbooted Pi OS card.
# This is the bootstrap path when the freshly created SSH user deliberately
# has no sudo password yet.
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo ./install-offline.sh /mounted/rootfs" >&2
    exit 1
fi

rootfs=${1:?give the mounted Raspberry Pi rootfs path}
base_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
portal_dir="$base_dir/../esp32/portal"

if [ ! -d "$portal_dir" ]; then
    echo "The Wignet ESP32 portal assets are missing: $portal_dir" >&2
    exit 1
fi


test -d "$rootfs/etc/systemd/system"
install -d -m 0755 "$rootfs/usr/local/lib/wignet-tv"
install -m 0755 "$base_dir/wignet_tv.py" "$base_dir/wifi_setup.py" "$base_dir/cec.py" "$base_dir/tv_power.py" \
    "$base_dir/boot-splash.sh" "$rootfs/usr/local/lib/wignet-tv/"
install -d -m 0755 "$rootfs/usr/local/sbin"
install -m 0755 "$base_dir/wignet-tv-boot.sh" "$rootfs/usr/local/sbin/wignet-tv-boot"
install -m 0644 "$base_dir/sound_list.py" "$base_dir/sound_player.py" "$base_dir/tune_render.py" \
    "$rootfs/usr/local/lib/wignet-tv/"
install -m 0644 "$base_dir/mml.py" "$rootfs/usr/local/lib/wignet-tv/mml.py"
install -d -m 0755 "$rootfs/usr/local/lib/wignet-tv/portal/static"
install -m 0644 "$portal_dir/wifi.html" "$portal_dir/connect.html" \
    "$rootfs/usr/local/lib/wignet-tv/portal/"
install -m 0644 "$portal_dir/portal.css" "$portal_dir/wignet-portal.css" "$portal_dir/portal.js" \
    "$rootfs/usr/local/lib/wignet-tv/portal/static/"
install -d -m 0755 "$rootfs/etc/NetworkManager/dnsmasq-shared.d"
install -m 0644 "$base_dir/wignet-tv-captive-dns.conf" \
    "$rootfs/etc/NetworkManager/dnsmasq-shared.d/wignet-tv.conf"
install -m 0644 "$base_dir/wignet-tv.service" "$base_dir/wignet-tv-setup.service" \
    "$base_dir/wignet-tv-boot.service" "$rootfs/etc/systemd/system/"
install -d -m 0755 "$rootfs/etc/udev/rules.d" "$rootfs/etc/modules-load.d"
install -m 0644 "$base_dir/90-wignet-tv-splash.rules" "$rootfs/etc/udev/rules.d/"
printf "vc4\ni2c_bcm2835\n" >"$rootfs/etc/modules-load.d/wignet-tv.conf"
if ! grep -q '^wignet-tv:' "$rootfs/etc/passwd"; then
    useradd --root "$rootfs" --system --create-home --user-group wignet-tv
fi
install -d -m 0755 "$rootfs/etc/systemd/system/multi-user.target.wants"
ln -sfn ../wignet-tv.service "$rootfs/etc/systemd/system/multi-user.target.wants/wignet-tv.service"
# Boot tuning runs once the first boot's cloud-init is done; the quiet console
# and the other boot changes take effect from the following boot.
ln -sfn ../wignet-tv-boot.service "$rootfs/etc/systemd/system/multi-user.target.wants/wignet-tv-boot.service"

echo "Wignet TV bootstrap installed. Safely eject the card, boot the Pi, and wait for the local Wi-Fi/on-screen setup."
