#!/bin/sh
# Run on a freshly imaged Raspberry Pi OS Lite system, as root.
set -eu

client_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
portal_dir="$client_dir/../esp32/portal"
if [ ! -d "$portal_dir" ]; then
    echo "The Wignet ESP32 portal assets are missing: $portal_dir" >&2
    exit 1
fi


install -d -m 0755 /usr/local/lib/wignet-tv
install -m 0755 "$client_dir/wignet_tv.py" "$client_dir/wifi_setup.py" "$client_dir/cec.py" "$client_dir/tv_power.py" \
    "$client_dir/boot-splash.sh" /usr/local/lib/wignet-tv/
install -m 0755 "$client_dir/wignet-tv-boot.sh" /usr/local/sbin/wignet-tv-boot
install -m 0644 "$client_dir/sound_list.py" "$client_dir/sound_player.py" "$client_dir/tune_render.py" \
    /usr/local/lib/wignet-tv/
install -m 0644 "$client_dir/mml.py" /usr/local/lib/wignet-tv/mml.py
# Reported to Wignet as fw= on /schedule: the commit this was installed from.
python3 "$client_dir/tv_version.py" >/usr/local/lib/wignet-tv/VERSION
install -d -m 0755 /usr/local/lib/wignet-tv/portal/static
install -m 0644 "$portal_dir/wifi.html" "$portal_dir/connect.html" /usr/local/lib/wignet-tv/portal/
install -m 0644 "$portal_dir/portal.css" "$portal_dir/wignet-portal.css" "$portal_dir/portal.js" \
    /usr/local/lib/wignet-tv/portal/static/
install -d -m 0755 /etc/NetworkManager/dnsmasq-shared.d
install -m 0644 "$client_dir/wignet-tv-captive-dns.conf" \
    /etc/NetworkManager/dnsmasq-shared.d/wignet-tv.conf
install -m 0644 "$client_dir/wignet-tv.service" /etc/systemd/system/wignet-tv.service
install -m 0644 "$client_dir/wignet-tv-setup.service" /etc/systemd/system/wignet-tv-setup.service
install -m 0644 "$client_dir/wignet-tv-boot.service" /etc/systemd/system/
install -m 0644 "$client_dir/90-wignet-tv-splash.rules" /etc/udev/rules.d/
# Load the HDMI driver and its DDC bus first thing, not when udev gets to
# them: the framebuffer the boot splash goes to waits for both.
printf "vc4\ni2c_bcm2835\n" >/etc/modules-load.d/wignet-tv.conf
id -u wignet-tv >/dev/null 2>&1 || useradd --system --create-home --user-group wignet-tv
# StateDirectory= in wignet-tv.service creates /var/lib/wignet-tv with this
# service account as owner; do not pre-create it as root here.
cat >/etc/wignet-tv.conf <<'EOF'
# The client declares itself to Wignet (full colour, screen size): nothing to
# set up on the server. Unset, the size is the HDMI mode's; set both to
# override it (an admin-set size on the server still wins).
WIGNET_HOST=https://api.wignet.app
# Wake a compatible TV and select this input at Wignet client startup.  Set
# off for a monitor or a television whose CEC settings should remain untouched.
WIGNET_CEC=on
# Ring the Wignet's alarms and pushed sounds through the TV's speakers (HDMI
# audio). Set off for a monitor without speakers: the Wignet then offers none.
WIGNET_SOUND=on
# The ALSA output; the default is usually HDMI on a Pi Zero (aplay -L lists them).
# WIGNET_AUDIO_DEVICE=hdmi:CARD=vc4hdmi,DEV=0
# WIGNET_CEC_DEVICE=/dev/cec0
# WIGNET_WIDTH=1920
# WIGNET_HEIGHT=1080
# WIGNET_MAC=aa:bb:cc:dd:ee:ff
EOF
systemctl daemon-reload
systemctl enable --now wignet-tv.service
systemctl enable wignet-tv-boot.service
# Quiet console, no initramfs, unused services off: see wignet-tv-boot.sh.
# `wignet-tv-boot log on` keeps the Linux messages on the TV for diagnosis.
/usr/local/sbin/wignet-tv-boot apply
