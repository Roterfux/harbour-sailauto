#!/bin/sh
# Normally started by the app through harbour-sailauto-aoa.path/.service.
# Switch the phone's USB port to Android Open Accessory (AOA) mode, so a car
# (or OpenAuto over USB) talks Android Auto to SailAuto through
# /dev/usb_accessory. Needs root:  devel-su sh aoa-gadget.sh on|off|replug|status
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
    # The autostart watcher (if enabled) already handles START.
    systemctl is-active -q harbour-sailauto-autostart 2>/dev/null && return
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
    # Fallback for the non-systemd-run watcher; never hit the autostart one.
    systemctl is-active -q harbour-sailauto-autostart 2>/dev/null \
        || pkill -f "udevadm monitor --kernel --property" 2>/dev/null || true
}

is_on() {
    [ -f "$SAVE" ] && [ "$(cat "$G/idProduct" 2>/dev/null)" = "0x2d00" ] && [ -n "$(cat "$G/UDC" 2>/dev/null)" ]
}

# --- Autostart -------------------------------------------------------------
# Runs as root service harbour-sailauto-autostart (switched on in the app).
# Triggers: (1) the car asks for Android Auto over USB (kernel uevent
# ACCESSORY=START, also in normal USB mode), (2) the phone connects to a known
# car over Bluetooth (addresses the app saved in .../harbour-sailauto/cars).
# Then: accessory mode on, start SailAuto for the user. When the car is gone
# (USB not configured for 60 s and no car Bluetooth): back to normal USB.
AUTOLOG=/run/harbour-sailauto/autostart.log
AUTOUSER=/var/lib/harbour-sailauto/user

alog() { echo "$(date +%T) $*" >> "$AUTOLOG"; }

auto_user() { [ -f "$AUTOUSER" ] && head -c 64 "$AUTOUSER" | tr -cd 'a-zA-Z0-9_.-'; }

car_macs() {
    u=$(auto_user); [ -n "$u" ] || return 0
    home=$(eval echo "~$u")
    f="$home/.local/share/org.roterfux/harbour-sailauto/cars"
    [ -f "$f" ] && grep -E '^([0-9A-F]{2}:){5}[0-9A-F]{2}$' "$f" | head -8
}

car_bt_connected() {
    for mac in $(car_macs); do
        dev=dev_$(echo "$mac" | tr ':' '_')
        for hci in /org/bluez/hci0 /org/bluez/hci1; do
            dbus-send --system --print-reply --dest=org.bluez "$hci/$dev" \
                org.freedesktop.DBus.Properties.Get string:org.bluez.Device1 string:Connected 2>/dev/null \
                | grep -q "boolean true" && return 0
        done
    done
    return 1
}

launch_app() {
    u=$(auto_user)
    [ -n "$u" ] || { alog "kein Benutzer bekannt, App nicht gestartet"; return; }
    uid=$(id -u "$u" 2>/dev/null) || { alog "Benutzer $u unbekannt"; return; }
    date +%s > /run/harbour-sailauto/car     # a running app reacts to this
    env="XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/dbus/user_bus_socket"
    if command -v runuser > /dev/null; then
        runuser -u "$u" -- env $env systemctl --user start harbour-sailauto-auto.service >> "$AUTOLOG" 2>&1
    else
        su "$u" -s /bin/sh -c "env $env systemctl --user start harbour-sailauto-auto.service" >> "$AUTOLOG" 2>&1
    fi && alog "App gestartet (Benutzer $u)" || alog "App-Start fehlgeschlagen"
}

car_found() {
    now=$(date +%s)
    [ $((now - ${last_car:-0})) -ge 20 ] || return 0
    last_car=$now
    alog "Auto erkannt ($1)"
    if ! is_on; then
        sh /usr/share/harbour-sailauto/aoa-gadget.sh on > /run/harbour-sailauto/last.log 2>&1 \
            && echo on > /run/harbour-sailauto/state || echo error > /run/harbour-sailauto/state
        auto_on=1
        absent=0
    fi
    launch_app
}

