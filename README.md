# SailAuto (harbour-sailauto) – MVP

Sailfish-OS-App, die sich gegenüber einer Android-Auto-Head-Unit wie ein
Android-Telefon verhält: Sie spricht das Android-Auto-Protokoll (AAP),
schickt eine eigene Auto-Oberfläche als H.264-Video und nimmt Touch-Eingaben
zurück. Testziel ist **OpenAuto** (Raspberry Pi / Linux-PC), kein echtes Auto.

> Kein Google-Produkt. Kein Android-Code. Eigene Implementierung des
> Protokolls, Werte gegen aasdk (die Bibliothek unter OpenAuto) geprüft.

## Was das MVP kann

| Bereich | Stand |
|---|---|
| Transport | TCP, Telefon lauscht auf Port 5277 (OpenAuto „Wireless“/„Connect to device“) |
| Version + TLS | Telefon = TLS-Server (TLS 1.2), eigenes Zertifikat oder selbstsigniertes Dev-Zertifikat |
| Service Discovery | liest Video-Auflösung, Ränder, DPI, Touch-Größe der Head Unit |
| Video | QPainter-Oberfläche → GStreamer → H.264 (droidvenc auf dem Telefon, x264/openh264 als Fallback), Flusskontrolle über ACKs |
| Touch | Tippen/Ziehen der Head Unit landet in der Auto-Oberfläche |
| Sensoren | Nachtmodus (dunkles Theme), Fahrstatus |
| Keepalive | Antwortet auf Pings (OpenAuto trennt sonst nach ~10 s) |
| Sailfish-UI | Silica-Seite: Start/Stop, IP-Adresse, Status, Protokoll, Cover |

Noch **nicht** drin: Audio, Mikrofon, Tasten/Drehregler, echte Apps
(Navigation/Musik/Telefon sind Platzhalter), USB, kabelloses AA mit echtem Auto.

## Aufbau

```
src/aap/protobuf.h     Mini-Protobuf (kein protoc nötig)
src/aap/protocol.h     Kanal- und Nachrichten-IDs (aus aasdk_proto geprüft)
src/aap/cryptor.*      OpenSSL über Memory-BIOs (TLS läuft in AAP-Nachrichten)
src/aap/session.*      Zustandsmaschine Telefon-Seite, Framing, Verschlüsselung
src/video/carscreen.*  Auto-Oberfläche (800x480 / 720p / 1080p, Sicherheitsrand)
src/video/h264encoder.* GStreamer appsrc → Encoder → h264parse → appsink
src/controller.*       TCP-Server, verbindet alles, QML-Schnittstelle
qml/                   Silica-Oberfläche auf dem Telefon
tools/hu_sim.py        Head-Unit-Simulator (verhält sich wie OpenAuto)
```

Ablauf einer Sitzung:

```
HU  VERSION_REQUEST ───────────────▶ Telefon   VERSION_RESPONSE ◀──
HU  SSL_HANDSHAKE  ◀──────────────▶ Telefon   (HU = TLS-Client)
HU  AUTH_COMPLETE  ───────────────▶
    SERVICE_DISCOVERY_REQUEST ◀──── Telefon   ──▶ RESPONSE (Kanäle)
    CHANNEL_OPEN (Video, Input, Sensor) ◀── Telefon
    Video: SETUP → START → VIDEO_FOCUS → H.264-Frames, HU ACKt jeden Frame
    Input: BINDING → Touch-Events von der HU
    PING alle 5 s von der HU → PING_RESPONSE
```

## Testen ohne Telefon (Linux)

```bash
sudo apt install qtbase5-dev qt5-qmake libssl-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly ffmpeg
```

```bash
mkdir build && cd build && qmake CONFIG+=desktop ../harbour-sailauto.pro && make -j
```

```bash
QT_QPA_PLATFORM=offscreen ./harbour-sailauto
```

In einem zweiten Terminal (tippt nach 2 s auf „Musik“):

```bash
python3 tools/hu_sim.py --seconds 8 --out out.h264 --tap 300,250@2
```

```bash
ffmpeg -i out.h264 -vf "select=eq(n\,120)" -frames:v 1 shot.png
```

