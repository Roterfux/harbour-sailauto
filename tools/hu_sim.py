#!/usr/bin/env python3
"""
Minimal Android Auto head-unit simulator (TCP / "wireless" mode).

Behaves like OpenAuto (aasdk) does: it connects to the phone on port 5277,
is the TLS client, pings every few seconds, offers video/input/sensor/audio
channels, acks every video frame and sends touch events.

Use it to test harbour-sailauto without a car or a Raspberry Pi:

    python3 tools/hu_sim.py --host 192.168.1.23 --seconds 10 --out out.h264
    ffmpeg -i out.h264 -frames:v 1 shot.png

Only Python's standard library is needed.
"""
import argparse
import socket
import ssl
import struct
import sys
import threading
import time

# ---- protocol constants (same values as aasdk) ----------------------------
CH_CONTROL, CH_INPUT, CH_SENSOR, CH_VIDEO, CH_MEDIA_AUDIO, CH_BT = 0, 1, 2, 3, 4, 8
F_FIRST, F_LAST, F_BULK, F_CONTROL, F_ENC = 1, 2, 3, 4, 8

VERSION_REQUEST, VERSION_RESPONSE, SSL_HANDSHAKE, AUTH_COMPLETE = 1, 2, 3, 4
SD_REQUEST, SD_RESPONSE, CH_OPEN_REQUEST, CH_OPEN_RESPONSE = 5, 6, 7, 8
PING_REQUEST, PING_RESPONSE, SHUTDOWN_REQUEST, SHUTDOWN_RESPONSE = 0x0B, 0x0C, 0x0F, 0x10
AV_MEDIA_TS, AV_MEDIA, AV_SETUP_REQ, AV_START, AV_STOP, AV_SETUP_RESP = 0, 1, 0x8000, 0x8001, 0x8002, 0x8003
AV_ACK, VIDEO_FOCUS_REQ, VIDEO_FOCUS_IND = 0x8004, 0x8007, 0x8008
INPUT_EVENT, BINDING_REQ, BINDING_RESP = 0x8001, 0x8002, 0x8003
SENSOR_START_REQ, SENSOR_START_RESP, SENSOR_EVENT = 0x8001, 0x8002, 0x8003


# ---- tiny protobuf ---------------------------------------------------------
def varint(v):
    out = bytearray()
    v &= (1 << 64) - 1
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def f_varint(field, v):
    return varint(field << 3) + varint(v)


def f_bytes(field, b):
    if isinstance(b, str):
        b = b.encode()
    return varint((field << 3) | 2) + varint(len(b)) + b


def pb_parse(data):
    fields, pos = {}, 0
    while pos < len(data):
        key, pos = _rv(data, pos)
        field, wt = key >> 3, key & 7
        if wt == 0:
            val, pos = _rv(data, pos)
        elif wt == 2:
            ln, pos = _rv(data, pos)
            val, pos = data[pos:pos + ln], pos + ln
        elif wt == 1:
            val, pos = struct.unpack_from("<Q", data, pos)[0], pos + 8
        elif wt == 5:
            val, pos = struct.unpack_from("<I", data, pos)[0], pos + 4
        else:
            raise ValueError("wire type %d" % wt)
        fields.setdefault(field, []).append(val)
    return fields


def _rv(d, pos):
    v = shift = 0
    while True:
        b = d[pos]
        pos += 1
        v |= (b & 0x7F) << shift
        if not b & 0x80:
            return v, pos
        shift += 7


