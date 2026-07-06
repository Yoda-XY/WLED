# RawPixelWS – rohes, ungebremstes Pixel-WebSocket für WLED

Öffnet `/rawws` auf deinem WLED-Gerät. Sendet bei **jedem tatsächlich neuen
Frame** (keine feste Zeitscheibe wie 20/40ms) die vollen RGBW-Rohwerte aller
Pixel als kompaktes Binärpaket – ohne JSON-Overhead, ohne Downsampling, ohne
das Vermischen von W in RGB, das beim eingebauten "Peek"/`/json/live`
passiert.

## Warum nicht einfach `/ws` bzw. `/json/live` patchen?

Kurz zusammengefasst (Details auch im Kommentarkopf von
`usermod_raw_pixel_ws.cpp`):

- `/json/live` läuft über ArduinoJson-Serialisierung → CPU-Overhead, dazu
  Downsampling ab 256 LEDs (nur jedes n-te Pixel wird geliefert).
- Die Darstellung dort addiert den weißen Kanal in RGB
  (`r = scale8(qadd8(w, r), bri)`), gedacht für die Vorschau im Browser, nicht
  als Rohwert. Das erzeugt genau das Verhalten, das du beschrieben hast:
  ausgebrannte 255/255/255/255-Werte auf Segmenten außer dem ersten, sobald
  ein Effekt mit weißem Kanal läuft.
- Beide Wege hängen zusätzlich an der Anfrage-/Queue-Logik des Webservers,
  was die 0–800ms Schwankungen erklärt, die du siehst.

Ein eigener, schlanker Endpunkt (dieses Usermod) umgeht das komplett, ohne
den WLED-Core selbst patchen zu müssen (übersteht also Firmware-Updates).

## Installation (aktuelles Usermod-Framework)

1. Diesen Ordner neben deinen WLED-Checkout legen (oder als eigenes Git-Repo
   pushen).
2. In `platformio_override.ini` deiner WLED-Umgebung:

   ```ini
   [env:esp32dev]
   extends = env:esp32dev
   custom_usermods = ${env:esp32dev.custom_usermods} symlink:///pfad/zu/raw_pixel_ws_usermod
   ```

3. Normal mit PlatformIO bauen/flashen.

## Installation (älterer Baum mit `usermods_list.cpp`)

In `wled00/usermods_list.cpp`:

```cpp
#include "../usermods/raw_pixel_ws_usermod/usermod_raw_pixel_ws.cpp"
// ...
usermods.add(new RawPixelWsUsermod());
```

Die `REGISTER_USERMOD(...)`-Zeile am Ende der `.cpp` ggf. entfernen/auskommentieren,
falls dieses Makro in deinem Baum noch nicht existiert.

## Nutzung

`ws://WLED-IP/rawws` verbinden, Binärframes empfangen. Format:

```
Byte 0    : 0xA5 (Magic)
Byte 1    : Protokollversion (1)
Byte 2-3  : uint16 LE, Anzahl LEDs in diesem Paket
danach    : je LED 4 Bytes R,G,B,W (0-255, roh, unskaliert)
```

Siehe `client_example.js` für einen minimalen Parser.

## Einstellungen (Usermod-Settings-Seite in der WLED-UI)

- **enabled**: Endpunkt an/aus.
- **onlyMainSegment**: nur das Hauptsegment senden statt des gesamten
  Framebuffers (kleinere Pakete, falls du nur ein Segment brauchst).

## Wenn du stattdessen WLED als *Empfänger* mit niedrigster Latenz ansteuern willst

Falls es dir eigentlich darum geht, Farbwerte **in** WLED mit möglichst wenig
Latenz reinzuschicken (statt Pixelwerte auszulesen), ist der bessere Weg,
eines der eingebauten **UDP-Realtime-Protokolle** zu nutzen statt WebSocket:

- **DDP** oder **DRGBW/DNRGBW** über den UDP-Realtime-Port (Standard 21324)
- Kein TCP-Handshake/Queueing, kein JSON, nativ mit korrektem RGBW-Kanal
- Wird von WLED für genau diesen Zweck empfohlen (Ambilight, LedFx, xLights, etc.)

Das ist in der Regel die niedrigste erreichbare Latenz, die WLED überhaupt
anbietet – niedriger als jedes Websocket-Verfahren.
