#!/bin/sh
# Run by udev (90-wignet-tv-splash.rules) the moment vc4 registers the HDMI
# framebuffer, long before the network, Wi-Fi onboarding or Python: it copies
# the splash wignet_tv.py drew and cached at its last start, so the screen
# does not change when the client takes over.  No cache yet (first boot, new
# screen size): the screen stays black until the client draws it.
fb=/sys/class/graphics/fb0
read -r size <"$fb/virtual_size"
read -r stride <"$fb/stride"
read -r bits <"$fb/bits_per_pixel"
[ "$bits" = 16 ] || exit 0
# Same name as wignet_tv.splash_cache_name.
splash=/var/lib/wignet-tv/splash-${size%,*}x${size#*,}-$stride.rgb565
[ -f "$splash" ] || exit 0
exec cat "$splash" >/dev/fb0
