#!/usr/bin/env python3
"""
Decode a SailAuto session recording (trace.bin) into readable lines.

    python3 tools/aap_trace.py ~/Documents/SailAuto/20261125-213000/trace.bin
    python3 tools/aap_trace.py trace.bin --all      # include every video frame/ack/ping

Record format (big endian): u64 time_us, 'I'/'O', u8 channel, u8 flags,
u16 message id, u32 full payload size, u32 stored size, payload.
"""
import argparse
import os
import struct
import sys

CONTROL = {1: "VERSION_REQUEST", 2: "VERSION_RESPONSE", 3: "SSL_HANDSHAKE", 4: "AUTH_COMPLETE",
           5: "SERVICE_DISCOVERY_REQUEST", 6: "SERVICE_DISCOVERY_RESPONSE", 7: "CHANNEL_OPEN_REQUEST",
           8: "CHANNEL_OPEN_RESPONSE", 9: "CHANNEL_CLOSE", 11: "PING_REQUEST", 12: "PING_RESPONSE",
           13: "NAV_FOCUS_REQUEST", 14: "NAV_FOCUS_NOTIFICATION", 15: "BYEBYE_REQUEST", 16: "BYEBYE_RESPONSE",
           17: "VOICE_SESSION", 18: "AUDIO_FOCUS_REQUEST", 19: "AUDIO_FOCUS_NOTIFICATION",
           20: "CAR_CONNECTED_DEVICES_REQUEST", 21: "CAR_CONNECTED_DEVICES_RESPONSE", 22: "USER_SWITCH_REQUEST",
           23: "BATTERY_STATUS", 24: "CALL_AVAILABILITY", 25: "USER_SWITCH_RESPONSE",
           26: "SERVICE_DISCOVERY_UPDATE", 255: "UNEXPECTED_MESSAGE", 65535: "FRAMING_ERROR"}
CHANNEL = {0x0000: "MEDIA_DATA", 0x0001: "CODEC_CONFIG", 7: "CHANNEL_OPEN_REQUEST", 8: "CHANNEL_OPEN_RESPONSE",
           0x8000: "SETUP", 0x8001: "START|INPUT|SENSOR_REQ|PAIRING_REQ", 0x8002: "STOP|BINDING|SENSOR_RESP|PAIRING_RESP",
           0x8003: "CONFIG|BINDING_RESP|SENSOR_BATCH|AUTH_DATA", 0x8004: "ACK", 0x8005: "MIC_REQUEST",
           0x8006: "MIC_RESPONSE", 0x8007: "VIDEO_FOCUS_REQUEST", 0x8008: "VIDEO_FOCUS_NOTIFICATION"}


def rv(d, pos):
    v = shift = 0
    while True:
        if pos >= len(d):
            raise ValueError
        b = d[pos]; pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7


def is_msg(d):
    if not d:
        return False
    try:
        pos = 0
        while pos < len(d):
            k, pos = rv(d, pos)
            if k >> 3 == 0:
                return False
            t = k & 7
            if t == 0:
                _, pos = rv(d, pos)
            elif t == 2:
                n, pos = rv(d, pos); pos += n
            elif t == 1:
                pos += 8
            elif t == 5:
                pos += 4
            else:
                return False
            if pos > len(d):
                return False
        return True
    except ValueError:
        return False


def dump(d, depth=0):
    out, pos = [], 0
    try:
        while pos < len(d):
            k, pos = rv(d, pos)
            f, t = k >> 3, k & 7
            if t == 0:
                v, pos = rv(d, pos); out.append("%d:%d" % (f, v))
            elif t == 2:
                n, pos = rv(d, pos); sub = d[pos:pos + n]; pos += n
                if sub and all(32 <= c < 127 for c in sub) and not is_msg(sub):
                    out.append('%d:"%s"' % (f, sub.decode()))
                elif is_msg(sub) and depth < 6:
                    out.append("%d:{%s}" % (f, dump(sub, depth + 1)))
                else:
                    out.append("%d:<%d B %s>" % (f, len(sub), sub[:16].hex()))
            elif t == 1:
                pos += 8; out.append("%d:fix64" % f)
            elif t == 5:
                pos += 4; out.append("%d:fix32" % f)
            else:
                out.append("<?>"); break
    except ValueError:
        out.append("<kaputt>")
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--all", action="store_true")
    a = ap.parse_args()
    data = open(a.trace, "rb").read()
    if not data.startswith(b"AAPTRACE1\n"):
        sys.exit("not a SailAuto trace")
    pos, counts = 10, {}
    hdr = struct.calcsize(">QcBBHII")  # 21 bytes
    while pos + hdr <= len(data):
        ts, d, ch, fl, mid, full, stored = struct.unpack_from(">QcBBHII", data, pos)
        pos += hdr
        body = data[pos:pos + stored]; pos += stored
        name = CONTROL.get(mid, "0x%04x" % mid) if ch == 0 else CHANNEL.get(mid, "0x%04x" % mid)
        key = (d, ch, mid)
        counts[key] = counts.get(key, 0) + 1
        bulk = ch != 0 and mid in (0, 0x8004) or ch == 0 and mid in (11, 12)
        if bulk and not a.all and counts[key] > 3:
            continue
        arrow = "->" if d == b"O" else "<-"
        detail = "" if (mid in (0, 1) or (ch == 0 and mid == 3)) else dump(body)
        print("%9.3f %s ch%-2d %-28s %6d B%s %s" % (ts / 1e6, arrow, ch, name, full,
                                                   " ENC" if fl & 8 else "", detail))
    print("--- counts (dir, channel, id): " + ", ".join(
        "%s%d/0x%04x=%d" % (k[0].decode(), k[1], k[2], v) for k, v in sorted(counts.items(), key=lambda x: -x[1])[:12]))


if __name__ == "__main__":
    main()
