#!/bin/sh
# Normally started by the app through harbour-sailauto-aoa.path/.service.
# Switch the phone's USB port to Android Open Accessory (AOA) mode, so a car
# (or OpenAuto over USB) talks Android Auto to SailAuto through
# /dev/usb_accessory. Needs root:  devel-su sh aoa-gadget.sh on|off|status
#
# "on" stops usb-moded (it would undo the change on the next cable event),
# remembers the current gadget setup and presents the phone as
# 18d1:2d00 (Google accessory) with only the accessory function.
# "off" restores the old setup and starts usb-moded again.
# If anything goes wrong: "off", unplug/replug, or reboot.

set -e
G=${SAILAUTO_GADGET:-$(ls -d /sys/kernel/config/usb_gadget/*/ 2>/dev/null | head -1)}
G=${G%/}
# Root-only folder (no symlink games in /tmp).
mkdir -p /run/harbour-sailauto
SAVE=/run/harbour-sailauto/aoa.saved
[ -n "$G" ] || { echo "Kein configfs-USB-Gadget gefunden"; exit 1; }
CFG=$(ls -d "$G"/configs/*/ | head -1)
CFG=${CFG%/}
ACC=$(ls -d "$G"/functions/accessory.* 2>/dev/null | head -1)
UDC=$(ls /sys/class/udc | head -1)

WATCHPID=/run/harbour-sailauto/watch.pid
WATCHLOG=/run/harbour-sailauto/watch.log

# Android reacts to the host's AOA "START" (kernel uevent ACCESSORY=START) by
# re-enumerating in accessory mode. Sailfish has nobody doing that, so a car
# that does the AOA handshake would wait forever. This loop does it.
watch_loop() {
    last=0
    echo "$(date +%T) Wächter läuft" >> "$WATCHLOG"
    udevadm monitor --kernel --property 2>/dev/null | while IFS= read -r line; do
        case "$line" in
        ACCESSORY=*)
            echo "$(date +%T) Kernel: $line" >> "$WATCHLOG"
            if [ "$line" = "ACCESSORY=START" ]; then
                now=$(date +%s)
                # At most once every 5 s, else a host could make us loop.
                if [ $((now - last)) -ge 5 ]; then
                    last=$now
                    echo "$(date +%T) START -> USB neu verbinden als 18d1:2d00" >> "$WATCHLOG"
                    sleep 0.3
                    echo "" > "$G/UDC" 2>/dev/null || true
                    sleep 0.5
                    echo "$UDC" > "$G/UDC" 2>/dev/null || true
                    chmod 666 /dev/usb_accessory 2>/dev/null || true
                fi
            fi
            ;;
        esac
    done
}

start_watch() {
    stop_watch
    command -v udevadm > /dev/null || { echo "udevadm fehlt, kein Wächter" >> "$WATCHLOG"; return; }
    if command -v systemd-run > /dev/null; then
        # Own unit, so it survives the oneshot service that called us.
        systemd-run --unit=harbour-sailauto-aoa-watch --collect \
            /bin/sh /usr/share/harbour-sailauto/aoa-gadget.sh watchloop > /dev/null 2>&1 \
            || systemd-run --unit=harbour-sailauto-aoa-watch \
                /bin/sh /usr/share/harbour-sailauto/aoa-gadget.sh watchloop > /dev/null 2>&1
    else
        (watch_loop) > /dev/null 2>&1 &
        echo $! > "$WATCHPID"
    fi
}

stop_watch() {
    systemctl stop harbour-sailauto-aoa-watch 2>/dev/null || true
    if [ -f "$WATCHPID" ]; then
        pkill -P "$(cat "$WATCHPID")" 2>/dev/null || true
        kill "$(cat "$WATCHPID")" 2>/dev/null || true
        rm -f "$WATCHPID"
    fi
    pkill -f "udevadm monitor --kernel --property" 2>/dev/null || true
}

status() {
    echo "gadget:  $G"
    echo "config:  $CFG"
    echo "udc:     $(cat "$G/UDC" 2>/dev/null) (verfügbar: $UDC)"
    echo "ids:     $(cat "$G/idVendor"):$(cat "$G/idProduct")"
    echo "aktiv:   $(ls "$CFG" | grep -v -e MaxPower -e bmAttributes -e strings | tr '\n' ' ')"
    for f in "$CFG"/*; do [ -L "$f" ] && echo "  $(basename "$f") -> $(basename "$(readlink "$f")")"; done
    echo "device:  $(ls -l /dev/usb_accessory 2>&1)"
    echo "usb-moded: $(systemctl is-active usb-moded 2>/dev/null)"
    echo "wächter: $(systemctl is-active harbour-sailauto-aoa-watch 2>/dev/null)"
    [ -f "$WATCHLOG" ] && tail -5 "$WATCHLOG"
}

case "$1" in
on)
    [ -n "$ACC" ] || { echo "Keine accessory-Funktion im Gadget"; exit 1; }
    if [ ! -f "$SAVE" ]; then
        {
            echo "VID=$(cat "$G/idVendor")"
            echo "PID=$(cat "$G/idProduct")"
            for f in "$CFG"/*; do [ -L "$f" ] && echo "LINK=$(basename "$f"):$(readlink "$f")"; done
        } > "$SAVE"
    fi
    systemctl stop usb-moded 2>/dev/null || true
    echo "" > "$G/UDC" 2>/dev/null || true
    for f in "$CFG"/*; do [ -L "$f" ] && rm "$f"; done
    echo 0x18d1 > "$G/idVendor"
    echo 0x2d00 > "$G/idProduct"
    ln -s "$ACC" "$CFG/f1"
    echo "$UDC" > "$G/UDC"
    # Let the (sandboxed, non-root) app open the accessory device.
    for i in 1 2 3 4 5; do [ -e /dev/usb_accessory ] && break; sleep 1; done
    chmod 666 /dev/usb_accessory 2>/dev/null || true
    : > "$WATCHLOG"
    start_watch
    echo "AOA an. Jetzt in SailAuto den USB-Schalter einschalten."
    status
    ;;
off)
    stop_watch
    if [ ! -f "$SAVE" ]; then
        # Nothing to restore: AOA was not switched on by us. Leave USB alone.
        systemctl start usb-moded 2>/dev/null || true
        echo "AOA war nicht an."
        status
        exit 0
    fi
    echo "" > "$G/UDC" 2>/dev/null || true
    for f in "$CFG"/*; do [ -L "$f" ] && rm "$f"; done
    if [ -f "$SAVE" ]; then
        while IFS= read -r line; do
            case "$line" in
            VID=*) echo "${line#VID=}" > "$G/idVendor" ;;
            PID=*) echo "${line#PID=}" > "$G/idProduct" ;;
            LINK=*) l=${line#LINK=}; ln -s "${l#*:}" "$CFG/${l%%:*}" ;;
            esac
        done < "$SAVE"
        rm -f "$SAVE"
    fi
    echo "$UDC" > "$G/UDC" 2>/dev/null || true
    systemctl start usb-moded 2>/dev/null || true
    echo "AOA aus, USB wie vorher."
    status
    ;;
status|"")
    status
    ;;
watchloop)
    watch_loop
    ;;
*)
    echo "Aufruf: $0 on|off|status"
    exit 1
    ;;
esac
