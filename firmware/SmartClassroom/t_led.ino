// ============================================================================
//  LED + buzzer task (core 1, 10 ms tick). Implements the Device Signals table.
//  When several states apply, the one with the lower number wins:
//   1 Fire  2 Panic  3 Granted  4 Denied  5 Heat Danger  6 Sensor error
//   7 Camera fault  8 Weather Danger  9 Warning  10 Setup/offline  11 Safe
// ============================================================================

// One-shot beep sequence (durations in ms: on, off, on, off ...)
static uint16_t beepSeq[8];
static uint8_t  beepLen = 0, beepIdx = 0;
static uint32_t beepStepAt = 0;

void beepPattern(const uint16_t* seq, uint8_t len) {
  LOCK();
  memcpy(beepSeq, seq, len * sizeof(uint16_t));
  beepLen = len; beepIdx = 0; beepStepAt = millis();
  UNLOCK();
}
void beepShort()  { static const uint16_t p[] = {120, 0};            beepPattern(p, 2); }
void beepTwo()    { static const uint16_t p[] = {100, 120, 100, 0};  beepPattern(p, 4); }
void beepThree()  { static const uint16_t p[] = {120, 150, 120, 150, 120, 0}; beepPattern(p, 6); }

static void setRgb(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xFFFFFFFF;
  uint32_t v = (r << 16) | (g << 8) | b;
  if (v == last) return;                // avoid re-sending identical colour
  last = v;
  neopixelWrite(PIN_RGB, r, g, b);      // arduino-esp32 built-in WS2812 driver
}

void ledBuzzerInit() {
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  setRgb(0, 0, 0);
}

void ledTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t lastHeatBeep = 0;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    bool hb = (now % 500) < 250;          // 2 Hz heartbeat: 250 ms on / 250 ms off

    LOCK();
    bool fire = S.fireLatched, silenced = S.fireSilenced;
    bool panic = now < S.panicUntil;
    bool granted = now < S.grantedUntil;
    uint32_t deniedAt = S.deniedAt;
    bool denied = deniedAt && (now - deniedAt) < 900;
    bool danger = S.status == ST_DANGER;
    bool warning = S.status == ST_WARNING;
    bool sensorErr = !S.dhtOk || !S.rfidOk;
    bool camFault = strcmp(S.camFault, "OK") != 0;
    bool weather = S.weatherDanger;
    bool offline = S.net == NET_SETUP || S.net == NET_OFFLINE || S.net == NET_CONNECTING;
    bool beeping = beepLen > 0;
    UNLOCK();

    bool buzz = false;
    if (fire) {                                   // 1 Fire: red 5 flashes/s
      if ((now % 200) < 100) setRgb(255, 0, 0); else setRgb(0, 0, 0);
      if (!silenced) buzz = (now % 400) < 200;    // 200 on / 200 off continuous
    } else if (panic) {                           // 2 Panic: red/blue alternating
      if ((now % 500) < 250) setRgb(255, 0, 0); else setRgb(0, 0, 255);
      buzz = (now % 200) < 100;                   // 100/100 for 10 s
    } else if (granted) {                         // 3 Access granted: solid green
      setRgb(0, 255, 0);
    } else if (denied) {                          // 4 Access denied: 3 quick red flashes
      uint32_t t = now - deniedAt;
      if ((t % 300) < 150) setRgb(255, 0, 0); else setRgb(0, 0, 0);
    } else if (danger) {                          // 5 Heat Danger: red heartbeat
      setRgb(hb ? 255 : 0, 0, 0);
      if (now - lastHeatBeep >= 2000) { lastHeatBeep = now; beepShort(); }
    } else if (sensorErr) {                       // 6 Sensor error: magenta
      if (hb) setRgb(255, 0, 255); else setRgb(0, 0, 0);
    } else if (camFault) {                        // 7 Camera fault: cyan
      if (hb) setRgb(0, 255, 255); else setRgb(0, 0, 0);
    } else if (weather) {                         // 8 Weather Danger: purple
      if (hb) setRgb(128, 0, 255); else setRgb(0, 0, 0);
    } else if (warning) {                         // 9 Warning: yellow
      if (hb) setRgb(255, 180, 0); else setRgb(0, 0, 0);
    } else if (offline) {                         // 10 Setup / offline: blue
      if (hb) setRgb(0, 0, 255); else setRgb(0, 0, 0);
    } else {                                      // 11 Safe: green
      if (hb) setRgb(0, 255, 0); else setRgb(0, 0, 0);
    }
    if (!danger) lastHeatBeep = now - 2000 + 50;  // first Danger beep comes at once

    if (gWeatherBeepReq) { gWeatherBeepReq = false; beepThree(); }

    // one-shot beeps run on top of the colour (not during fire/panic patterns)
    if (beeping && !buzz) {
      LOCK();
      if (beepLen) {
        bool on = (beepIdx % 2) == 0;
        buzz = on;
        if (now - beepStepAt >= beepSeq[beepIdx]) {
          beepStepAt = now; beepIdx++;
          if (beepIdx >= beepLen) beepLen = 0;
        }
      }
      UNLOCK();
    }
    digitalWrite(PIN_BUZZER, buzz ? HIGH : LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
