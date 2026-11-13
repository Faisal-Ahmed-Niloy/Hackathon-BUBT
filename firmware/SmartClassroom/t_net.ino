// ============================================================================
//  Network task (core 0): Wi-Fi setup mode, reconnect with growing retry gaps,
//  mDNS, NTP (Asia/Dhaka), the offline access point after 30 s without Wi-Fi,
//  and the Bluetooth LE beacon SC-<TeamID>-<STATE> while offline.
// ============================================================================
static String wifiSsid, wifiPass;
static volatile bool trialRequested = false;
static String trialSsid, trialPass, trialRoom, trialToken, trialServer;
String setupState = "idle";      // idle | connecting | ok | error   (read by the setup page)
String setupMsg = "";
static volatile uint8_t lastDiscReason = 0;
static bool apOn = false;
static bool bleReady = false, bleAdvertising = false;
static String bleName;
uint32_t wifiDownSince = 0;
uint32_t lastServerOk = 0;        // set by the cloud task
static bool mdnsUp = false;

static String randomApPass() {
  const char* cs = "abcdefghjkmnpqrstuvwxyz23456789";
  String p;
  for (int i = 0; i < 8; i++) p += cs[esp_random() % strlen(cs)];
  return p;
}

void loadConfig() {
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  gRoomId = prefs.getString("room", "");
  gToken = prefs.getString("token", "");
  gServerUrl = prefs.getString("server", DEFAULT_SERVER_URL);
#ifdef FAULT_TEST_SERVER_URL
  gServerUrl = FAULT_TEST_SERVER_URL;       // faulty build for the rollback test
#endif
  gApPass = prefs.getString("apPass", "");
  if (gApPass.length() != 8) { gApPass = randomApPass(); prefs.putString("apPass", gApPass); }
  loadOfflineCards();
}

void clearWifiAndRestart() {
  prefs.remove("ssid"); prefs.remove("pass");
  Serial.println("[NET] saved Wi-Fi details cleared - restarting in setup mode");
  delay(200);
  ESP.restart();
}

static void onWifiEvent(WiFiEvent_t e, WiFiEventInfo_t info) {
  if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) lastDiscReason = info.wifi_sta_disconnected.reason;
}

static void startAp(const char* why) {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, gApPass.c_str());
  apOn = true;
  Serial.printf("[NET] AP ON ssid=%s pass=%s ip=%s (%s)\n", AP_SSID, gApPass.c_str(),
                WiFi.softAPIP().toString().c_str(), why);
}
static void stopAp() {
  if (!apOn) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  Serial.println("[NET] AP OFF");
}

// called by the setup page handler (t_web.ino)
void requestSetupTrial(const String& ssid, const String& pass, const String& room, const String& token, const String& server) {
  trialSsid = ssid; trialPass = pass; trialRoom = room; trialToken = token; trialServer = server;
  setupState = "connecting"; setupMsg = "Connecting to " + ssid + " ...";
  trialRequested = true;
}

static void setNet(NetState n) {
  LOCK(); NetState old = S.net; S.net = n; uint32_t pend = S.pending; UNLOCK();
  if (old == n) return;
  switch (n) {
    case NET_LIVE:    Serial.println("[NET] LIVE"); break;
    case NET_OFFLINE: Serial.println("[NET] OFFLINE"); break;
    case NET_SYNCING: Serial.printf("[NET] SYNCING n=%lu\n", (unsigned long)pend); break;
    case NET_SETUP:   Serial.println("[NET] SETUP MODE"); break;
    default: break;
  }
}

// ---------------- BLE beacon ----------------
static void bleUpdate(bool offline) {
  if (!offline) {
    if (bleAdvertising) { BLEDevice::getAdvertising()->stop(); bleAdvertising = false; Serial.println("[BLE] advertising stopped"); }
    return;
  }
  LOCK();
  const char* st = S.fireLatched ? "FIRE" : (millis() < S.panicUntil ? "PANIC" : "OFFLINE");
  UNLOCK();
  String name = String("SC-") + TEAM_ID + "-" + st;
  if (bleAdvertising && name == bleName) return;
  if (!bleReady) { BLEDevice::init(name.c_str()); bleReady = true; }
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->stop();
  esp_ble_gap_set_device_name(name.c_str());
  BLEAdvertisementData d;
  d.setFlags(0x06);
  d.setName(name.c_str());
  adv->setAdvertisementData(d);
  adv->start();
  bleAdvertising = true; bleName = name;
  Serial.printf("[BLE] advertising %s\n", name.c_str());
}

static void onGotIp() {
  Serial.printf("[NET] connected ssid=%s ip=%s rssi=%d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (mdnsUp) MDNS.end();
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
    Serial.printf("[NET] mDNS http://%s.local\n", MDNS_NAME);
  }
  configTzTime("<+06>-6", "pool.ntp.org", "time.google.com", "time.cloudflare.com");   // Asia/Dhaka
}

