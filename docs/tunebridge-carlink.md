# TuneBridge ↔ SailAuto: Steuerung aus dem Auto

SailAuto (Android-Auto-Brücke) zeigt im Auto TuneBridges Listen und eine Suche.
Ohne diese Erweiterung spielt SailAuto die Songs selbst ab (nur gespeicherte oder
mit YouTube-Treffer, Suche nur offline). Mit ihr spielt **TuneBridge selbst**
(eigene Warteschlange, Spotify-Songs, Online-Suche).

Weg: MPRIS `OpenUri` mit dem Schema `tunebridge:`. Kein neuer D-Bus-Dienst nötig,
Amber.Mpris kann das schon.

## TuneBridge: was einbauen

1. Am `MprisPlayer` in `qml/harbour-tunebridge.qml`:

   ```qml
   supportedUriSchemes: ["tunebridge"]
   onOpenUriRequested: appWindow.carOpen(url.toString())
   ```

   SailAuto erkennt die Erweiterung an `SupportedUriSchemes` (enthält
   `tunebridge`) und nutzt sie nur dann.

2. `carOpen(uri)` (Werte sind URL-kodiert, `decodeURIComponent`):

   | URI | Was TuneBridge tun soll |
   |---|---|
   | `tunebridge:list?kind=K&id=I&start=N` | Liste laden wie beim Antippen auf der Startseite (`kind` wie in `library()`: `saved`, `favs`, `recent`, `liked`, `playlist`, `album`; `id` leer bei saved/favs/recent), dann ab Song `N` (0-basiert) abspielen. Ohne Spotify: die am Telefon gespeicherte Kopie (`list-<kind>-<id>.json`). |
   | `tunebridge:search?q=TEXT` | Online suchen (wie die Suchseite, `backend.search`), Ergebnis merken und **nach** `car-search.json` im Datenordner schreiben (siehe unten). Nicht abspielen. |
   | `tunebridge:play?source=search&start=N` | Die gemerkte Suche ab Song `N` als Warteschlange abspielen. |

   Unbekanntes einfach ignorieren.

3. `car-search.json` (in `~/.local/share/harbour-tunebridge/`, atomar wie
   `_write`):

   ```json
   {"query": "TEXT", "tracks": [ {"id": "...", "title": "...", "artists": "...",
     "album": "...", "cover": "https://...", "ms": 215000}, ... ]}
   ```

   `query` genau wie empfangen (dekodiert). Gleiches Track-Format wie
   `favourites.json`. SailAuto wartet bis 20 s auf die Datei (neuer als die
   Anfrage, passende `query`).

## Testen am Telefon

```bash
dbus-send --session --print-reply --dest=org.mpris.MediaPlayer2.tunebridge /org/mpris/MediaPlayer2 org.freedesktop.DBus.Properties.Get string:org.mpris.MediaPlayer2 string:SupportedUriSchemes
```

```bash
dbus-send --session --print-reply --dest=org.mpris.MediaPlayer2.tunebridge /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.OpenUri string:"tunebridge:search?q=queen"
```

```bash
cat ~/.local/share/harbour-tunebridge/car-search.json
```

```bash
dbus-send --session --print-reply --dest=org.mpris.MediaPlayer2.tunebridge /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player.OpenUri string:"tunebridge:play?source=search&start=0"
```

## SailAuto-Seite (fertig ab 0.23.0)

- Liste im Auto antippen → `tunebridge:list?...&start=N`, „Abspielen“ → `start=0`.
- „TuneBridge: Suchen“ → Tastatur (nur im Stand) → `tunebridge:search?q=` →
  Ergebnisse aus `car-search.json` → Antippen → `tunebridge:play?source=search&start=N`.
- Läuft TuneBridge nicht oder fehlt die Erweiterung: wie bisher (eigener Player,
  Offline-Suche).
