// ============================================================================
//  Local web server (core 0). Pages are stored in LittleFS (/www). On first
//  boot after a flash, the copies embedded in web_assets.h are written there,
//  so no separate filesystem upload tool is needed.
//    setup mode : /            -> /www/setup.html  (scan list, password, Room ID, token)
//    normal     : /            -> /www/index.html  (local status page, polls /api/status every 1 s)
// ============================================================================
#include "web_assets.h"

static WebServer web(80);
extern String setupState, setupMsg;

void installWebAssets() {
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  if (!LittleFS.exists("/www")) LittleFS.mkdir("/www");
  File v = LittleFS.open("/www/ver.txt", "r");
  String cur = v ? v.readString() : "";
  if (v) v.close();
  if (cur != WEB_ASSETS_VERSION) {
    File f = LittleFS.open("/www/index.html", "w"); f.print(INDEX_HTML); f.close();
    f = LittleFS.open("/www/setup.html", "w"); f.print(SETUP_HTML); f.close();
    f = LittleFS.open("/www/ver.txt", "w"); f.print(WEB_ASSETS_VERSION); f.close();
    Serial.println("[NET] web pages written to LittleFS");
  }
  xSemaphoreGive(gFsLock);
}

static void sendFile(const char* path) {
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  File f = LittleFS.open(path, "r");
  if (!f) { xSemaphoreGive(gFsLock); web.send(404, "text/plain", "missing"); return; }
  String s = f.readString(); f.close();
  xSemaphoreGive(gFsLock);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/html; charset=utf-8", s);
}

static const char* netName(NetState n) {
  switch (n) { case NET_SETUP: return "SETUP"; case NET_CONNECTING: return "CONNECTING"; case NET_LIVE: return "LIVE";
               case NET_SYNCING: return "SYNCING"; default: return "OFFLINE"; }
}

static void handleStatus() {
  JsonDocument d;
  char tbuf[32] = "-";
  time_t now = time(nullptr);
  if (gTimeSynced) { struct tm tmv; localtime_r(&now, &tmv); strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tmv); }
  uint32_t ms = millis();
  LOCK();
  d["device_id"] = DEVICE_ID; d["room_id"] = gRoomId; d["time"] = tbuf;
  d["uptime_s"] = ms / 1000;
  if (!isnan(S.tempC)) { d["temp_c"] = S.tempC; d["rh"] = S.rh; d["heat_index_c"] = S.hi; }
  d["status"] = statusName(S.status);
  d["trend"] = trendName(S.trend);
  d["quality"] = (S.qualityGood && S.dhtOk && S.rfidOk) ? "good" : "bad";
  d["dht_ok"] = S.dhtOk; d["rfid_ok"] = S.rfidOk;
  d["flame"] = S.flame;
  d["door"] = S.doorUnlocked ? "UNLOCKED" : "LOCKED";
  d["fire"] = S.fireLatched; d["fire_silenced"] = S.fireSilenced;
  d["cannot_silence"] = ms < S.cannotSilenceUntil;
  d["panic"] = ms < S.panicUntil;
  d["last_uid"] = S.lastUid;
  d["warn_c"] = S.warnC; d["danger_c"] = S.dangerC;
  d["weather_line"] = S.weatherLine;
  d["weather_alert"] = S.weatherDanger; d["weather_text"] = S.weatherText;
  d["check_name"] = S.checkName; d["check_result"] = S.checkResult; d["check_reason"] = S.checkReason;
  d["check_ts"] = (uint32_t)S.checkTs;
  d["cam_position"] = S.camPosition; d["cam_fault"] = S.camFault;
  d["people"] = S.people; d["over_capacity"] = S.overCapacity; d["intruder"] = S.intruder; d["night"] = S.nightMode;
  d["presence"] = S.presence; d["zone"] = S.zone;
  d["hand_seat"] = S.handSeat; d["hand_ts"] = (uint32_t)S.handTs;
  d["conn"] = netName(S.net); d["pending"] = S.pending;
  UNLOCK();
  d["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  d["chip_temp_c"] = temperatureRead();
  d["fw"] = FW_VERSION;
  String out; serializeJson(d, out);
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", out);
}

static void handleRoot() {
  LOCK(); bool setup = S.net == NET_SETUP; UNLOCK();
  sendFile(setup ? "/www/setup.html" : "/www/index.html");
}

static void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); web.send(200, "application/json", "{\"scanning\":true,\"list\":[]}"); return; }
  if (n == WIFI_SCAN_RUNNING) { web.send(200, "application/json", "{\"scanning\":true,\"list\":[]}"); return; }
  JsonDocument d; d["scanning"] = false;
  JsonArray a = d["list"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i); o["rssi"] = WiFi.RSSI(i); o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  WiFi.scanNetworks(true);          // refresh for the next poll
  String out; serializeJson(d, out);
  web.send(200, "application/json", out);
}

static void handleSave() {
  String ssid = web.arg("ssid"), pass = web.arg("pass"), room = web.arg("room"), token = web.arg("token"), server = web.arg("server");
  ssid.trim(); room.trim(); token.trim(); server.trim();
  if (!ssid.length() || !room.length() || !token.length()) { web.send(400, "application/json", "{\"error\":\"SSID, Room ID and device token are required\"}"); return; }
  requestSetupTrial(ssid, pass, room, token, server);
  web.send(200, "application/json", "{\"ok\":true}");
}

static void handleSetupStatus() {
  JsonDocument d;
  d["state"] = setupState; d["msg"] = setupMsg;
  d["server"] = gServerUrl; d["room"] = gRoomId; d["device_id"] = DEVICE_ID;
  String out; serializeJson(d, out);
  web.send(200, "application/json", out);
}

void webTask(void*) {
  esp_task_wdt_add(nullptr);
  web.on("/", handleRoot);
  web.on("/setup", []() { sendFile("/www/setup.html"); });
  web.on("/api/status", handleStatus);
  web.on("/scan", handleScan);
  web.on("/save", HTTP_POST, handleSave);
  web.on("/setupstatus", handleSetupStatus);
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
  for (;;) {
    esp_task_wdt_reset();
    web.handleClient();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
