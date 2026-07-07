/*
 * Raw Pixel WebSocket Usermod
 * ---------------------------
 * Öffnet einen zweiten, schlanken WebSocket-Endpunkt (/rawws), der bei jedem
 * tatsächlich neuen LED-Frame die kompletten, unveränderten RGBW-Rohwerte
 * ALLER Pixel (über alle Segmente hinweg) als kompaktes Binärformat sendet.
 *
 * Warum ein eigener Endpunkt statt /ws bzw. /json/live ("Peek")?
 *  - /json/live kodiert über ArduinoJson -> spürbarer Overhead, dazu
 *    Downsampling ab 256 LEDs (nur jedes n-te Pixel).
 *  - /json/live mischt den weißen Kanal additiv in RGB rein
 *    (r = scale8(qadd8(w, r), bri)) - das ist für die reine Browser-Vorschau
 *    gedacht, nicht für rohe RGBW-Auswertung. Genau daher kommen die
 *    "255,255,255,255"-Werte bei Segmenten > 0, sobald ein Effekt mit
 *    weißem Kanal läuft.
 *  - Beide Wege laufen zusätzlich über die Anfrage-Queue des Webservers.
 *
 * Dieses Usermod umgeht das komplett: es liest strip.getPixelColor(i) direkt
 * (das ist der tatsächliche, fertig kombinierte Framebuffer-Wert jedes
 * Pixels, exakt so wie er auch an die LEDs ausgegeben wird - inkl. korrektem
 * W-Kanal, ohne Mischung) und schickt das 1:1 als Binärpaket raus, sobald
 * WLED tatsächlich einen neuen Frame gerendert hat. Kein festes 20/40ms
 * Intervall - die Sendefrequenz folgt exakt der echten FPS von WLED
 * (typisch 60-200+ FPS je nach Effekt/Board), und es wird nichts gesendet,
 * wenn sich nichts geändert hat oder kein Client verbunden ist.
 *
 * Binärformat (Little Endian):
 *   Byte 0    : Magic 0xA5
 *   Byte 1    : Protokollversion (aktuell 1)
 *   Byte 2-3  : uint16 Anzahl LEDs in diesem Paket
 *   danach je LED 4 Bytes: R, G, B, W (0-255, unskaliert, roh)
 *
 * Installation (neues Usermod-Framework, empfohlen):
 *   1. Diesen Ordner (raw_pixel_ws_usermod) neben deinen WLED-Checkout legen,
 *      oder als eigenes Repo pushen.
 *   2. library.json (liegt bei) sorgt für den PlatformIO-Verweis.
 *   3. In platformio_override.ini:
 *        [env:esp32dev]
 *        extends = env:esp32dev
 *        custom_usermods = ${env:esp32dev.custom_usermods} symlink:///pfad/zu/raw_pixel_ws_usermod
 *   4. Ganz normal kompilieren/flashen (PlatformIO Build).
 *
 * Installation (älteres WLED, usermods_list.cpp):
 *   In wled00/usermods_list.cpp:
 *     #include "../usermods/raw_pixel_ws_usermod/usermod_raw_pixel_ws.cpp" // Pfad anpassen
 *     ...
 *     usermods.add(new RawPixelWsUsermod());
 *   und die REGISTER_USERMOD-Zeile am Dateiende auskommentieren/entfernen,
 *   falls dein Baum das alte Makro nicht kennt.
 */

#include "wled.h"

#ifndef RAWWS_MAX_QUEUE_PER_CLIENT
#define RAWWS_MAX_QUEUE_PER_CLIENT 2   // Backpressure: langsame Clients werden übersprungen statt alles zu blockieren
#endif

class RawPixelWsUsermod : public Usermod {
  private:
    AsyncWebSocket wsRaw = AsyncWebSocket("/rawws");
    uint32_t lastFrameSent = 0;   // letzter strip.now Wert, der bereits verschickt wurde
    bool enabled = true;          // per Usermod-Settings umschaltbar
    bool onlyMainSegment = false; // optional: nur Hauptsegment statt gesamten Framebuffer senden

    static const char _name[];
    static const char _enabled[];
    static const char _onlyMain[];