auto_tick() {
    if car_bt_connected; then
        [ "$bt" = 1 ] || { bt=1; car_found "Bluetooth"; }
    else
        [ "$bt" = 1 ] && alog "Auto-Bluetooth getrennt"
        bt=0
    fi
    if [ "$auto_on" = 1 ]; then
        if ! is_on; then auto_on=0; return; fi       # switched off by the app
        st=$(cat "/sys/class/udc/$UDC/state" 2>/dev/null)
        if [ "$st" = configured ] || [ "$bt" = 1 ]; then absent=0; else absent=$((absent + 5)); fi
        if [ "$absent" -ge 60 ]; then
            alog "Auto weg (USB \"$st\", kein Bluetooth) -> USB normal"
            sh /usr/share/harbour-sailauto/aoa-gadget.sh off > /run/harbour-sailauto/last.log 2>&1
            echo off > /run/harbour-sailauto/state
            auto_on=0
        fi
    fi
}

auto_watch() {
    set +e   # long-running loop: a failed probe must not end it
    mkdir -p /run/harbour-sailauto
    alog "Autostart-Wächter läuft (Autos: $(car_macs | tr '\n' ' '))"
    is_on && auto_on=1 || auto_on=0
    absent=0; bt=0; last=0; last_car=0
    {
        udevadm monitor --kernel --property 2>/dev/null &
        while sleep 5; do echo TICK; done
    } | while IFS= read -r line; do
        case "$line" in
        TICK) auto_tick ;;
        ACCESSORY=START)
            alog "Kernel: ACCESSORY=START"
            if is_on; then
                now=$(date +%s)
                if [ $((now - last)) -ge 5 ]; then
                    last=$now
                    alog "START -> USB neu verbinden als 18d1:2d00"
                    sleep 0.3
                    echo "" > "$G/UDC" 2>/dev/null || true
                    sleep 0.5
                    echo "$UDC" > "$G/UDC" 2>/dev/null || true
                    chmod 666 /dev/usb_accessory 2>/dev/null || true
                fi
                [ "$auto_on" = 1 ] || launch_app
            else
                car_found "USB"
            fi
            ;;
        ACCESSORY=*) alog "Kernel: $line" ;;
        esac
    done
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
    echo "autostart: $(systemctl is-active harbour-sailauto-autostart 2>/dev/null) (Benutzer: $(auto_user))"
    [ -f "$AUTOLOG" ] && tail -5 "$AUTOLOG"
    [ -f "$WATCHLOG" ] && tail -5 "$WATCHLOG"
}

case "$1" in
on)
    [ -n "$ACC" ] || { echo "Keine accessory-Funktion im Gadget"; exit 1; }
    if is_on; then
        # Already accessory mode (e.g. switched by the autostart watcher):
        # do not re-enumerate, the car may be talking to us right now.
        chmod 666 /dev/usb_accessory 2>/dev/null || true
        echo "AOA ist schon an."
        exit 0
    fi
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
replug)
    # Like pulling and re-inserting the cable: the car sees a fresh device and
    # starts Android Auto again (it stays silent on a reused connection).
    [ -f "$SAVE" ] || { echo "AOA ist nicht an"; exit 1; }
    echo "" > "$G/UDC" 2>/dev/null || true
    sleep 1
    echo "$UDC" > "$G/UDC"
    for i in 1 2 3 4 5; do [ -e /dev/usb_accessory ] && break; sleep 1; done
    chmod 666 /dev/usb_accessory 2>/dev/null || true
    echo "$(date +%T) App: USB neu verbunden" >> "$WATCHLOG"
    echo "USB neu verbunden."
    ;;
status|"")
    status
    ;;
watchloop)
    watch_loop
    ;;
autowatch)
    auto_watch
    ;;
*)
    echo "Aufruf: $0 on|off|replug|status"
    exit 1
    ;;
esac
