#!/bin/sh
# Wignet TV rootfs: read-only, with every piece of state on /data.
set -eu

# /data: Wi-Fi, the client's state and splash cache, SSH host keys, clock.
grep -q '[[:space:]]/data[[:space:]]' "$TARGET_DIR/etc/fstab" ||
	printf '/dev/mmcblk0p4\t/data\t\text4\tnoatime,commit=60\t0\t0\n' >>"$TARGET_DIR/etc/fstab"
mkdir -p "$TARGET_DIR/data"
ln -snf /data/wignet-tv "$TARGET_DIR/var/lib/wignet-tv"
ln -snf /data/dropbear "$TARGET_DIR/etc/dropbear"

# Services this client does not want at boot.  seedrng: no writable place to
# keep a seed, and the kernel's crng is ready long before the network.
# dnsmasq: only wifi_wpa.py starts it, for the setup AP; a system instance
# would hold port 53.  crond: nothing to schedule.
for script in S01seedrng S20seedrng S20urandom S80dnsmasq S50crond; do
	rm -f "$TARGET_DIR/etc/init.d/$script"
done

# SSH for development, keys only: WIGNET_AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub make
if [ -n "${WIGNET_AUTHORIZED_KEYS:-}" ]; then
	install -d -m 0700 "$TARGET_DIR/root/.ssh"
	install -m 0600 "$WIGNET_AUTHORIZED_KEYS" "$TARGET_DIR/root/.ssh/authorized_keys"
else
	rm -f "$TARGET_DIR/root/.ssh/authorized_keys"
fi
