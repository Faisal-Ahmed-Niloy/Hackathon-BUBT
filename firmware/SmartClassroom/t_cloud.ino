// ============================================================================
//  Cloud task (core 0): HTTPS to the platform with the server certificate
//  checked against ca_cert.h, device token as Bearer auth.
//   - sends the outbox oldest-first in batches; (device_id, seq) duplicates
//     are acknowledged by the server and never stored twice
//   - executes commands returned in each response (settings, reset fire, OTA,
//     weather alert, teacher tags, camera/presence state, offline card list)
//   - OTA: download over HTTPS, SHA-256 + image check, other OTA slot, restart;
//     rollback to the previous slot if the new firmware cannot reach the server
//     within 60 s.
// ============================================================================
// ca_cert.h is included from app.h

static WiFiClientSecure tls;
static HTTPClient http;
static uint32_t recentCmds[8];
static uint8_t recentIdx = 0;
static String pendingAcks;            // comma separated command ids to ack
static bool serverWasOk = false;
static bool otaPendingConfirm = false;
static uint32_t otaBootMs = 0;
String gWeatherAlertText = "";

// ---------------------------------------------------------------- OTA boot --
void otaBootCheck() {
  String rb = prefs.getString("rbFrom", "");
  if (rb.length()) {                                     // we are the old firmware again
    Serial.printf("[OTA] rolled back from %s, running %s\n", rb.c_str(), FW_VERSION);
    emitEvent("OTA_ROLLBACK", String("from=") + rb + " to=" + FW_VERSION);
    prefs.remove("rbFrom");
  }
  if (prefs.getBool("otaPend", false)) {
    uint32_t boots = prefs.getUInt("otaBoots", 0) + 1;
    prefs.putUInt("otaBoots", boots);
    Serial.printf("[OTA] new firmware %s started (boot %lu), must reach the server within 60 s\n", FW_VERSION, (unsigned long)boots);
    otaPendingConfirm = true;
    otaBootMs = millis();
    if (boots > 3) otaRollback("crash loop");
  } else {
    Serial.printf("[OTA] running firmware %s\n", FW_VERSION);
  }
}

void otaRollback(const char* why) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* prev = esp_ota_get_next_update_partition(running);   // the other slot
  Serial.printf("[OTA] ROLLBACK (%s): %s -> %s\n", why, running->label, prev ? prev->label : "?");
  prefs.putString("rbFrom", FW_VERSION);
  prefs.putBool("otaPend", false);
  prefs.putUInt("otaBoots", 0);
  if (prev && esp_ota_set_boot_partition(prev) == ESP_OK) {
    delay(300);
    ESP.restart();
  }
  Serial.println("[OTA] rollback failed - staying on this firmware");
  prefs.remove("rbFrom");
}

static void otaConfirm() {
  if (!otaPendingConfirm) return;
  otaPendingConfirm = false;
  prefs.putBool("otaPend", false);
  prefs.putUInt("otaBoots", 0);
  esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("[OTA] firmware %s confirmed\n", FW_VERSION);
  emitEvent("OTA_DONE", String("version=") + FW_VERSION);
}