void netTask(void*) {
  esp_task_wdt_add(nullptr);
  WiFi.onEvent(onWifiEvent);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);           // we control the retry gaps ourselves
  WiFi.setHostname(MDNS_NAME);
  Serial.printf("[NET] setup access point %s password: %s\n", AP_SSID, gApPass.c_str());

  bool haveCreds = wifiSsid.length() > 0;
  uint32_t trialStart = 0, nextTry = 0, backoff = 1000, apStopAt = 0;
  bool wasConnected = false;

  if (!haveCreds) {
    setNet(NET_SETUP);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, gApPass.c_str());
    apOn = true;
    Serial.printf("[NET] SETUP: join %s (password %s) and open http://%s\n", AP_SSID, gApPass.c_str(), WiFi.softAPIP().toString().c_str());
    WiFi.scanNetworks(true);
  } else {
    setNet(NET_CONNECTING);
    WiFi.mode(WIFI_STA);
    Serial.printf("[NET] connecting to %s\n", wifiSsid.c_str());
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    nextTry = millis() + 8000;
  }

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    bool conn = WiFi.status() == WL_CONNECTED;

    // ---------- setup trial (from the setup page) ----------
    if (trialRequested) {
      trialRequested = false;
      trialStart = now;
      lastDiscReason = 0;
      WiFi.mode(WIFI_AP_STA);
      WiFi.disconnect(false);
      Serial.printf("[NET] connecting to %s\n", trialSsid.c_str());
      WiFi.begin(trialSsid.c_str(), trialPass.c_str());
    }
    if (trialStart) {
      if (conn) {
        trialStart = 0;
        wifiSsid = trialSsid; wifiPass = trialPass;
        prefs.putString("ssid", wifiSsid); prefs.putString("pass", wifiPass);
        prefs.putString("room", trialRoom); prefs.putString("token", trialToken);
        if (trialServer.length()) prefs.putString("server", trialServer);
        gRoomId = trialRoom; gToken = trialToken;
#ifndef FAULT_TEST_SERVER_URL
        if (trialServer.length()) gServerUrl = trialServer;
#endif
        haveCreds = true;
        setupState = "ok";
        setupMsg = "Connected. Device IP " + WiFi.localIP().toString() + "  -  http://" MDNS_NAME ".local";
        onGotIp();
        wasConnected = true;
        setNet(NET_CONNECTING);
        apStopAt = now + 15000;                 // keep the AP briefly so the page shows success
      } else if (now - trialStart > WIFI_SETUP_TIMEOUT ||
                 lastDiscReason == WIFI_REASON_AUTH_FAIL || lastDiscReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                 lastDiscReason == WIFI_REASON_HANDSHAKE_TIMEOUT || lastDiscReason == WIFI_REASON_NO_AP_FOUND) {
        // give "not found" a little longer (first scan can miss), password errors are immediate
        if (lastDiscReason == WIFI_REASON_NO_AP_FOUND && now - trialStart < 8000) { /* wait */ }
        else {
          uint8_t r = lastDiscReason;
          trialStart = 0;
          WiFi.disconnect(false);
          setupState = "error";
          if (r == WIFI_REASON_NO_AP_FOUND) setupMsg = "Network not found / unreachable";
          else if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_HANDSHAKE_TIMEOUT)
            setupMsg = "Wrong Wi-Fi password";
          else setupMsg = "Could not connect (timeout)";
          Serial.printf("[NET] setup error: %s (reason %u)\n", setupMsg.c_str(), r);
          setNet(NET_SETUP);
          if (!apOn) startAp("setup");
        }
      }
    }
    if (apStopAt && now > apStopAt) { apStopAt = 0; if (conn) stopAp(); }

    // ---------- normal operation with saved credentials ----------
    if (haveCreds && !trialStart) {
      if (conn && !wasConnected) {
        wasConnected = true; backoff = 1000; wifiDownSince = 0;
        Serial.println("[NET] reconnected");
        onGotIp();
      }
      if (!conn && wasConnected) {
        wasConnected = false; wifiDownSince = now; nextTry = now + backoff;
        Serial.printf("[NET] disconnected (reason %u)\n", lastDiscReason);
      }
      if (!conn) {
        if (!wifiDownSince) wifiDownSince = now;
        if (now >= nextTry) {
          Serial.printf("[NET] reconnect attempt (next gap %lus)\n", (unsigned long)(backoff * 2 / 1000));
          WiFi.disconnect(false);
          WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
          nextTry = now + backoff;
          backoff = min<uint32_t>(backoff * 2, 8000);   // 1,2,4,8,8... s
        }
        if (now - wifiDownSince >= OFFLINE_AP_AFTER_MS) startAp("Wi-Fi down 30 s");
      }
      // ---------- connection state LIVE / OFFLINE / SYNCING ----------
      LOCK(); NetState ns = S.net; uint32_t pend = S.pending; UNLOCK();
      bool serverOk = conn && lastServerOk && (now - lastServerOk < SERVER_LIVE_MS);
      if (!serverOk) {
        if (ns == NET_LIVE || ns == NET_SYNCING || (ns == NET_CONNECTING && (!conn ? now - wifiDownSince > 5000 : now > 20000)))
          setNet(NET_OFFLINE);
      } else {
        if (ns == NET_OFFLINE || ns == NET_CONNECTING) setNet(pend > 2 ? NET_SYNCING : NET_LIVE);
        else if (ns == NET_SYNCING && pend <= 1) setNet(NET_LIVE);
      }
      if (conn && apOn && !apStopAt && serverOk) stopAp();
      bool offline = (ns == NET_OFFLINE);
      bleUpdate(offline);
      if (conn && !gTimeSynced && time(nullptr) > 1700000000) {
        gTimeSynced = true; gSyncEpoch = time(nullptr); gSyncMillis = millis();
        Serial.println("[NET] NTP time synced (Asia/Dhaka)");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
