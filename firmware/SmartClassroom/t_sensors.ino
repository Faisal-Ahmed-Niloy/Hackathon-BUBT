// ============================================================================
//  Sensor task (core 1, 20 ms tick, never blocks):
//    flame every 100 ms, RFID every 200 ms, DHT11 every 5 s,
//    touch pad every 50 ms, BOOT button every 50 ms.
// ============================================================================
static DHT dht(PIN_DHT, DHT11);
static MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);

static int  dhtFailCount = 0;
static int  rfidBadCount = 0;
static bool cardWasPresent = false;
static int  cardAbsentCount = 0;
static char cardLastUid[24] = "";

uint32_t touchBaseline = 0, touchThreshold = 0;
volatile uint32_t touchRaw = 0;
volatile bool touchActive = false;
bool gStartupFlame = false;

bool readFlameRaw() {
  int v = digitalRead(PIN_FLAME);
  return FLAME_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

static bool rfidVersionOk() {
  byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  // genuine 0x91/0x92, clones 0x88/0x12/0x18/0xB2; unplugged SDA reads 0x00 or 0xFF
  return v == 0x91 || v == 0x92 || v == 0x88 || v == 0x12 || v == 0x18 || v == 0xB2;
}

void sensorsInit() {
  pinMode(PIN_FLAME, INPUT_PULLUP);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  // Level 1 task 6: check the flame sensor FIRST. Flame -> door UNLOCKED.
  bool flameNow = readFlameRaw();
  LOCK();
  S.flame = flameNow;
  gStartupFlame = flameNow;
  S.doorUnlocked = flameNow || S.fireLatched;   // fire latch restored from flash keeps door open
  UNLOCK();
  Serial.printf("[EVENT] STARTUP flame=%d door=%s\n", flameNow ? 1 : 0, (flameNow || S.fireLatched) ? "UNLOCKED" : "LOCKED");

  dht.begin();

  SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  LOCK(); S.rfidOk = rfidVersionOk(); UNLOCK();
  if (!S.rfidOk) Serial.println("[ERROR] SENSOR ERROR: RFID");

  // Touch panic pad: calibrate the baseline at start-up (pad must not be touched)
  uint64_t sum = 0;
  for (int i = 0; i < 32; i++) { sum += touchRead(PIN_TOUCH); delay(5); }
  touchBaseline = sum / 32;
  // ESP32-S3 touch values RISE when touched
  touchThreshold = touchBaseline + max<uint32_t>(touchBaseline / 5, 1500);
  Serial.printf("[EVENT] TOUCH_CAL pin=GPIO%d baseline=%lu threshold=%lu\n",
                PIN_TOUCH, (unsigned long)touchBaseline, (unsigned long)touchThreshold);
}

static void pollFlame() {
  bool f = readFlameRaw();
  LOCK(); S.flame = f; UNLOCK();
}

static void pollDht() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool ok = !isnan(t) && !isnan(h);
  if (!ok) {
    dhtFailCount++;
    // 2 consecutive failures at a 5 s period -> error reported in <= 10 s (< 12 s)
    if (dhtFailCount == 2) {
      LOCK(); S.dhtOk = false; S.qualityGood = false; UNLOCK();
      Serial.println("[ERROR] SENSOR ERROR: DHT11");
      emitEvent("SENSOR_ERROR", "DHT11");
    } else if (dhtFailCount == 1) {
      LOCK(); S.qualityGood = false; UNLOCK();
    }
    return;
  }
  bool wasErr = dhtFailCount >= 2;
  dhtFailCount = 0;
  if (wasErr) { Serial.println("[EVENT] SENSOR_OK DHT11"); }
  onDhtReading(t, h);   // t_safety.ino validates range, quality, trend, heat index
  LOCK(); S.dhtOk = true; UNLOCK();
}

static void pollRfid() {
  if (!rfidVersionOk()) {
    rfidBadCount++;
    if (rfidBadCount == 2) {                 // 400 ms -> well inside 3 s
      LOCK(); S.rfidOk = false; UNLOCK();
      Serial.println("[ERROR] SENSOR ERROR: RFID");
      emitEvent("SENSOR_ERROR", "RFID");
    }
    if (rfidBadCount % 5 == 0) rfid.PCD_Init();   // try to bring it back
    return;
  }
  if (rfidBadCount >= 2) {
    rfid.PCD_Init();
    rfid.PCD_SetAntennaGain(rfid.RxGain_max);
    Serial.println("[EVENT] SENSOR_OK RFID");
  }
  rfidBadCount = 0;
  LOCK(); S.rfidOk = true; UNLOCK();

  // WakeupA also wakes a card that is still held (halted) on the reader,
  // so a held card is seen every poll but reported only once.
  byte atqa[2]; byte len = sizeof(atqa);
  MFRC522::StatusCode st = rfid.PICC_WakeupA(atqa, &len);
  bool present = false;
  char uid[24] = "";
  if (st == MFRC522::STATUS_OK || st == MFRC522::STATUS_COLLISION) {
    if (rfid.PICC_ReadCardSerial()) {
      present = true;
      int p = 0;
      for (byte i = 0; i < rfid.uid.size && p < 22; i++) p += sprintf(uid + p, "%02X", rfid.uid.uidByte[i]);
    }
    rfid.PICC_HaltA();
  }
  if (present) {
    cardAbsentCount = 0;
    if (!cardWasPresent || strcmp(uid, cardLastUid) != 0) {
      cardWasPresent = true;
      strncpy(cardLastUid, uid, sizeof(cardLastUid));
      handleCardTap(uid);        // t_access.ino
    }
  } else if (cardWasPresent && ++cardAbsentCount >= 2) {
    cardWasPresent = false;      // removed for 2 polls (400 ms) -> next tap counts again
  }
}

static void pollTouch() {
  uint32_t v = touchRead(PIN_TOUCH);
  touchRaw = v;
  touchActive = v > touchThreshold;
}

static void pollBootButton() {
  static uint32_t pressedAt = 0;
  static bool fired = false;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (!pressedAt) pressedAt = millis();
    if (!fired && millis() - pressedAt >= 5000) {
      fired = true;
      Serial.println("[NET] BOOT held 5 s - clearing saved Wi-Fi details");
      clearWifiAndRestart();     // t_net.ino
    }
  } else { pressedAt = 0; fired = false; }
}

void sensorTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t tFlame = 0, tRfid = 0, tDht = millis() - DHT_PERIOD_MS + 1500, tTouch = 0;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    if (now - tFlame >= FLAME_PERIOD_MS) { tFlame = now; pollFlame(); }
    if (now - tTouch >= 50)              { tTouch = now; pollTouch(); pollBootButton(); }
    if (now - tRfid >= RFID_PERIOD_MS)   { tRfid = now; pollRfid(); }
    if (now - tDht >= DHT_PERIOD_MS)     { tDht = now; pollDht(); }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
