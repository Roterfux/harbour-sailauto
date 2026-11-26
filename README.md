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
| Video | QPainter-Oberfläche → H.264. Versucht GStreamer (droidvenc, x264, openh264), sonst eingebauter Encoder: geänderte 16×16-Blöcke unkomprimiert (I_PCM), unveränderte als Skip – verlustfrei, ohne Abhängigkeiten. Flusskontrolle über ACKs |
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
src/video/h264encoder.* GStreamer appsrc → Encoder → h264parse → appsink, Fallback-Kette
src/video/pcmh264.*    eingebauter H.264-Encoder (I_PCM + P_Skip), immer verfügbar
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

## OpenAuto unter Windows (WSL2) – getestet

Das Original `f1xpl/openauto` baut mit aktuellem Boost nicht mehr. Der
gepflegte Fork `opencardev` (Crankshaft NG) baut auf Ubuntu 24.04 und
funktioniert mit SailAuto (getestet: aasdk `9bf6adf`, openauto `4cc739b`,
Video 800x480@60 und Touch).

Windows 10/11, PowerShell als Admin:

```powershell
wsl --install -d Ubuntu-24.04
```

Danach im Ubuntu-Fenster:

```bash
sudo apt update && sudo apt install -y git build-essential cmake pkg-config libboost-all-dev libusb-1.0-0-dev libssl-dev libprotobuf-dev protobuf-compiler libabsl-dev qtbase5-dev qt5-qmake qtmultimedia5-dev libqt5multimedia5-plugins libqt5multimediawidgets5 qtconnectivity5-dev librtaudio-dev libpulse-dev libtag1-dev libblkid-dev libgps-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav
```

```bash
git clone https://github.com/opencardev/aasdk && cd aasdk && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc) && sudo make install && sudo ldconfig && cd ~
```

```bash
git clone https://github.com/opencardev/openauto && cd openauto && mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -DNOPI=ON .. && make -j$(nproc) && cd ~
```

**Wichtig unter WSL:** OpenAuto verbindet sich beim Start mit einem GPS-Dienst
auf `127.0.0.1:2947`. Unter WSL (mirrored) hängt das ewig, OpenAuto sendet dann
nie etwas. Abhilfe: in einem eigenen Fenster einen Dummy offen lassen:

```bash
nc -lk 127.0.0.1 2947
```

Audio von OpenAuto abschalten (SailAuto hat noch keinen Ton), im Startordner:

```bash
cd ~ && printf '[AudioChannel]\nMediaEnabled=false\nGuidanceEnabled=false\nSystemEnabled=false\nTelephonyEnabled=false\n\n[Audio]\nOutputBackendType=1\n' > ~/openauto.ini
```

Starten (der Fork zeigt den WLAN-Knopf nur, wenn diese Datei existiert;
`gateway_wlan0` trägt die IP vor und verbindet sofort):

```bash
touch /tmp/mobile_hotspot_detected && echo 192.168.1.23 > /tmp/gateway_wlan0
```

```bash
~/openauto/build/bin/autoapp-*
```

Sicherheitshinweis „OK“ → Kachel oben links („WiFi“) → verbindet zu Port 5277.
Ohne Telefon zuerst gegen den Linux-Build von SailAuto in derselben WSL
testen: IP `127.0.0.1`.

## Testen mit OpenAuto

1. OpenAuto auf Raspberry Pi, Linux-PC oder Windows/WSL2 (siehe oben)
   bauen. Crankshaft-NG-Images enthalten es fertig.
2. Telefon und OpenAuto ins selbe WLAN (oder Pi in den Hotspot des Telefons).
3. Auf dem Telefon SailAuto öffnen, Schalter „Auf Head Unit warten“ an.
4. In OpenAuto „Wireless“ / „Connect to device“ → IP aus der App eingeben.
5. Im SailAuto-Protokoll sieht man jeden Schritt; Fehler bitte von dort kopieren.

## USB (Auto per Kabel) – experimentell

Ab 0.1.5. Wie ein Android-Telefon meldet sich das Telefon am USB-Host (Auto
oder OpenAuto am PC) als „Android Open Accessory“ (18d1:2d00); danach läuft
dasselbe Protokoll wie über WLAN, nur über `/dev/usb_accessory`.
Voraussetzung: Kernel mit `CONFIG_USB_CONFIGFS_F_ACC` (Xperia 10 III: ja).

Auf dem Telefon (root):

```bash
devel-su sh /usr/share/harbour-sailauto/aoa-gadget.sh on
```

Dann in SailAuto „USB (Auto per Kabel)“ einschalten und Kabel einstecken.
Zurück zu normalem USB (MTP/Entwicklermodus):

