#include "net.h"
#include "config.h"
#include "swing.h"
#include "game_page.h"
#include "scope_page.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
// baseIntervalMs is what was asked for (config default, or OP_SETRATE).
// sendIntervalMs is what is actually being used right now, which backs off on
// its own when the link cannot keep up and recovers when it can. Keeping the
// two separate is what lets it come back to full rate when you walk back into
// range instead of staying slow until the next reboot.
static uint16_t baseIntervalMs = SEND_INTERVAL_MS_DEFAULT;
static uint16_t sendIntervalMs = SEND_INTERVAL_MS_DEFAULT;
static uint16_t seqCounter = 0;

// Link health, all reported over OP_STAT.
static uint16_t droppedFrames = 0;     // frames not sent because the queue was full
static uint8_t  consecutiveSkips = 0;
static uint32_t lastCleanSendMs = 0;   // last time a frame went out without skipping
static uint16_t sentSinceStat = 0;
static uint8_t  lastTxHz = 0;
static uint32_t lastStatMs = 0;

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
  baseIntervalMs = ms;
  sendIntervalMs = ms;          // start from the requested rate; backoff re-derives from here
  consecutiveSkips = 0;
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
  // AP-only unless STA credentials are actually configured. AP_STA keeps a
  // second interface — and a second set of radio duties — alive for nothing
  // when STA_SSID is empty, and the radio is the scarce resource outdoors.
  WiFi.mode(strlen(STA_SSID) > 0 ? WIFI_AP_STA : WIFI_AP);

  char apSsid[32];
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apSsid, sizeof(apSsid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
  // Explicit channel and a bounded client count, rather than the library
  // defaults: see config.h AP_CHANNEL / WS_MAX_CLIENTS.
  WiFi.softAP(apSsid, AP_PASSWORD, AP_CHANNEL, 0 /* not hidden */, WS_MAX_CLIENTS + 2);
  Serial.printf("SoftAP up: SSID=\"%s\"  password=\"%s\"  IP=%s  ch=%d\n",
                apSsid, AP_PASSWORD, WiFi.softAPIP().toString().c_str(), AP_CHANNEL);

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
  // setSleep(false) covers the Arduino layer; the esp_wifi call underneath it
  // is the one that actually pins the power-save mode, and is set explicitly
  // so a core update changing the wrapper's behaviour cannot quietly
  // reintroduce the spikes.
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  // Full transmit power. Indoors this is irrelevant — every wall bounces a
  // second copy of the signal around your body, so even a weak transmitter
  // gets through. Outdoors there are no reflections and the only path left
  // runs straight through the player, so every dB of margin is a dB fewer
  // retransmissions, and retransmissions are what the send queue backs up
  // behind.
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("WiFi: power-save off, tx power %d (0.25 dBm units)\n", (int)WiFi.getTxPower());

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

// RSSI the AP sees from the associated station, i.e. how well the *phone's*
// transmissions are arriving. Reported rather than inferred: on a link this
// asymmetric (a phone's antenna and power budget are nothing like a bare
// ESP32's) guessing from one side is how "it's laggy outside" stays a mystery.
static int8_t associatedRssi() {
  wifi_sta_list_t stations;
  if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num == 0) return 0;
  int8_t best = -127;
  for (int i = 0; i < stations.num; i++) {
    if (stations.sta[i].rssi > best) best = stations.sta[i].rssi;
  }
  return best;
}

// Once per STAT_INTERVAL_MS: how the link is actually doing, so the game can
// show it and the failure can be identified on the spot.
static void sendStat(const ImuSample &s, uint32_t nowMs) {
  uint32_t elapsed = nowMs - lastStatMs;
  if (elapsed > 0) {
    uint32_t hz = (sentSinceStat * 1000UL) / elapsed;
    lastTxHz = (uint8_t)(hz > 255 ? 255 : hz);
  }
  sentSinceStat = 0;
  lastStatMs = nowMs;

  uint8_t buf[14];
  buf[0] = OP_STAT;
  buf[1] = (uint8_t)ws.count();
  buf[2] = (uint8_t)associatedRssi();
  buf[3] = lastTxHz;
  putU16(buf, 4, droppedFrames);
  putI16(buf, 6, (int32_t)lroundf(s.temp_c * 10.0f));
  putU32(buf, 8, ESP.getFreeHeap());
  putU16(buf, 12, sendIntervalMs);

  // Diagnostics are the first thing to give up airtime when there is none to
  // spare — they must never be the reason a telemetry frame gets queued behind
  // something else.
  if (ws.availableForWriteAll()) ws.binaryAll(buf, sizeof(buf));

  // Reaping stale clients belongs on this slow cadence, not on every telemetry
  // frame: a phone that walks out of range never sends a close, so its socket
  // lingers until TCP gives up minutes later — and until it does, every
  // broadcast is also being retransmitted at a peer that is not there.
  ws.cleanupClients(WS_MAX_CLIENTS);
}