static bool doOta(const String& url, const String& shaHex, uint32_t size, const String& ver) {
  Serial.printf("[OTA] download %s (%lu bytes) version %s\n", url.c_str(), (unsigned long)size, ver.c_str());
  WiFiClientSecure c; c.setCACert(ROOT_CA); c.setTimeout(15);
  HTTPClient h;
  if (!h.begin(c, url)) { emitEvent("OTA_REJECTED", "bad url"); return false; }
  h.addHeader("Authorization", "Bearer " + gToken);
  int code = h.GET();
  if (code != 200) { Serial.printf("[OTA] HTTP %d\n", code); emitEvent("OTA_REJECTED", String("http=") + code); h.end(); return false; }
  int len = h.getSize();
  if (len <= 0 || (size && (uint32_t)len != size)) { emitEvent("OTA_REJECTED", "size mismatch"); h.end(); return false; }
  if (!Update.begin(len, U_FLASH)) { emitEvent("OTA_REJECTED", "no space"); h.end(); return false; }
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
  WiFiClient* st = h.getStreamPtr();
  static uint8_t buf[4096];
  int got = 0, lastPct = -10;
  uint32_t lastData = millis();
  while (got < len) {
    esp_task_wdt_reset();
    size_t avail = st->available();
    if (avail) {
      int r = st->readBytes(buf, min<size_t>(avail, sizeof(buf)));
      if (r > 0) {
        mbedtls_sha256_update(&ctx, buf, r);
        if (Update.write(buf, r) != (size_t)r) break;
        got += r; lastData = millis();
        int pct = (int)((int64_t)got * 100 / len);
        if (pct >= lastPct + 10) { lastPct = pct; Serial.printf("[OTA] progress %d%%\n", pct); }
      }
    } else {
      if (!h.connected() || millis() - lastData > 15000) break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  h.end();
  uint8_t dig[32]; mbedtls_sha256_finish(&ctx, dig); mbedtls_sha256_free(&ctx);
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", dig[i]); hex[64] = 0;
  bool shaOk = shaHex.length() == 0 || shaHex.equalsIgnoreCase(hex);
  if (got != len || !shaOk) {
    Update.abort();
    Serial.printf("[OTA] check FAILED (got %d/%d, sha %s) - keeping %s\n", got, len, shaOk ? "ok" : "mismatch", FW_VERSION);
    emitEvent("OTA_REJECTED", got != len ? "incomplete" : "sha256 mismatch");
    return false;
  }
  if (!Update.end(true)) {          // esp_ota_end verifies the image structure + checksum
    Serial.printf("[OTA] image check FAILED: %s - keeping %s\n", Update.errorString(), FW_VERSION);
    emitEvent("OTA_REJECTED", String("image invalid: ") + Update.errorString());
    return false;
  }
  Serial.println("[OTA] check OK, installed into the other OTA slot - restarting");
  prefs.putBool("otaPend", true);
  prefs.putUInt("otaBoots", 0);
  delay(500);
  ESP.restart();
  return true;
}

// ------------------------------------------------------------ commands ------
static bool seenCmd(uint32_t id) {
  for (auto v : recentCmds) if (v == id) return true;
  recentCmds[recentIdx++ % 8] = id;
  return false;
}

static void handleCommand(JsonObject c) {
  uint32_t id = c["id"] | 0;
  if (pendingAcks.length()) pendingAcks += ",";
  pendingAcks += String(id);
  if (seenCmd(id)) return;
  if (id && id == prefs.getUInt("otaCmd", 0)) return;     // OTA command already handled before a restart
  String type = c["type"] | "";
  if (type == "settings") {
    applySettings(c["warn_c"] | DEFAULT_WARN_C, c["danger_c"] | DEFAULT_DANGER_C);
  } else if (type == "reset_fire") {
    Serial.println("[EVENT] RESET_FIRE received");
    gResetFireReq = true;
  } else if (type == "ota") {
    String ver = c["version"] | "";
    if (ver.length() && ver == FW_VERSION) { Serial.printf("[OTA] already running %s - skipped
", FW_VERSION); return; }
    prefs.putUInt("otaCmd", id);
    doOta(c["url"] | "", c["sha256"] | "", c["size"] | 0, ver);
  } else if (type == "weather") {
    bool active = c["active"] | false;
    String sev = c["severity"] | "DANGER";
    String txt = c["text"] | "";
    LOCK();
    bool was = S.weatherDanger;
    S.weatherDanger = active;
    strncpy(S.weatherText, txt.c_str(), sizeof(S.weatherText) - 1);
    UNLOCK();
    Serial.printf("[EVENT] WEATHER_ALERT %s %s\n", active ? sev.c_str() : "CLEARED", txt.c_str());
    if (active && !was) gWeatherBeepReq = true;          // three beeps once, on arrival
  } else if (type == "teacher_tags") {
    setTeacherTags(c["uids"] | "");
    Serial.printf("[EVENT] TEACHER_TAGS %s\n", getTeacherTags().c_str());
  } else if (type == "event") {                          // server events shown on Serial
    Serial.printf("[EVENT] %s %s\n", (const char*)(c["event"] | ""), (const char*)(c["detail"] | ""));
  } else if (type == "camera_position") {
    String p = c["position"] | "DOOR";
    LOCK(); strncpy(S.camPosition, p.c_str(), sizeof(S.camPosition) - 1); UNLOCK();
    Serial.printf("[CAM] position=%s\n", p.c_str());
  }
}

static void applyState(JsonObject st) {
  if (st.isNull()) return;
  LOCK();
  if (st["weather_line"].is<const char*>()) strncpy(S.weatherLine, st["weather_line"], sizeof(S.weatherLine) - 1);
  if (st["people"].is<int>()) S.people = st["people"];
  if (st["over_capacity"].is<bool>()) S.overCapacity = st["over_capacity"];
  if (st["intruder"].is<bool>()) S.intruder = st["intruder"];
  if (st["night"].is<bool>()) S.nightMode = st["night"];
  if (st["presence"].is<const char*>()) strncpy(S.presence, st["presence"], sizeof(S.presence) - 1);
  if (st["zone"].is<const char*>()) strncpy(S.zone, st["zone"], sizeof(S.zone) - 1);
  if (st["hand_seat"].is<const char*>()) strncpy(S.handSeat, st["hand_seat"], sizeof(S.handSeat) - 1);
  if (st["hand_ts"].is<uint32_t>()) S.handTs = st["hand_ts"];
  UNLOCK();
  if (st["camera_fault"].is<const char*>()) setCameraFault(st["camera_fault"]);
  if (st["offline_cards"].is<const char*>()) setOfflineCards(st["offline_cards"] | "", st["class_end"] | 0);
}

void handleServerJson(const String& body) {
  JsonDocument d;
  if (deserializeJson(d, body)) return;
  applyState(d["state"]);
  for (JsonObject c : d["cmds"].as<JsonArray>()) handleCommand(c);
}

// --------------------------------------------------------------- HTTP ------
int httpPost(const String& path, const String& body, String& resp) {
  if (!WiFi.isConnected() || gServerUrl.length() < 8) return -1;
  if (strlen(ROOT_CA) < 100) { Serial.println("[ERROR] ca_cert.h not generated - refusing insecure HTTPS"); return -2; }
  http.setReuse(true);
  http.setTimeout(8000);
  if (!http.begin(tls, gServerUrl + path)) return -3;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + gToken);
  http.addHeader("X-Device", DEVICE_ID);
  int code = http.POST(body);
  resp = code > 0 ? http.getString() : "";
  http.end();
  return code;
}

static void markServer(bool ok, int code) {
  if (ok) {
    lastServerOk = millis();
    if (!serverWasOk) { serverWasOk = true; Serial.printf("[NET] server connected %s\n", gServerUrl.c_str()); }
    otaConfirm();
  } else if (serverWasOk) {
    serverWasOk = false;
    Serial.printf("[NET] server disconnected (code %d), will retry\n", code);
  }
}

// rewrite ts for records taken before NTP sync in this boot
static void fixTimestamps(String& arr) {
  if (!gTimeSynced || arr.indexOf("\"ts\":0") < 0) return;
  JsonDocument d;
  if (deserializeJson(d, arr)) return;
  for (JsonObject r : d.as<JsonArray>()) {
    if ((r["ts"] | 0) == 0 && (r["boot"] | 0) == gBootCount) {
      uint32_t up = r["up_ms"] | 0;
      r["ts"] = (uint32_t)(gSyncEpoch - (time_t)((int32_t)(gSyncMillis - up) / 1000));
    }
  }
  arr = ""; serializeJson(d, arr);
}

void cloudTask(void*) {
  esp_task_wdt_add(nullptr);
  tls.setCACert(ROOT_CA);
  tls.setTimeout(8);
  uint32_t lastPost = 0, retryGap = 1000, lastCsi = 0;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();

    if (otaPendingConfirm && now - otaBootMs > OTA_CONFIRM_MS) otaRollback("server not reached in 60 s");

    bool canTry = WiFi.isConnected() && gToken.length() && now - lastPost >= retryGap;
    LOCK(); uint32_t pend = S.pending; UNLOCK();
    if (canTry && (pend > 0 || pendingAcks.length() || now - lastPost > 5000)) {
      lastPost = now;
      String recs; uint32_t eSeg, eOff;
      int n = outboxPeek(recs, 25, eSeg, eOff);
      fixTimestamps(recs);
      String body = String("{\"device_id\":\"") + DEVICE_ID + "\",\"fw\":\"" + FW_VERSION +
                    "\",\"teacher_tags\":\"" + getTeacherTags() + "\",\"acks\":[" + pendingAcks + "],\"records\":" + recs + "}";
      String resp;
      int code = httpPost("/api/device/ingest", body, resp);
      bool ok = (code == 200 || code == 409);       // 409 = all were replays already stored
      markServer(ok, code);
      if (ok) {
        outboxAck(eSeg, eOff, n);
        pendingAcks = "";
        retryGap = 200;                              // drain the backlog quickly
        handleServerJson(resp);
        LOCK(); bool more = S.pending > 0; UNLOCK();
        if (!more) retryGap = 1000;
      } else {
        if (code == 401) Serial.println("[ERROR] server refused the device token (401)");
        retryGap = min<uint32_t>(retryGap < 1000 ? 1000 : retryGap * 2, 8000);
      }
    }

    // CSI features (Level 11) roughly once a second
    if (serverWasOk && now - lastCsi >= 1000) {
      lastCsi = now;
      String feats;
      if (csiTakeFeatures(feats)) { String r; int code = httpPost("/api/device/csi", feats, r); if (code == 200) handleServerJson(r); }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
