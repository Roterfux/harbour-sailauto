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

Ab 0.4.2: Sendet das Auto 15 s nach dem Öffnen nichts (z. B. nach einer
abgebrochenen Sitzung), meldet die App das USB-Gerät kurz ab und wieder an –
wie Kabel neu einstecken (höchstens 3× hintereinander). Von Hand: Pull-down
„USB neu verbinden“.

Probe am PC vor dem Auto: OpenAuto im USB-Modus. Unter WSL das Telefon mit
[usbipd-win](https://github.com/dorssel/usbipd-win) durchreichen
(`usbipd attach --wsl --auto-attach --busid <id>`) und `autoapp` mit
`sudo -E` starten (libusb braucht Rechte).

## Statusleiste, Apps, Benachrichtigungen

Ab 0.6.0.

- **Statusleiste** im Auto (oben rechts): Akku in % (grün = lädt, rot < 20 %),
  Mobilnetz (Balken, 2G–5G, R = Roaming), WLAN, Bluetooth (blau = Gerät
  verbunden), Uhr. Quellen: sysfs, oFono, ConnMan, BlueZ; alle 10 s.
- **Apps**: neue Kachel „Apps“ im Auto. Welche Apps dort stehen, legst du am
  Telefon fest: Pull-down „Apps fürs Auto“ (Musik-/Radio-Apps sind
  vorausgewählt). Antippen im Auto startet die App am Telefon; bei Musik-/Radio-
  Apps springt das Auto auf „Musik“ und steuert den Player dieser App (MPRIS).
  Die App selbst im Auto anzeigen geht unter Sailfish nicht.
- **Benachrichtigungen** (Schalter „Benachrichtigungen im Auto“, Standard an):
  neue Benachrichtigungen als Banner, 8 s, Antippen schließt. Der Text
  erscheint nur, wenn das Auto „steht“ meldet (Fahrstatus-Sensor), sonst nur
  App und Titel. Die App liest sie als D-Bus-Monitor mit. Im Log stehen nur
  App und Länge, nicht der Inhalt.

## Eigene Navigation (ab 0.15.0)

Standard: SailAuto navigiert selbst (Schalter in „Ziele“). Ziel im Auto wählen
(Zuhause/Arbeit/Adresse/Kontakte/letzte Ziele) → Route sofort, ohne Telefon.
Route: offline über OSM Scout Server (`/v2/route`, Valhalla), sonst online über
den Valhalla-Server der FOSSGIS (OpenStreetMap). Anzeige: nächstes Manöver mit
Entfernung und Straße (oben links), Restweg/-zeit/Ankunft klein oben rechts
mit rundem ✕ zum Beenden, blaue Routenlinie auf der Live-Karte. Weicht man ab (3 Messungen > 45 m), wird neu berechnet.
Position: Auto-GPS, sonst Telefon-GPS. Noch keine Sprachansagen.

Karte (ab 0.18.0): selbst gezeichnet aus Vektorkacheln (OpenMapTiles-Schema),
offline von OSM Scout Server (`/v1/mbgl/tile`), sonst online von OpenFreeMap
(ohne Schlüssel), sonst Bildkacheln von openstreetmap.org. Tag- und Nachtfarben
wie bei OsmAnd. Schrift wie bei OsmAnd erst nach dem Kippen, immer aufrecht und
ohne Überlappung, nach Wichtigkeit: Orte, Straßennamen entlang der Straße (nur
wo sie gerade genug ist), Straßennummern als Schild (A1, B17), Seen. Nah am Auto
größer. Der Kartenaufbau läuft im Hintergrund, das Bild hängt nie.
Ab 0.19.0: eigene Position als kleines 3D-Auto (von hinten, perspektivisch zur
Karte), Karte 1,5-fach schärfer gezeichnet, größere Schrift. Einstellungen am
Telefon (Ziele → Karte im Auto): Auto oder Pfeil, Ausschnitt Nah/Normal/Weit,
Karte flüssig (volle Bildrate) und Stromsparen.
Ab 0.19.1 für Linkslenker: Blättern in Listen, „Liste“, „Radio & Musik“ und „Ziel“
liegen links, näher am Fahrer; Auto-Symbol kleiner.
Ab 0.26.0: Spurempfehlung unter dem Abbiegehinweis (Spuren aus OpenStreetMap
`turn:lanes`, von Valhalla im OSRM-Format geholt; zu nehmende Spuren kräftig,
andere blass; ab 1,5 km vor dem Abbiegen). Stau-Zeitverlust (TomTom) steckt in
Restzeit und Ankunft, „+5 min Stau“ in Orange.
Ab 0.22.1: Karte offline. Vektorkacheln von OpenFreeMap bleiben am Telefon
(`~/.cache/harbour-sailauto/vt`, höchstens 400 MB, älteste fliegen zuerst). Bei
Routenstart wird die ganze Strecke vorgeladen (Einstellung „Karte entlang der
Route vorladen“). Ohne Internet: Kacheln aus dem Speicher, letzte Kartenadresse
gemerkt.
Ab 0.22.0: Tempolimit als Schild neben dem Tempo (OpenStreetMap maxspeed über
Valhalla `trace_attributes`: offline OSM Scout Server, sonst FOSSGIS). Mit Route
für die nächsten 50 km im Voraus, ohne Route alle 15 s aus den letzten
Positionen. Zu schnell (> Limit + 3): Tempo rot.
Ab 0.21.1: Zoom stufenlos nach Tempo (langsam = näher, schnell = weiter weg),
Tempo unten links, Ziel als Stecknadel mit Zielflagge.
Ab 0.19.2 im Tunnel (kein GPS > 2 s): das Auto fährt mit der Geschwindigkeit
weiter (Tacho-Sensor des Autos, sonst letzte GPS-Geschwindigkeit), bei aktiver
Route entlang der Route, sonst geradeaus; die Navigation zählt mit. Wiederholt
das Auto nur die alte Position, gilt das auch als „kein GPS“. Höchstens 10 min.

### Stau und Unfälle (ab 0.21.0)

Mit eigenem, kostenlosem TomTom-Schlüssel (developer.tomtom.com, ohne
Kreditkarte; Einstellungen → „Stau und Unfälle“): während der eigenen
Navigation alle 5 min die Verkehrsmeldungen für die nächsten 60 km der Route
(TomTom Traffic Incidents, ~12 Abfragen pro Fahrstunde, Zähler pro Monat in den
Einstellungen). Nur Meldungen auf der Route und in Fahrtrichtung zählen (Stau
auf der Gegenfahrbahn nicht). Im Auto: Warnkarte unter dem Abbiegehinweis
(„Stau in 2,4 km · 1,8 km lang · +7 min · A1“), Stau orange/rot auf der Route,
Unfälle als roter Punkt; auf anderen Seiten einmal als Banner.

## Navigation mit Pure Maps

Ab 0.7.0. Die Seite „Navigation“ im Auto zeigt, was Pure Maps am Telefon
navigiert (über dessen D-Bus-Schnittstelle): Abbiege-Symbol (aus Pure Maps),
Entfernung, Straße, Anweisung, unten Restweg, Restzeit, Ankunft und Fortschritt.
Startet eine Route, springt das Auto auf diese Seite; auf anderen Seiten steht
das nächste Manöver klein oben links. Läuft Pure Maps nicht, gibt es einen
Knopf „Pure Maps starten“. Ziel wählen weiterhin am Telefon.
Sprachansagen von Pure Maps laufen mit „Telefon-Ton ins Auto“ über den
Medienkanal ins Auto (eigener Ansagekanal: später).

**Ziel eingeben (ab 0.10.0):** Ohne Route zeigt die Seite „Navigation“
Zuhause, Arbeit (am Telefon: Pull-down „Ziele“), „Adresse eingeben“ (Tastatur,
nur im Stand), „Kontakte“ (Kontakte mit Adresse) und die letzten 5 Ziele.
Antippen schickt die Adresse an Pure Maps (startet es bei Bedarf). Pure Maps
erlaubt von außen nur „Suchen“: am Telefon das Ergebnis antippen und „Route“,
dann im Auto „Route bereit – Navigation starten“.

**Ziel im Auto wählen (ab 0.12.0):** Suche über OSM Scout Server (offline,
`/v1/search`, nach Position gewichtet); die Treffer stehen im Auto, Antippen
schickt den Punkt an Pure Maps (ShowPoi). Am Telefon bleibt ein Tipp auf
„Navigieren“, dann im Auto „Navigation starten“. Ohne OSM Scout Server sucht
Pure Maps wie bisher am Telefon.

**Live-Karte (ab 0.11.0, optional, Pull-down „Ziele“ → „Live-Karte“):** Karte
geneigt (3D), Fahrtrichtung oben, Auto unten in der Mitte, Position vom GPS des
Autos, sonst (ab 0.12.0) vom Telefon-GPS; zwischen den Messungen weitergerechnet, Zoom nach Tempo. Kacheln von
OSM Scout Server (`http://127.0.0.1:8553/v1/tile?…`, Adresse änderbar), Tag/Nacht
folgt dem Auto. Mit Route: Abbiege-Karte oben links, Restweg unten links.
Gezeichnet mit QPainter (kein OpenGL); braucht spürbar Rechenleistung.

OSM Scout (Karry) bietet keine Navigationsdaten über D-Bus an, nur
„Seite öffnen“ und „Geo-Link öffnen“ – deshalb Pure Maps.

## Anrufe und Parkplatz

Ab 0.8.0.

- **Eingehender Anruf**: Musik pausiert, das Auto zeigt den Anrufer
  (Name und Bild aus dem Adressbuch, sonst Nummer) mit vier Knöpfen:
  **Ablehnen**, **Stumm** (Klingelton aus, Musik bleibt aus), **Stumm + Musik**
  (Klingelton aus, Musik läuft weiter), **Annehmen**. Nach „Stumm“ bleibt unten
  eine Leiste mit Annehmen/Ablehnen. Während des Gesprächs: Leiste mit Dauer und
  „Auflegen“. Lenkrad: Hörer-Taste = annehmen, Auflegen-Taste = ablehnen/
  auflegen. Danach läuft die Musik weiter; verpasste Anrufe als Banner.
  Der Gesprächston läuft wie bisher über Bluetooth (Freisprechen des Autos).
  Technik: Sailfish-voicecall-Dienst (D-Bus), Adressbuch über QtContacts.
  Im Log steht nur der Zustand, nicht die Nummer.
- **Telefonbuch** (ab 0.9.0, Kachel „Telefon“ im Auto): Reiter „Favoriten“
  und „Alle“ mit Bild (sonst Initialen), Name und Nummer; blättern per Wischen
  oder mit den Pfeilen. Kontakt antippen → alle Nummern als große Knöpfe
  „Anrufen · Mobil/Privat/Arbeit“; „Zurück“ führt erst zur Liste. Gewählt wird
  über den voicecall-Dienst (erste SIM); unten erscheint „Wählt: Name“ mit
  „Auflegen“. Favoriten = in „Personen“ am Telefon als Favorit markiert;
  ab 0.9.2 als große Kacheln (3×2, blättern mit Pfeilen), eine Nummer = Antippen
  ruft sofort an. Apps aus der Kachel „Apps“ werden nur einmal gestartet.
  Im Log nur Anzahlen, keine Namen/Nummern.
- **Zuletzt** (ab 0.10.0): dritter Reiter im Telefonbuch mit der Anrufliste
  (rot = verpasst, Pfeil = ein-/ausgehend, Name aus dem Adressbuch, Zeit).
  Antippen ruft zurück. Gelesen vom selben Helfer aus der Sailfish-Anrufliste.
- **Musik-Listen** (ab 0.10.0): Knopf „Liste“ auf der Musikseite zeigt
  Playlists und Titelliste des Players (MPRIS Playlists/TrackList), Antippen
  spielt. Bietet ein Player das nicht an, sagt das Auto das; im Log steht, was
  der Player kann.
- **Wo steht mein Auto?** Die App fragt die GPS-Position des Autos ab (Sensor
  LOCATION) und merkt sich die letzte beim Trennen. Am Telefon: Abschnitt
  „Auto geparkt“ mit Uhrzeit, „Auf Karte zeigen“ (geo:-Link → Pure Maps /
  OSM Scout) und „Kopieren“. Koordinaten stehen nicht im Log.

## Eigener Player: Radio & Musik

Ab 0.13.0. Die Musik-Apps geben keine Listen heraus, deshalb hat SailAuto einen
eigenen Player (GStreamer, erscheint als MPRIS-Player „SailAuto Radio & Musik“,
also mit Lenkradtasten, Fortschritt usw.). Im Auto: Musik → „Liste“ (oder
„Radio & Musik“ ohne Player):

- Radio: Favoriten, Beliebt (meistgehörte Sender des Landes), Radio suchen
  (Tastatur, nur im Stand). Quelle: Radio-Browser-Verzeichnis (online).
  Beim Hören: Liste → „★ Sender merken“. ⏮/⏭ wechseln zwischen Favoriten.
- Musik: Ordner im Musik-Verzeichnis des Telefons und auf der Speicherkarte,
  Ordner → Titel, Antippen spielt ab dort der Reihe nach.
- „Listen von <App>“, falls der aktuelle Player welche anbietet.
- Ab 0.14.0: **SailWave: Sender** (aus SailWaves Datenbank) und **Sailcast:
  Podcasts** (Abos aus Sailcasts Datenbank, Folgen aus den Feeds) – gespielt vom
  eigenen Player. Ab 0.18.0 sucht SailAuto die Sailcast-Abos in allen Dateien
  der App (SQLite, JSON, OPML); im Log steht, woher sie kamen.
- Ab 0.18.0: **TuneBridge**: Listen wie auf TuneBridges Startseite (auf dem
  Telefon, Favoriten, zuletzt gehört, Playlists, Alben) und Suche in allen Songs,
  die TuneBridge kennt. Gelesen aus `~/.local/share/harbour-tunebridge/` (nur
  lesen). Gespielt vom eigenen Player: gespeicherte Songs aus der Datei, andere
  über TuneBridges yt-dlp (Internet). Playlists erscheinen, sobald sie einmal in
  TuneBridge geöffnet wurden. Spielt TuneBridge gerade, stehen seine Listen
  oben.
- Ab 0.23.0: Hat TuneBridge die Auto-Erweiterung (`docs/tunebridge-carlink.md`,
  MPRIS `OpenUri` mit `tunebridge:`), spielt TuneBridge Listen und Suchtreffer
  selbst (auch Spotify-Songs), und die Suche geht online. Sonst wie oben.
- Ab 0.18.0: App wechseln / beenden als kleine Icons oben in der Liste.

Adresssuche ohne OSM Scout Server: online über Photon (wie Pure Maps).

## Helles Aussehen (ab 0.20.0)

Einstellung „Anzeige im Auto → Aussehen“: Automatisch (hell am Tag, dunkel bei
Nacht nach dem Lichtsensor des Autos, Standard), Immer hell, Immer dunkel.
Die Karte hat eigene Tag- und Nachtfarben.

## GPU (ab 0.24.0)

Bildschirm und Karte werden über OpenGL gezeichnet (Framebuffer mit
Kantenglättung, Karte direkt auf der GPU, die Kartentextur bleibt dort). Die
ersten Bilder werden geprüft; stimmt etwas nicht oder fehlt OpenGL, zeichnet
SailAuto von selbst mit der CPU (Protokoll: „Grafik: …“). Ab 0.26.1 wird die
geneigte Karte im GPU-Modus als Textur mit Mipmaps und 16x anisotroper
Filterung gezeichnet (eigener Shader, perspektivisch korrekt; GLES 3 wenn
vorhanden): die Ferne flimmert nicht mehr. Protokoll: „Karte: GPU-Filter …“. Schalter in den
Einstellungen.

Video-Kodierer (ab 0.25.0): zuerst der Hardware-Kodierer des Telefons direkt
über libdroidmedia (per libhybris geladen, YUV-Bilder statt Kamera-Puffer; der
Aufbau der Android-Strukturen richtet sich nach der installierten
droidmedia-Version). Kommt nach 4 s kein Bild oder meldet er einen Fehler, geht
es mit Software weiter. Stürzt die App dabei ab, bleibt eine Markierung
(`~/.cache/harbour-sailauto/harbour-sailauto/hwenc-trying`) und beim nächsten
Start wird gleich Software genommen. Software: openh264 v2.7.0 (NEON,
Bildschirm-Modus, Keyframe alle ~10 s, QP 18–40, 3 Mbit/s bei 800×480).
Schalter „Hardware-Videokodierer“.

## Stromsparen (ab 0.18.1)

Ein Bild, das sich nicht ändert, wird nicht neu kodiert: ruhige Seiten laufen
mit 1 Bild/s statt 30, nach 1 s ohne Änderung wird nur 5-mal pro Sekunde
nachgesehen; Tippen bringt sofort volle Rate. Die Karte beim Fahren läuft mit
so vielen Bildern, wie das Auto nimmt (meist 30/s). `SAILAUTO_FULLFPS=1` schaltet das ab.

## Wetter, Ladebildschirm

Ab 0.12.0. **Wetter** oben im Auto (Symbol + Temperatur), antippen: jetzt,
nächste 6 Stunden, 3 Tage. Quelle Open-Meteo (kein Schlüssel); dafür geht die
auf ~10 km gerundete Position ins Internet. Schalter „Wetter im Auto“.
**Ladebildschirm** beim Start einer App aus dem Auto (bis ihr Player da ist,
max. 15 s; Antippen schließt). **Musik ohne Player:** freundlicher Hinweis und
Kacheln zum Starten der Musik-Apps.

## Autostart im Auto

Ab 0.5.0, Schalter „Autostart im Auto“ (Standard: aus). Schaltet den
Root-Dienst `harbour-sailauto-autostart` ein. Der startet SailAuto (USB-Modus
gleich an), wenn

- das Auto per USB Android Auto anfragt (Kernel-Meldung `ACCESSORY=START`, auch
  im normalen USB-Modus – ob der Sailfish-Kernel das im MTP-/Entwicklermodus
  meldet, zeigt der erste Test), oder
- sich das Telefon per Bluetooth mit einem bekannten Auto verbindet. Bekannt =
  einmal per USB verbunden; die App merkt sich die Bluetooth-Adresse des Autos
  (`~/.local/share/org.roterfux/harbour-sailauto/cars`).

Ist das Auto 60 s weg (USB nicht verbunden und kein Bluetooth), stellt der
Dienst USB wieder auf normal. Log: `/run/harbour-sailauto/autostart.log`,
Status: `devel-su sh /usr/share/harbour-sailauto/aoa-gadget.sh status`.

## Telefon-Ton im Auto

Ab 0.3.0, Schalter „Telefon-Ton ins Auto“ (Standard: an). Sobald das Auto-Bild
läuft, legt die App in PulseAudio einen virtuellen Ausgang „SailAuto-Auto“ an,
macht ihn zum Standard und leitet alle laufenden Wiedergaben dorthin um
(Musik-Apps, Pure Maps, …). Ab 0.4.1 schreibt PulseAudio den Ton in eine Pipe
(module-pipe-sink, keine Aufnahme → kein Mikrofon, keine Rückkopplung); Lücken
(Pause, Titelwechsel) füllt die App mit Stille. Der Ton geht (48 kHz Stereo)
über den Medienkanal ans Auto: sobald etwas spielt Audio-Fokus +
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
Spulen (ab 0.4.2): ⏭/⏮ am Lenkrad halten = 10 s-Schritte alle 0,5 s,
REWIND/FAST_FORWARD (89/90) genauso, Tippen auf den Fortschrittsbalken springt
an die Stelle (MPRIS Seek/SetPosition).

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

Unterbricht das Auto selbst (Verkehrsfunk, eigene Ansage, Radio-Taste), pausiert
SailAuto den Player; bei einer kurzen Unterbrechung behält SailAuto den
Audio-Fokus, und der Player läuft danach von allein weiter. Sagt das Auto nie
„zurück“, geht der Ton wieder ans Auto, sobald die Musik wieder spielt (oder nach
2 min); beim Neuverbinden startet der Ton immer frisch. Ändert sich die Lautstärke am Telefon, zeigt der Autobildschirm sie
2 s lang unten als Balken.

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

## Fremdcode / Lizenzen

- openh264 v2.7.0 (BSD-2-Clause), `third_party/openh264`
- Kachel-Symbole im Auto: Material Design Icons (Apache License 2.0)
- Karte: © OpenStreetMap-Mitwirkende (ODbL), © OpenMapTiles (Vektorschema); Vektorkacheln von OSM Scout Server oder OpenFreeMap, Bildkacheln von openstreetmap.org, Ortsnamen dort über Overpass; Routen: Valhalla (OSM Scout Server / FOSSGIS); Verkehrsmeldungen: TomTom (eigener Schlüssel)
