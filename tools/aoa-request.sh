#!/bin/sh
# Runs as root from harbour-sailauto-aoa.service. Reads the app's request
# ("on"/"off"/"replug"), runs aoa-gadget.sh and reports the result where only root can
# write: /run/harbour-sailauto/state (on|off|busy|error) and last.log.
REQ=/run/harbour-sailauto-req/request
OUT=/run/harbour-sailauto
mkdir -p "$OUT" && chmod 755 "$OUT"
# Only a plain file, only the two known words.
[ -f "$REQ" ] && [ ! -L "$REQ" ] || exit 0
cmd=$(head -c 10 "$REQ" | tr -cd 'a-z')
case "$cmd" in on|off|replug) ;; *) exit 0 ;; esac
echo busy > "$OUT/state"
if sh /usr/share/harbour-sailauto/aoa-gadget.sh "$cmd" > "$OUT/last.log" 2>&1; then
    # replug keeps accessory mode on
    [ "$cmd" = replug ] && echo on > "$OUT/state" || echo "$cmd" > "$OUT/state"
else
    echo error > "$OUT/state"
fi