void netSendTelemetry(const ImuSample &s) {
  uint32_t nowMs = millis();
  if (lastStatMs == 0) lastStatMs = nowMs;

  // The whole point (config.h, "link backpressure"): an orientation is only
  // worth sending if it can go out now. If the previous one has not drained,
  // this one would sit behind it and arrive stale, and the one after that
  // staler still. Drop it instead — the next sample is 10 ms away and strictly
  // better. Latency stays bounded; the update rate is what degrades.
  if (!ws.availableForWriteAll()) {
    droppedFrames++;
    if (consecutiveSkips < 255) consecutiveSkips++;
    // Persistent skipping means we are generating frames faster than this link
    // can carry them, so stop generating them that fast. Fewer, bigger-spaced
    // frames also mean fewer packets contending for airtime, which is what
    // lets a marginal link recover instead of collapsing.
    if (consecutiveSkips >= BACKPRESSURE_SKIPS_BEFORE_BACKOFF &&
        sendIntervalMs < SEND_INTERVAL_CEILING_MS) {
      uint16_t next = sendIntervalMs + SEND_INTERVAL_BACKOFF_MS;
      sendIntervalMs = (next > SEND_INTERVAL_CEILING_MS) ? SEND_INTERVAL_CEILING_MS : next;
      consecutiveSkips = 0;
      // Recovery is measured from here — "clean for 2 s *since backing off*",
      // not since whenever the rate last happened to be at base. Otherwise the
      // first frame through after a long congested stretch already satisfies
      // the recovery test and the rate oscillates instead of settling.
      lastCleanSendMs = nowMs;
      Serial.printf("!! link congested — backing off to ~%u Hz\n", 1000 / sendIntervalMs);
    }
    if (nowMs - lastStatMs >= STAT_INTERVAL_MS) sendStat(s, nowMs);
    return;
  }

  uint8_t buf[20];
  uint8_t flags = 0;
  if (imuIsCalibrated()) flags |= FLAG_CALIBRATED;
  if (s.fault) flags |= FLAG_IMU_FAULT;
  if (swingIsActive()) flags |= FLAG_SWING_ACTIVE;

  buf[0] = FRAME_MAGIC;
  buf[1] = flags;
  putU16(buf, 2, seqCounter++);
  putU32(buf, 4, nowMs);
  putI16(buf, 8, (int32_t)lroundf(s.roll_deg * 100.0f));
  putI16(buf, 10, (int32_t)lroundf(s.pitch_deg * 100.0f));
  putI16(buf, 12, (int32_t)lroundf(s.wx_dps * 10.0f));
  putI16(buf, 14, (int32_t)lroundf(s.wy_dps * 10.0f));
  putI16(buf, 16, (int32_t)lroundf(s.wz_dps * 10.0f));
  putI16(buf, 18, (int32_t)lroundf(swingPeakDps() * 10.0f));

  ws.binaryAll(buf, sizeof(buf));
  sentSinceStat++;
  // Decay rather than reset: a link losing eight frames in every nine is
  // just as congested as one losing eight in a row, but never accumulates
  // eight *consecutive* skips, so a hard reset here would leave it hammering
  // a link it should have backed off from. A healthy link decays this to zero
  // within a few frames regardless.
  if (consecutiveSkips > 0) consecutiveSkips--;

  // Recover only after a sustained clean stretch, not on the first frame that
  // happens to get through: walking back toward the tablet should restore full
  // rate, but one lucky gap in the interference should not, or the rate
  // oscillates and the feel oscillates with it.
  if (sendIntervalMs > baseIntervalMs && nowMs - lastCleanSendMs >= SEND_RATE_RECOVER_MS) {
    sendIntervalMs = (sendIntervalMs > baseIntervalMs + SEND_INTERVAL_BACKOFF_MS)
                     ? (uint16_t)(sendIntervalMs - SEND_INTERVAL_BACKOFF_MS)
                     : baseIntervalMs;
    Serial.printf("-- link clear — recovering to ~%u Hz\n", 1000 / sendIntervalMs);
    lastCleanSendMs = nowMs;
  } else if (sendIntervalMs <= baseIntervalMs) {
    lastCleanSendMs = nowMs;
  }

  if (nowMs - lastStatMs >= STAT_INTERVAL_MS) sendStat(s, nowMs);
}

uint16_t netGetSendIntervalMs() { return sendIntervalMs; }