```bash
devel-su sh /usr/share/harbour-sailauto/aoa-gadget.sh off
```

Notfalls: Kabel ab/an oder Neustart. Die App läuft dafür ohne Sailjail-Sandbox.

Probe am PC vor dem Auto: OpenAuto im USB-Modus. Unter WSL das Telefon mit
[usbipd-win](https://github.com/dorssel/usbipd-win) durchreichen
(`usbipd attach --wsl --auto-attach --busid <id>`) und `autoapp` mit
`sudo -E` starten (libusb braucht Rechte).

## Telefon-Ton im Auto

Ab 0.3.0, Schalter „Telefon-Ton ins Auto“ (Standard: an). Sobald das Auto-Bild
läuft, legt die App in PulseAudio einen virtuellen Ausgang „SailAuto-Auto“ an,
macht ihn zum Standard und leitet alle laufenden Wiedergaben dorthin um
(Musik-Apps, Pure Maps, …). Dessen Monitor wird aufgenommen (48 kHz Stereo)
und über den Medienkanal ans Auto geschickt: sobald etwas spielt Audio-Fokus +
START, nach 3 s Stille STOP + Fokus zurück (Radio kommt wieder). Beim Trennen,
Beenden oder SIGTERM wird alles zurückgestellt; ein Überbleibsel nach einem
Absturz wird beim nächsten Start entfernt. Im Log: `Ton: "<App>" … spielt im
Auto`, oder `… wird vom System immer wieder zurückgeholt`, falls die
Sailfish-Audiorichtlinie die Umleitung verhindert.

Noch nicht: Telefonate, Mikrofon, Steuerung der Player vom Auto aus.

## Musiksteuerung

Ab 0.4.0. Die App findet den Musik-Player am Telefon über MPRIS (Sailfish-
Mediaplayer und andere Player mit MPRIS-Unterstützung). Die Seite „Musik“ im
Auto zeigt Cover, Titel, Interpret, Album, Fortschritt und ⏮ ⏯ ⏭.
Lenkradtasten: die App bindet genau die Tasten, die das Auto anbietet
(Log: `Dienst …: Touch/Tasten Tasten: …`), und gibt PLAY_PAUSE/PLAY/PAUSE/
NEXT/PREVIOUS an den Player weiter. Ohne Player bleibt der Knopf der Tontest.

Kombiinstrument: Titel dort nur, wenn das Auto einen Musikstatus-Dienst
anbietet – der Touran (MIB2) tut das nicht.

## Tontest

Im Auto auf dem SailAuto-Bildschirm „Musik“ → Play: eine Test-Melodie über den
Medienkanal (48 kHz Stereo); erster Ton nur links, zweiter nur rechts, dann
beide. Pause stoppt. Am Telefon im Pull-down-Menü: „Tontest Musik“ und
„Tontest Ansage“ (3 Pieptöne auf dem Navigationsansage-Kanal, das Auto sollte
das Radio leiser machen). Ablauf wie bei Android: Audio-Fokus anfragen →
START → PCM in Echtzeit im ACK-Fenster des Autos → STOP → Fokus freigeben.
Im Log: `Audio-Fokus vom Auto: …`, `Tontest …`.

Am PC: `python3 tools/hu_sim.py --sound-test --audio-out snd` schreibt den
empfangenen Ton als `snd-ch<N>.wav`.

## Diagnose ohne Auto

Jede Sitzung (WLAN oder USB) wird aufgezeichnet in
`~/Documents/SailAuto/<Datum-Uhrzeit>/`:

| Datei | Inhalt |
|---|---|
| `log.txt` | alles lesbar: jede Nachricht beider Richtungen mit Namen und dekodiertem Inhalt, TLS-Details (Version, Cipher, Zertifikat des Autos), Fehler, „HÄNGT“-Warnungen nach 10 s ohne Fortschritt, Zusammenfassung am Ende |
| `trace.bin` | alle Nachrichten roh (ausgehendes Video gekürzt) |
| `discovery.bin` / `.txt` | was das Auto anbietet (Kanäle, Audio-Formate, Bluetooth, Hersteller) |

Am PC auswerten bzw. das Auto nachspielen:

```bash
python3 tools/aap_trace.py ~/Documents/SailAuto/<Sitzung>/trace.bin
```

```bash
python3 tools/hu_sim.py --discovery discovery.bin --seconds 10
```

Holen vom Telefon (WLAN, SSH an):

```bash
scp -r defaultuser@<telefon-ip>:Documents/SailAuto ./sailauto-logs
```

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
