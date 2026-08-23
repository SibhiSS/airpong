#include "net.h"
#include "config.h"
#include "swing.h"
#include "game_page.h"
#include "scope_page.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint16_t sendIntervalMs = SEND_INTERVAL_MS_DEFAULT;
static uint16_t seqCounter = 0;

static void putU16(uint8_t *buf, int off, uint16_t v) { buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF; }
static void putU32(uint8_t *buf, int off, uint32_t v) {
  buf[off] = v & 0xFF; buf[off + 1] = (v >> 8) & 0xFF;
  buf[off + 2] = (v >> 16) & 0xFF; buf[off + 3] = (v >> 24) & 0xFF;
}
static void putI16(uint8_t *buf, int off, int32_t v) {
  if (v > 32767) v = 32767;
  if (v < -32768) v = -32768;
  putU16(buf, off, (uint16_t)(int16_t)v);
}

static void setRateHz(uint8_t hz) {
  if (hz < 10) hz = 10;
  if (hz > 200) hz = 200;
  uint16_t ms = 1000 / hz;
  if (ms < SEND_INTERVAL_MS_MIN) ms = SEND_INTERVAL_MS_MIN;
  if (ms > SEND_INTERVAL_MS_MAX) ms = SEND_INTERVAL_MS_MAX;
  sendIntervalMs = ms;
  Serial.printf("-- send rate set to ~%u Hz (%u ms) --\n", 1000 / ms, ms);
}

static void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                       AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("ws client #%u connected from %s\n", client->id(),
                    client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("ws client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      // Only handle complete, unfragmented frames — this protocol's messages
      // (1-9 bytes) never need fragmentation, so anything else is unexpected.
      if (!(info->final && info->index == 0 && info->len == len && len >= 1)) break;
      uint8_t op = data[0];
      if (op == OP_REZERO) {
        imuZero();
      } else if (op == OP_PING && len >= 5) {
        // Echo the client's own timestamp back alongside ours, so the client
        // can compute RTT with a single subtraction and doesn't need clock
        // sync with the ESP32 to do it.
        uint32_t clientTs;
        memcpy(&clientTs, data + 1, 4);
        uint8_t reply[9];
        reply[0] = OP_PING;
        memcpy(reply + 1, &clientTs, 4);
        uint32_t nowMs = millis();
        memcpy(reply + 5, &nowMs, 4);
        client->binary(reply, sizeof(reply));
      } else if (op == OP_SETRATE && len >= 2) {
        setRateHz(data[1]);
      }
      break;
    }
    default: break;
  }
}

void netInit() {
  WiFi.mode(WIFI_AP_STA);

  char apSsid[32];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  WiFi.softAP(apSsid, AP_PASSWORD);
  Serial.printf("SoftAP up: SSID=\"%s\"  password=\"%s\"  IP=%s\n",
                apSsid, AP_PASSWORD, WiFi.softAPIP().toString().c_str());

  if (strlen(STA_SSID) > 0) {
    Serial.printf("Joining \"%s\"", STA_SSID);
    WiFi.begin(STA_SSID, STA_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
      delay(250);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nSTA connected, IP=%s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("\nSTA connect timed out — continuing on SoftAP only.");
    }
  }

  // Default ESP32 modem sleep saves battery by napping between beacons, which
  // injects exactly the kind of 100ms+ latency spike a timing-sensitive game
  // cannot tolerate. Trade a bit of battery life for consistent latency.
  WiFi.setSleep(false);

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  // Serving these pages itself, rather than pointing a phone at a file on
  // the dev PC, is what actually makes this work off the PC entirely: join
  // the AP, browse to this IP, done. No file path, no second network.
  // no-cache: these pages change every reflash during active development,
  // and a phone browser caching a stale one — then a fix "not working" when
  // it actually already shipped — is exactly the confusion that cost real
  // time earlier. Correctness here matters more than the bandwidth saved.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse_P(200, "text/html", GAME_HTML);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  server.on("/scope", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse_P(200, "text/html", SCOPE_HTML);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });
  server.begin();
  Serial.println("HTTP+WS server started on port 80");
}

void netSendTelemetry(const ImuSample &s) {
  uint8_t buf[20];
  uint8_t flags = 0;
  if (imuIsCalibrated()) flags |= FLAG_CALIBRATED;
  if (s.fault) flags |= FLAG_IMU_FAULT;
  if (swingIsActive()) flags |= FLAG_SWING_ACTIVE;

  buf[0] = FRAME_MAGIC;
  buf[1] = flags;
  putU16(buf, 2, seqCounter++);
  putU32(buf, 4, millis());
  putI16(buf, 8, (int32_t)lroundf(s.roll_deg * 100.0f));
  putI16(buf, 10, (int32_t)lroundf(s.pitch_deg * 100.0f));
  putI16(buf, 12, (int32_t)lroundf(s.wx_dps * 10.0f));
  putI16(buf, 14, (int32_t)lroundf(s.wy_dps * 10.0f));
  putI16(buf, 16, (int32_t)lroundf(s.wz_dps * 10.0f));
  putI16(buf, 18, (int32_t)lroundf(swingPeakDps() * 10.0f));

  ws.binaryAll(buf, sizeof(buf));
  ws.cleanupClients();
}

uint16_t netGetSendIntervalMs() { return sendIntervalMs; }
