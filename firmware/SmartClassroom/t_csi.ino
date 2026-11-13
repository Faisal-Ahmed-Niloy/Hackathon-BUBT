// ============================================================================
//  Wi-Fi CSI (Level 11). CSI is captured from packets sent by the access point
//  the board is connected to (filtered by its BSSID). To guarantee >= 20
//  packets/s the board pings its gateway every 30 ms; every reply carries CSI.
//  Features computed ON THE DEVICE every second:
//    16 subcarrier groups x (mean amplitude, std amplitude) + RSSI mean/std +
//    packet count  -> 35 numbers, sent to the server (camera is never used).
// ============================================================================
#define CSI_SC 64           // L-LTF subcarriers in HT20
#define CSI_GROUPS 16

static portMUX_TYPE csiMux = portMUX_INITIALIZER_UNLOCKED;
static float accSum[CSI_SC], accSq[CSI_SC];
static float rssiSum = 0, rssiSq = 0;
static volatile uint32_t accN = 0, pktTotal = 0;
static uint8_t apBssid[6];
static bool csiOn = false;
static float lastRate = 0;
static int lastSubcarriers = 0;
static String featQueue;           // JSON objects separated by ','
static int featCount = 0;
static esp_ping_handle_t pingH = nullptr;

static void IRAM_ATTR csiCb(void* ctx, wifi_csi_info_t* info) {
  if (!info || !info->buf || memcmp(info->mac, apBssid, 6) != 0) return;
  int n = min<int>(info->len / 2, CSI_SC);
  portENTER_CRITICAL_ISR(&csiMux);
  for (int i = 0; i < n; i++) {
    int8_t im = info->buf[2 * i], re = info->buf[2 * i + 1];
    float a = sqrtf((float)re * re + (float)im * im);
    accSum[i] += a; accSq[i] += a * a;
  }
  rssiSum += info->rx_ctrl.rssi; rssiSq += (float)info->rx_ctrl.rssi * info->rx_ctrl.rssi;
  accN++; pktTotal++;
  lastSubcarriers = n;
  portEXIT_CRITICAL_ISR(&csiMux);
}

static void csiStart() {
  memcpy(apBssid, WiFi.BSSID(), 6);
  wifi_csi_config_t cfg = {};
  cfg.lltf_en = true; cfg.htltf_en = false; cfg.stbc_htltf2_en = false;
  cfg.ltf_merge_en = true; cfg.channel_filter_en = false; cfg.manu_scale = false;
  esp_wifi_set_csi_config(&cfg);
  esp_wifi_set_csi_rx_cb(csiCb, nullptr);
  esp_wifi_set_csi(true);

  esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
  ip_addr_t gw; gw.type = IPADDR_TYPE_V4; gw.u_addr.ip4.addr = (uint32_t)WiFi.gatewayIP();
  pc.target_addr = gw;
  pc.count = ESP_PING_COUNT_INFINITE;
  pc.interval_ms = 30;
  pc.timeout_ms = 500;
  pc.data_size = 16;
  pc.task_stack_size = 3072;
  esp_ping_callbacks_t cbs = {};
  if (esp_ping_new_session(&pc, &cbs, &pingH) == ESP_OK) esp_ping_start(pingH);
  csiOn = true;
  Serial.printf("[NET] CSI capture on, channel %d, AP %s\n", WiFi.channel(), WiFi.BSSIDstr().c_str());
}

static void csiStop() {
  esp_wifi_set_csi(false);
  if (pingH) { esp_ping_stop(pingH); esp_ping_delete_session(pingH); pingH = nullptr; }
  csiOn = false;
}

void csiPrint() {
  Serial.printf("[CMD] csi rate=%.1f pkt/s subcarriers=%d channel=%d capture=%s\n",
                lastRate, lastSubcarriers, WiFi.isConnected() ? WiFi.channel() : 0, csiOn ? "on" : "off");
}

bool csiTakeFeatures(String& out) {
  if (!featCount) return false;
  LOCK();
  String q = featQueue; int n = featCount;
  featQueue = ""; featCount = 0;
  UNLOCK();
  out = String("{\"device_id\":\"") + DEVICE_ID + "\",\"n\":" + n + ",\"windows\":[" + q + "]}";
  return true;
}

void csiTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t lastWin = millis();
  for (;;) {
    esp_task_wdt_reset();
    bool conn = WiFi.isConnected();
    if (conn && !csiOn && millis() > 15000) csiStart();
    if (!conn && csiOn) csiStop();
    if (csiOn && memcmp(apBssid, WiFi.BSSID(), 6) != 0) memcpy(apBssid, WiFi.BSSID(), 6);

    uint32_t now = millis();
    if (now - lastWin >= 1000) {
      float dt = (now - lastWin) / 1000.0f;
      lastWin = now;
      float sum[CSI_SC], sq[CSI_SC], rs, rq; uint32_t n;
      portENTER_CRITICAL(&csiMux);
      memcpy(sum, accSum, sizeof(sum)); memcpy(sq, accSq, sizeof(sq));
      rs = rssiSum; rq = rssiSq; n = accN;
      memset(accSum, 0, sizeof(accSum)); memset(accSq, 0, sizeof(accSq));
      rssiSum = rssiSq = 0; accN = 0;
      portEXIT_CRITICAL(&csiMux);
      lastRate = n / dt;
      if (n >= 5) {
        String o = String("{\"ts\":") + (uint32_t)tsNowOrZero() + ",\"pps\":" + String(lastRate, 1) + ",\"f\":[";
        int per = CSI_SC / CSI_GROUPS;
        for (int g = 0; g < CSI_GROUPS; g++) {
          float m = 0, v = 0;
          for (int k = 0; k < per; k++) {
            int i = g * per + k;
            float mi = sum[i] / n;
            m += mi; v += sq[i] / n - mi * mi;
          }
          m /= per; v = sqrtf(max(0.0f, v / per));
          o += String(m, 2) + "," + String(v, 2) + ",";
        }
        float rm = rs / n, rsd = sqrtf(max(0.0f, rq / n - rm * rm));
        o += String(rm, 1) + "," + String(rsd, 2) + "," + String(n) + "]}";
        LOCK();
        if (featCount < 10) { if (featCount) featQueue += ","; featQueue += o; featCount++; }
        UNLOCK();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
