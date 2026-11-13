// ============================================================================
//  Serial task (core 1): [STATUS] line every second in the fixed format and the
//  Serial commands (115200 baud, newline line ending). This is the "main" task
//  that `hang` freezes to prove the watchdog.
// ============================================================================
static volatile bool tagAddArmed = false;
static String cmdBuf;

bool registerNextTag() {
  if (!tagAddArmed) return false;
  tagAddArmed = false;
  LOCK(); String uid = S.lastUid; UNLOCK();
  String t = getTeacherTags();
  if (t.length()) t += ",";
  t += uid;
  setTeacherTags(t);
  Serial.printf("[CMD] Teacher tag registered: %s (list: %s)\n", uid.c_str(), t.c_str());
  beepShort();
  return true;
}

void printStatusLine() {
  char T[12] = "-", RH[8] = "-", HI[12] = "-";
  LOCK();
  if (!isnan(S.tempC)) { snprintf(T, sizeof(T), "%.1fC", S.tempC); snprintf(RH, sizeof(RH), "%.0f%%", S.rh); snprintf(HI, sizeof(HI), "%.1fC", S.hi); }
  bool flame = S.flame; bool door = S.doorUnlocked;
  const char* st = statusName(S.status);
  const char* tr = trendName(S.trend);
  UNLOCK();
  Serial.printf("[STATUS] up=%lus T=%s RH=%s HI=%s flame=%d door=%s state=%s trend=%s rssi=%d heap=%luk psram=%luk chip=%.1fC fw=%s\n",
                (unsigned long)(millis() / 1000), T, RH, HI, flame ? 1 : 0, door ? "UNLOCKED" : "LOCKED", st, tr,
                WiFi.isConnected() ? WiFi.RSSI() : 0, (unsigned long)(ESP.getFreeHeap() / 1024),
                (unsigned long)(ESP.getFreePsram() / 1024), temperatureRead(), FW_VERSION);
}

static void cmdHealth() {
  Serial.printf("[CMD] health chip_temp=%.1fC free_heap=%luk free_psram=%luk flash=%luMB cpu=%luMHz rssi=%d uptime=%lus reset_reason=%s fw=%s\n",
                temperatureRead(), (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getFreePsram() / 1024),
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)), (unsigned long)getCpuFrequencyMhz(),
                WiFi.isConnected() ? WiFi.RSSI() : 0, (unsigned long)(millis() / 1000), gResetReason.c_str(), FW_VERSION);
}

static void cmdTasks() {
  Serial.printf("[CMD] tasks (%d)\n", gTaskCount);
  for (int i = 0; i < gTaskCount; i++) {
    BaseType_t core = xTaskGetAffinity(gTasks[i].h);
    Serial.printf("[CMD]   %-8s core=%d free_stack=%u bytes\n", gTasks[i].name, core == tskNO_AFFINITY ? -1 : (int)core,
                  (unsigned)uxTaskGetStackHighWaterMark(gTasks[i].h));
  }
}

static void serialCommand(String c) {
  c.trim();
  if (!c.length()) return;
  if (c == "help") {
    Serial.println("[CMD] commands: help status health tasks ver hang reset csi | tag add | tag list | tag clear");
  } else if (c == "status") {
    printStatusLine();
  } else if (c == "health") {
    cmdHealth();
  } else if (c == "tasks") {
    cmdTasks();
  } else if (c == "ver") {
    Serial.printf("[CMD] ver firmware=%s build=%s %s device=%s\n", FW_VERSION, __DATE__, __TIME__, DEVICE_ID);
  } else if (c == "hang") {
    Serial.println("[CMD] hang: freezing the serial (main) task - watchdog must restart the board within 10 s");
    for (;;) { /* no esp_task_wdt_reset() here on purpose */ }
  } else if (c == "reset") {
    Serial.println("[CMD] reset: clearing saved Wi-Fi details");
    clearWifiAndRestart();
  } else if (c == "csi") {
    csiPrint();
  } else if (c == "tag add") {
    tagAddArmed = true;
    Serial.println("[CMD] tap the key tag now to register it as a Teacher tag");
  } else if (c == "tag list") {
    Serial.printf("[CMD] Teacher tags: %s\n", getTeacherTags().c_str());
  } else if (c == "tag clear") {
    setTeacherTags("");
    Serial.println("[CMD] Teacher tags cleared");
  } else {
    Serial.printf("[CMD] unknown command '%s' - type help\n", c.c_str());
  }
}

void serialTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t lastStatus = 0;
  for (;;) {
    esp_task_wdt_reset();
    while (Serial.available()) {
      char ch = Serial.read();
      if (ch == '\n' || ch == '\r') { if (cmdBuf.length()) { String c = cmdBuf; cmdBuf = ""; serialCommand(c); } }
      else if (cmdBuf.length() < 64) cmdBuf += ch;
    }
    if (millis() - lastStatus >= STATUS_LINE_MS) { lastStatus += STATUS_LINE_MS; if (millis() - lastStatus > 2000) lastStatus = millis(); printStatusLine(); }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