# ---- head unit -------------------------------------------------------------
class HeadUnit:
    def __init__(self, args):
        self.args = args
        self.sock = socket.create_connection((args.host, args.port), timeout=10)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # aasdk: SSL_VERIFY_NONE
        self.inc, self.outg = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = ctx.wrap_bio(self.inc, self.outg, server_side=False)
        self.handshaken = False
        self.auth_sent = False
        self.rx = b""
        self.partial = {}
        self.lock = threading.Lock()
        self.pings = self.pongs = 0
        self.session = None
        self.frames = 0
        self.config_msgs = 0
        self.video_bytes = 0
        self.out = open(args.out, "wb") if args.out else None
        self.opened = []
        self.audio_setups = 0
        self.audio_starts = self.audio_stops = 0
        self.audio = {}
        self.focus_log = []
        self.touch_ready = False
        self.focus_at = None
        self.errors = []
        self.night_sent = False
        self.done = False
        self.discovery = open(args.discovery, "rb").read() if args.discovery else None
        self.ch_video, self.ch_input, self.ch_sensor = CH_VIDEO, CH_INPUT, CH_SENSOR
        if self.discovery:
            for raw in pb_parse(self.discovery).get(1, []):
                c = pb_parse(raw)
                cid = c[1][0]
                if 3 in c:
                    av = pb_parse(c[3][0])
                    if av.get(1, [0])[0] == 3 or 4 in av:
                        self.ch_video = cid
                elif 4 in c:
                    self.ch_input = cid
                elif 2 in c:
                    self.ch_sensor = cid

    def log(self, *a):
        print("[hu %6.2f]" % (time.time() - self.t0), *a, flush=True)

    # -- frame I/O
    def send(self, ch, msg_id, payload=b"", enc=True, control=False):
        msg = struct.pack(">H", msg_id) + payload
        flags = (F_ENC if enc else 0) | (F_CONTROL if control else 0)
        with self.lock:
            if len(msg) < 0x4000:
                self._frame(ch, flags | F_BULK, msg, None)
            else:
                for off in range(0, len(msg), 0x4000):
                    chunk = msg[off:off + 0x4000]
                    ft = F_FIRST if off == 0 else (F_LAST if off + len(chunk) >= len(msg) else 0)
                    self._frame(ch, flags | ft, chunk, len(msg) if ft == F_FIRST else None)

    def _frame(self, ch, flags, chunk, total):
        if flags & F_ENC:
            self.tls.write(chunk)
            chunk = self.outg.read()
        hdr = struct.pack(">BBH", ch, flags, len(chunk))
        if total is not None:
            hdr += struct.pack(">I", total)
        self.sock.sendall(hdr + chunk)

    def recv_loop(self):
        while not self.done:
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                self.log("phone closed the connection")
                break
            self.rx += data
            while True:
                if len(self.rx) < 4:
                    break
                ch, flags, size = struct.unpack_from(">BBH", self.rx)
                hl = 8 if (flags & 3) == F_FIRST else 4
                if len(self.rx) < hl + size:
                    break
                total = struct.unpack_from(">I", self.rx, 4)[0] if hl == 8 else None
                payload = self.rx[hl:hl + size]
                self.rx = self.rx[hl + size:]
                if flags & F_ENC:
                    self.inc.write(payload)
                    payload = b""
                    while True:
                        try:
                            payload += self.tls.read(65536)
                        except ssl.SSLWantReadError:
                            break
                ft = flags & 3
                if ft == F_BULK:
                    self.handle(ch, flags, payload)
                elif ft == F_FIRST:
                    self.partial[ch] = [payload, total]
                else:
                    self.partial[ch][0] += payload
                    if ft == F_LAST:
                        msg, total = self.partial.pop(ch)
                        if len(msg) != total:
                            self.errors.append("size mismatch on ch %d" % ch)
                        self.handle(ch, flags, msg)
        self.done = True

    # -- protocol
    def start(self):
        self.t0 = time.time()
        threading.Thread(target=self.recv_loop, daemon=True).start()
        threading.Thread(target=self.ping_loop, daemon=True).start()
        self.send(CH_CONTROL, VERSION_REQUEST, struct.pack(">HH", 1, 1), enc=False)

    def ping_loop(self):
        while not self.done:
            time.sleep(self.args.ping)
            if self.pings - self.pongs > 1:
                self.errors.append("ping timeout")
                self.log("ping timer exceeded (OpenAuto would quit here)")
                self.done = True
                return
            self.pings += 1
            self.send(CH_CONTROL, PING_REQUEST, b"", enc=False)

    def handshake_step(self):
        try:
            self.tls.do_handshake()
            self.handshaken = True
        except ssl.SSLWantReadError:
            pass
        out = self.outg.read()
        if out:
            self.send(CH_CONTROL, SSL_HANDSHAKE, out, enc=False)
        if self.handshaken and not self.auth_sent:
            self.auth_sent = True
            self.log("TLS done:", self.tls.version(), self.tls.cipher()[0])
            self.send(CH_CONTROL, AUTH_COMPLETE, f_varint(1, 0), enc=False)

    def handle(self, ch, flags, msg):
        msg_id = struct.unpack_from(">H", msg)[0]
        p = msg[2:]
        if ch == CH_CONTROL:
            if msg_id == VERSION_RESPONSE:
                major, minor, status = struct.unpack(">HHH", p[:6])
                self.log("version response %d.%d status %d" % (major, minor, status))
                self.handshake_step()
            elif msg_id == SSL_HANDSHAKE:
                if self.handshaken:
                    # aasdk would answer this with a second AUTH_COMPLETE
                    self.errors.append("handshake data after TLS was finished")
                self.inc.write(p)
                self.handshake_step()
            elif msg_id == SD_REQUEST:
                f = pb_parse(p)
                self.log("service discovery from", f.get(4, [b"?"])[0].decode(), "/", f.get(5, [b"?"])[0].decode())
                self.send(CH_CONTROL, SD_RESPONSE, self.discovery or self.sd_response())
            elif msg_id == PING_RESPONSE:
                self.pongs += 1
            elif msg_id == 0x12:  # AUDIO_FOCUS_REQUEST -> grant like a car
                req = pb_parse(p).get(1, [0])[0]
                state = {1: 1, 2: 2, 3: 2, 4: 3}.get(req, 1)  # GAIN/TRANSIENT/LOSS
                self.focus_log.append((req, state))
                self.log("audio focus request %d -> %d" % (req, state))
                self.send(CH_CONTROL, 0x13, f_varint(1, state) + f_varint(2, 0))
            elif msg_id == SHUTDOWN_REQUEST:
                self.log("phone requests shutdown")
                self.send(CH_CONTROL, SHUTDOWN_RESPONSE, b"")
                self.done = True
            elif msg_id == SHUTDOWN_RESPONSE:
                self.done = True
            else:
                self.log("control msg 0x%04x" % msg_id)
            return
        if msg_id == CH_OPEN_REQUEST:
            f = pb_parse(p)
            self.log("channel open request ch %d (prio %d)" % (f[2][0], f[1][0]))
            if not flags & F_CONTROL:
                self.errors.append("channel open without CONTROL flag")
            self.opened.append(ch)
            self.send(ch, CH_OPEN_RESPONSE, f_varint(1, 0), control=True)
            return
        if ch == self.ch_video:
            self.handle_video(msg_id, p)
        elif ch in (self.ch_input, self.ch_sensor, CH_BT):
            self.handle_other(ch, msg_id, p)
        elif msg_id == AV_START:
            self.log("audio START on ch %d %s" % (ch, pb_parse(p)))
            self.audio_starts += 1
        elif msg_id == AV_STOP:
            self.log("audio STOP on ch %d" % ch)
            self.audio_stops += 1
        elif msg_id in (AV_MEDIA_TS, AV_MEDIA):
            data = p[8:] if msg_id == AV_MEDIA_TS else p
            self.audio.setdefault(ch, bytearray()).extend(data)
            self.send(ch, AV_ACK, f_varint(1, 1) + f_varint(2, 1))
        elif msg_id == AV_SETUP_REQ:
            # audio channel: accept PCM setup like a car does
            self.send(ch, AV_SETUP_RESP, f_varint(1, 2) + f_varint(2, 4) + f_varint(3, 0))
            self.audio_setups += 1
        else:
            self.log("ch %d msg 0x%04x" % (ch, msg_id))

    def handle_other(self, ch, msg_id, p):
        if msg_id == 0x8001 and ch == CH_BT:
            f = pb_parse(p)
            self.log("bluetooth pairing request from", f.get(1, [b"?"])[0].decode(), "method", f.get(2, [0])[0])
            self.send(ch, 0x8002, f_varint(1, 0) + f_varint(2, 1))
        elif ch == self.ch_input and msg_id == BINDING_REQ:
            self.send(ch, BINDING_RESP, f_varint(1, 0))
            self.touch_ready = True
        elif ch == self.ch_sensor and msg_id == SENSOR_START_REQ:
            f = pb_parse(p)
            self.send(ch, SENSOR_START_RESP, f_varint(1, 0))
            if f[1][0] == 13:
                self.send(ch, SENSOR_EVENT, f_bytes(13, f_varint(1, 0)))
            elif f[1][0] == 10:
                self.send(ch, SENSOR_EVENT, f_bytes(10, f_varint(1, 1 if self.args.night else 0)))
        else:
            self.log("ch %d msg 0x%04x" % (ch, msg_id))

    def handle_video(self, msg_id, p):
        if msg_id == AV_SETUP_REQ:
            self.send(self.ch_video, AV_SETUP_RESP, f_varint(1, 2) + f_varint(2, 1) + f_varint(3, 0))
        elif msg_id == AV_START:
            f = pb_parse(p)
            self.session = f.get(1, [0])[0]
            self.log("video start, session", self.session)
        elif msg_id == VIDEO_FOCUS_REQ:
            self.send(self.ch_video, VIDEO_FOCUS_IND, f_varint(1, 1) + f_varint(2, 0))
            self.focus_at = time.time()
        elif msg_id in (AV_MEDIA, AV_MEDIA_TS):
            data = p if msg_id == AV_MEDIA else p[8:]
            if msg_id == AV_MEDIA:
                self.config_msgs += 1
            else:
                self.frames += 1
            self.video_bytes += len(data)
            if self.out and not self.out.closed:
                self.out.write(data)
            self.send(self.ch_video, AV_ACK, f_varint(1, self.session or 0) + f_varint(2, 1))
        elif msg_id == AV_STOP:
            self.log("video stop")
        else:
            self.log("video msg 0x%04x" % msg_id)

    def sd_response(self):
        w, h = 800, 480
        vcfg = f_varint(1, 1) + f_varint(2, 1) + f_varint(3, 0) + f_varint(4, 0) + f_varint(5, 140)
        video = f_varint(1, CH_VIDEO) + f_bytes(3, f_varint(1, 3) + f_bytes(4, vcfg) + f_varint(5, 1))
        touch = f_varint(1, w) + f_varint(2, h)
        inp = f_varint(1, CH_INPUT) + f_bytes(4, f_varint(1, 3) + f_bytes(2, touch))
        sensor = f_varint(1, CH_SENSOR) + f_bytes(2, f_bytes(1, f_varint(1, 13)) + f_bytes(1, f_varint(1, 10)))
        acfg = f_varint(1, 48000) + f_varint(2, 16) + f_varint(3, 2)
        audio = f_varint(1, CH_MEDIA_AUDIO) + f_bytes(3, f_varint(1, 1) + f_varint(2, 3) + f_bytes(3, acfg))
        speech = f_varint(1, 5) + f_bytes(3, f_varint(1, 1) + f_varint(2, 1) +
                                          f_bytes(3, f_varint(1, 16000) + f_varint(2, 16) + f_varint(3, 1)))
        mic = f_varint(1, 7) + f_bytes(5, f_varint(1, 1) + f_bytes(2, f_varint(1, 16000) + f_varint(2, 16) + f_varint(3, 1)))
        bt = f_varint(1, CH_BT) + f_bytes(6, f_bytes(1, "AA:BB:CC:DD:EE:FF") + f_bytes(2, varint(2) + varint(4)))
        body = b"".join(f_bytes(1, c) for c in (audio, speech, video, inp, sensor, mic, bt))
        body += f_bytes(2, "OpenAuto") + f_bytes(3, "Universal") + f_bytes(4, "2018") + f_bytes(5, "20180301")
        body += f_varint(6, 1) + f_bytes(7, "f1x") + f_bytes(8, "OpenAuto Autoapp") + f_bytes(9, "1") + f_bytes(10, "1.0")
        body += f_varint(11, 0) + f_varint(12, 0)
        return body

    def touch(self, action, x, y):
        loc = f_varint(1, x) + f_varint(2, y) + f_varint(3, 0)
        ev = f_bytes(1, loc) + f_varint(2, 0) + f_varint(3, action)
        self.send(self.ch_input, INPUT_EVENT, f_varint(1, int(time.time() * 1e6)) + f_bytes(3, ev))

    def tap(self, x, y, drag_to=None):
        self.touch(0, x, y)
        time.sleep(0.1)
        if drag_to:
            for i in range(1, 6):
                self.touch(2, x + (drag_to[0] - x) * i // 5, y + (drag_to[1] - y) * i // 5)
                time.sleep(0.05)
            x, y = drag_to
        self.touch(1, x, y)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=5277)
    ap.add_argument("--seconds", type=float, default=8)
    ap.add_argument("--out", help="write the received H.264 stream here")
    ap.add_argument("--ping", type=float, default=2.0, help="ping interval (OpenAuto: 5 s)")
    ap.add_argument("--night", action="store_true")
    ap.add_argument("--audio-out", help="write received audio as <prefix>-ch<N>.wav")
    ap.add_argument("--sound-test", action="store_true",
                    help="tap Play on the SailAuto music page (media sound test), 2 s after video starts")
    ap.add_argument("--discovery", help="replay a ServiceDiscoveryResponse recorded by SailAuto "
                    "(discovery.bin from ~/Documents/SailAuto/<session>/) instead of the built-in one")
    ap.add_argument("--tap", action="append", default=[],
                    help="x,y[@seconds] tap on the 800x480 car screen, e.g. 300,250@3")
    args = ap.parse_args()

    hu = HeadUnit(args)
    hu.start()
    taps = []
    for t in args.tap:
        pos, _, at = t.partition("@")
        x, y = (int(v) for v in pos.split(","))
        taps.append((float(at or 3), x, y))
    if args.sound_test:
        taps += [(2.0, 300, 250), (3.0, 400, 330)]   # open Musik, press Play
    taps.sort()

    end = time.time() + args.seconds
    while time.time() < end and not hu.done:
        if taps and hu.touch_ready and hu.focus_at and time.time() - hu.focus_at >= taps[0][0]:
            _, x, y = taps.pop(0)
            hu.log("tap", x, y)
            hu.tap(x, y)
        time.sleep(0.02)

    elapsed = time.time() - (hu.focus_at or time.time())
    if args.audio_out:
        import wave
        fmt = {CH_MEDIA_AUDIO: (48000, 2), 5: (16000, 1)}
        for ch, pcm in hu.audio.items():
            rate, nch = fmt.get(ch, (48000, 2))
            with wave.open("%s-ch%d.wav" % (args.audio_out, ch), "wb") as w:
                w.setnchannels(nch); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(pcm))
    hu.done = True
    if hu.out:
        hu.out.close()
    print("---")
    print("channels opened : %s" % hu.opened)
    print("audio setups    : %d" % hu.audio_setups)
    print("audio start/stop: %d/%d, focus %s" % (hu.audio_starts, hu.audio_stops, hu.focus_log))
    for ch, pcm in hu.audio.items():
        print("audio ch%d bytes : %d" % (ch, len(pcm)))
    print("codec config msgs: %d" % hu.config_msgs)
    print("video frames    : %d (%.1f fps)" % (hu.frames, hu.frames / elapsed if elapsed > 0 else 0))
    print("video bytes     : %d" % hu.video_bytes)
    print("pings/pongs     : %d/%d" % (hu.pings, hu.pongs))
    print("errors          : %s" % (hu.errors or "none"))
    ok = hu.frames > 0 and hu.config_msgs == 1 and not hu.errors and hu.pongs >= hu.pings - 1
    print("RESULT          :", "OK" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
