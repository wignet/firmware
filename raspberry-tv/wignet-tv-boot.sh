#!/bin/sh
# Keep a Wignet TV's boot short and its Linux startup off the television.
#
#   wignet-tv-boot apply        install.sh and every boot (wignet-tv-boot.service)
#   wignet-tv-boot log on|off   diagnostics: show Linux boot messages on the TV
#   wignet-tv-boot status
#
# The diagnostics choice is the `wignet.bootlog=on` token in cmdline.txt, so it
# survives redeploys and can be set from a PC on the card's boot partition
# (it takes effect once `apply` has run on the next boot).  Changes to
# cmdline.txt and config.txt apply at the next reboot.
set -eu
set -f

BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
CMDLINE=$BOOT/cmdline.txt
CONFIG=$BOOT/config.txt
QUIET="quiet logo.nologo vt.global_cursor_default=0 consoleblank=0"
MARK=wignet.bootlog=on

log_mode() {
    for token in $(cat "$CMDLINE"); do
        [ "$token" = "$MARK" ] && { echo on; return; }
    done
    echo off
}

# Kernel messages go to tty3, which is never on screen: tty1 stays blank under
# the Wignet splash.  Verbose puts them back on tty1 as Pi OS ships it.
set_cmdline() {
    out=""
    for token in $(cat "$CMDLINE"); do
        case $token in console=tty1|console=tty3) continue ;; esac
        case " $QUIET $MARK " in *" $token "*) continue ;; esac
        out="$out $token"
    done
    if [ "$1" = on ]; then
        out="$out console=tty1 $MARK"
    else
        out="$out console=tty3 $QUIET"
    fi
    out=${out# }
    [ "$out" = "$(cat "$CMDLINE")" ] && return 0
    [ -e "$CMDLINE.wignet-orig" ] || cp "$CMDLINE" "$CMDLINE.wignet-orig"
    printf '%s\n' "$out" >"$CMDLINE.tmp"
    mv "$CMDLINE.tmp" "$CMDLINE"
    sync
    echo "cmdline.txt: boot messages $1 (next boot)"
}

set_config() {
    [ -e "$CONFIG.wignet-orig" ] || cp "$CONFIG" "$CONFIG.wignet-orig"
    changed=0
    # No rainbow square from the firmware before the kernel starts.
    if ! grep -q '^disable_splash=1' "$CONFIG"; then
        printf '\n[all]\ndisable_splash=1\n' >>"$CONFIG"
        changed=1
    fi
    # The rootfs is mounted by the kernel itself (MMC and ext4 are built in):
    # Pi OS's 14 MB initramfs costs about seven seconds on a Pi Zero.
    if grep -q '^auto_initramfs=1' "$CONFIG"; then
        sed -i 's/^auto_initramfs=1/auto_initramfs=0/' "$CONFIG"
        changed=1
    fi
    [ "$changed" = 0 ] || { sync; echo "config.txt: no firmware splash, no initramfs (next boot)"; }
}

set_getty() {
    if [ "$1" = on ]; then
        systemctl -q is-enabled getty@tty1.service || systemctl enable getty@tty1.service
    elif systemctl -q is-enabled getty@tty1.service; then
        # Its login prompt would be drawn over the splash.
        systemctl disable --now getty@tty1.service
    fi
}

# NetworkManager rewrites a netplan profile at each start (the Imager's Wi-Fi),
# then regenerates netplan and reloads systemd: three times 13-18 s on a Pi
# Zero.  Keyfiles have none of that.  A profile for an SSID Wignet already
# saved is dropped; any other is copied to a keyfile first, and kept if its
# secret did not come across.
migrate_netplan() {
    command -v nmcli >/dev/null || return 0
    list=$(nmcli -t -f UUID,TYPE,FILENAME connection show) || return 0
    printf '%s\n' "$list" | while IFS=: read -r uuid type file; do
        case $file in /run/NetworkManager/system-connections/netplan-*) ;; *) continue ;; esac
        [ "$type" = 802-11-wireless ] || continue
        ssid=$(nmcli -g 802-11-wireless.ssid connection show "$uuid")
        twin=$(printf '%s\n' "$list" | while IFS=: read -r other kind path; do
            case $path in /etc/NetworkManager/system-connections/*) ;; *) continue ;; esac
            [ "$kind" = 802-11-wireless ] || continue
            if [ "$(nmcli -g 802-11-wireless.ssid connection show "$other")" = "$ssid" ]; then
                echo "$other"
            fi
        done)
        if [ -n "$twin" ]; then
            nmcli connection delete "$uuid" >/dev/null && echo "netplan Wi-Fi '$ssid': duplicate removed"
            continue
        fi
        name="wifi-$ssid"
        nmcli connection clone "$uuid" "$name" >/dev/null || continue
        security=$(nmcli -g 802-11-wireless-security.key-mgmt connection show "$name")
        secret=$(nmcli -s -g 802-11-wireless-security.psk connection show "$name")
        if [ -z "$security" ] || [ -n "$secret" ]; then
            nmcli connection modify "$name" connection.autoconnect yes
            nmcli connection delete "$uuid" >/dev/null
            echo "netplan Wi-Fi '$ssid': moved to a keyfile"
        else
            nmcli connection delete "$name" >/dev/null
        fi
    done
}

trim_services() {
    # cloud-init applies the Imager's first-boot settings, then costs about
    # 35 s at every boot.  Off only once it has finished.
    if [ -e /var/lib/cloud/instance/boot-finished ] && [ ! -e /etc/cloud/cloud-init.disabled ]; then
        touch /etc/cloud/cloud-init.disabled
        echo "cloud-init: disabled (first boot done)"
    fi
    # rpi-swap re-runs mkswap at every boot (about 20 s); the client needs
    # less than half of the Pi Zero's memory.
    if [ -e /etc/rpi/swap.conf ] && [ ! -e /etc/rpi/swap.conf.d/90-wignet-tv.conf ]; then
        mkdir -p /etc/rpi/swap.conf.d
        printf '[Main]\nMechanism=none\n' >/etc/rpi/swap.conf.d/90-wignet-tv.conf
        echo "swap: off"
    fi
    # No Bluetooth, removable disks or network-online waiters on this client.
    for unit in bluetooth.service hciuart.service udisks2.service \
            NetworkManager-wait-online.service; do
        if systemctl -q is-enabled "$unit" 2>/dev/null; then
            systemctl disable "$unit" >/dev/null 2>&1 && echo "$unit: disabled"
        fi
    done
}

apply() {
    mode=$(log_mode)
    set_cmdline "$mode"
    set_config
    set_getty "$mode"
    migrate_netplan
    trim_services
}

case ${1:-apply} in
    apply) apply ;;
    log)
        case ${2:-} in
            on|off) set_cmdline "$2"; set_getty "$2"; echo "Reboot to apply." ;;
            *) echo "usage: wignet-tv-boot log on|off" >&2; exit 2 ;;
        esac ;;
    status) echo "boot messages on the TV: $(log_mode)" ;;
    *) echo "usage: wignet-tv-boot apply | log on|off | status" >&2; exit 2 ;;
esac