    // Baut das Binärpaket und schickt es an alle verbundenen, nicht überlasteten Clients
    void sendFrame() {
      uint16_t total;
      if (onlyMainSegment) {
        Segment &seg = strip.getMainSegment();
        total = seg.length();
      } else {
        total = strip.getLengthTotal();
      }
      if (total == 0) return;

      size_t bufSize = 4 + (size_t)total * 4;
      // Größere Buffer (viele hundert/tausend LEDs) lieber auf dem Heap statt Stack
      uint8_t *buf = (uint8_t*) malloc(bufSize);
      if (!buf) return; // OOM -> diesen Frame überspringen, nicht crashen

      buf[0] = 0xA5;
      buf[1] = 0x01;
      buf[2] = (uint8_t)(total & 0xFF);
      buf[3] = (uint8_t)((total >> 8) & 0xFF);

      size_t idx = 4;
      if (onlyMainSegment) {
        Segment &seg = strip.getMainSegment();
        for (uint16_t i = 0; i < total; i++) {
          uint32_t c = seg.getPixelColor(i); // rohe, unvermischte Segmentfarbe inkl. W
          buf[idx++] = R(c);
          buf[idx++] = G(c);
          buf[idx++] = B(c);
          buf[idx++] = W(c);
        }
      } else {
        for (uint16_t i = 0; i < total; i++) {
          uint32_t c = strip.getPixelColor(i); // fertiger Framebuffer-Wert, exakt wie ausgegeben
          buf[idx++] = R(c);
          buf[idx++] = G(c);
          buf[idx++] = B(c);
          buf[idx++] = W(c);
        }
      }

      // an alle Clients senden, die nicht "verstopft" sind (kein globales Blockieren durch einen langsamen Client)
      for (auto client : wsRaw.getClients()) {
        if (client->status() == WS_CONNECTED && client->queueLength() < RAWWS_MAX_QUEUE_PER_CLIENT) {
          client->binary(buf, bufSize);
        }
      }

      free(buf);
      wsRaw.cleanupClients();
    }

  public:
    void setup() override {
      wsRaw.onEvent([](AsyncWebSocket *server, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
        // Wir benötigen keine eingehenden Kommandos - reines Ausgabe-Websocket.
        // Bei Bedarf könnte man hier z.B. {"lv":true}-ähnliche Steuerbefehle parsen.
        if (type == WS_EVT_CONNECT) {
          DEBUG_PRINTF_P(PSTR("RawPixelWS: client #%u connected\n"), client->id());
        }
      });
      server.addHandler(&wsRaw);
    }

    void connected() override {
      // nichts weiter nötig - Websocket läuft unabhängig vom WiFi-Connect-Status,
      // AsyncWebSocket kümmert sich selbst um Clients.
    }

    void loop() override {
      if (!enabled) return;
      if (wsRaw.count() == 0) return;       // niemand hört zu -> gar nichts tun/rendern
      if (strip.now == lastFrameSent) return; // noch kein neuer Frame seit letztem Versand
      lastFrameSent = strip.now;
      sendFrame();
    }

    void addToConfig(JsonObject &root) override {
      JsonObject top = root.createNestedObject(FPSTR(_name));
      top[FPSTR(_enabled)]  = enabled;
      top[FPSTR(_onlyMain)] = onlyMainSegment;
    }

    bool readFromConfig(JsonObject &root) override {
      JsonObject top = root[FPSTR(_name)];
      bool configComplete = !top.isNull();
      configComplete &= getJsonValue(top[FPSTR(_enabled)],  enabled, true);
      configComplete &= getJsonValue(top[FPSTR(_onlyMain)], onlyMainSegment, false);
      return configComplete;
    }

    uint16_t getId() override {
      return USERMOD_ID_UNSPECIFIED; // ggf. eigene ID beantragen, falls PR an WLED geplant
    }
};

const char RawPixelWsUsermod::_name[]     PROGMEM = "RawPixelWS";
const char RawPixelWsUsermod::_enabled[]  PROGMEM = "enabled";
const char RawPixelWsUsermod::_onlyMain[] PROGMEM = "onlyMainSegment";

static RawPixelWsUsermod raw_pixel_ws_usermod;
REGISTER_USERMOD(raw_pixel_ws_usermod);