Ergebnis hier im Container: TLS 1.2, 3 Kanäle offen, ~31 fps, Pings
beantwortet, Touch wechselt die Seite, Nachtmodus schaltet das Theme.

## Testen mit OpenAuto

1. OpenAuto auf Raspberry Pi oder Linux-PC bauen (f1xpl/openauto bzw.
   OpenDsh / Crankshaft enthalten es fertig).
2. Telefon und OpenAuto ins selbe WLAN (oder Pi in den Hotspot des Telefons).
3. Auf dem Telefon SailAuto öffnen, Schalter „Auf Head Unit warten“ an.
4. In OpenAuto „Wireless“ / „Connect to device“ → IP aus der App eingeben.
5. Im SailAuto-Protokoll sieht man jeden Schritt; Fehler bitte von dort kopieren.

## Zertifikat

- Ohne Dateien: selbstsigniertes Dev-Zertifikat (OpenAuto prüft das Telefon nicht).
- Eigenes Zertifikat: `cert.pem` (Leaf + ggf. Kette dahinter) und `key.pem`
  nach `~/.local/share/org.roterfux/harbour-sailauto/` kopieren, neu verbinden.
  Status-Zeile zeigt dann „eigenes“.
- Ein echtes Auto prüft die Kette gegen Googles Android-Auto-CA. Das
  Zertifikat dafür kommt vom Nutzer; die App lädt es nur.

## RPM bauen

GitHub Actions (`.github/workflows/build.yml`) baut mit
`coderus/sailfishos-platform-sdk:5.2.0.15` für aarch64, armv7hl und i486 und
hängt die RPMs an ein Pre-Release `build-<sha>`. Lokal mit sfdk:

```bash
sfdk -c target=SailfishOS-5.2.0.15-aarch64 build
```

Nicht Harbour-tauglich (GStreamer/OpenSSL direkt gelinkt, offener Port) →
Verteilung über OpenRepos/Chum.

## Aufwand bis zum echten Einsatz

Schätzung in Claude-Sitzungen (je 1–3 h). Dazu kommt deine Testzeit am
Gerät – Claude sieht weder Telefon noch Auto, jede Runde braucht Logs/Screenshots von dir.

| Phase | Inhalt | Claude | Risiko |
|---|---|---|---|
| 0 ✅ | Dieses MVP: Protokoll, TLS, Video, Touch, Sensoren, Simulator | 1 | – |
| 1 | Am Telefon gegen OpenAuto zum Laufen bringen: droidvenc-Format/Latenz, Keyframes, KeepAlive, Bugs aus echten Logs | 1–2 | mittel (Encoder je Gerät anders) |
| 2 | Audio: Medien-/Sprach-/Systemkanal (PCM aus PulseAudio), Audio-Fokus, Mikrofon für Anrufe | 2–3 | mittel |
| 3 | Echte Oberfläche als QML offscreen (QQuickRenderControl): MPRIS-Mediensteuerung, Anrufe (oFono/voicecall), Benachrichtigungen, Drehregler/Tasten | 3–5 | niedrig–mittel |
| 4a | Echtes Auto per USB: Telefon muss USB-Gadget im AOA-Modus werden (Accessory-Funktion im Kernel, usb-moded-Konfiguration, root) | 3–6 | **hoch**, geräteabhängig |
| 4b | Kabelloses Android Auto: Bluetooth-RFCOMM-Profil (BlueZ), WLAN-Daten tauschen, Telefon als TCP-Client zur HU | 3–5 | mittel–hoch |
| 5 | Navigation mit Karte (z. B. Pure Maps/OSM Scout Server einbinden) | 3–6 | mittel |

Grob: **MVP gegen OpenAuto am Telefon ≈ 2–3 Sitzungen, „im eigenen Auto mit
Ton und Grundfunktionen“ ≈ 12–20 Sitzungen.** Größter Unsicherheitsfaktor ist
USB-AOA auf dem konkreten Sailfish-Gerät (Kernel-Support) bzw. ob das Auto
dein Zertifikat akzeptiert.

## Lizenz

GPLv3 (Protokollwissen aus aasdk/OpenAuto, beide GPLv3).
