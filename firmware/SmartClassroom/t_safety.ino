// ============================================================================
//  Safety logic (core 1, 100 ms tick): heat status with 3 s hold + hysteresis,
//  heat index, trend, data quality, fire latch, Teacher-tag silence, touch
//  panic, fail-safe door, and the 5 s telemetry snapshot.
// ============================================================================
static float tempHist[10];
static int   tempHistN = 0;
static HeatStatus candidate = ST_SAFE;
static uint32_t candidateSince = 0;
static String teacherTags;          // comma separated UIDs, kept in NVS
extern uint32_t touchBaseline, touchThreshold;
extern volatile bool touchActive;

void loadSafetyConfig() {
  S.warnC = prefs.getFloat("warnC", DEFAULT_WARN_C);
  S.dangerC = prefs.getFloat("dangerC", DEFAULT_DANGER_C);
  S.fireLatched = prefs.getBool("fire", false);       // Level 3/12: survives power cut
  S.fireSilenced = prefs.getBool("fireSil", false);
  teacherTags = prefs.getString("tTags", "");
  if (S.fireLatched) { S.doorUnlocked = true; S.status = ST_FIRE; }
}

bool isTeacherTag(const char* uid) {
  String key = String(",") + teacherTags + ",";
  return key.indexOf(String(",") + uid + ",") >= 0;
}
void setTeacherTags(const String& csv) {
  teacherTags = csv;
  prefs.putString("tTags", teacherTags);
}
String getTeacherTags() { return teacherTags; }

// NOAA heat index (Rothfusz regression with Steadman fallback), in deg C
float heatIndexC(float tC, float rh) {
  float tF = tC * 9.0f / 5.0f + 32.0f;
  float hi = 0.5f * (tF + 61.0f + ((tF - 68.0f) * 1.2f) + (rh * 0.094f));
  if ((hi + tF) / 2.0f >= 80.0f) {
    hi = -42.379f + 2.04901523f * tF + 10.14333127f * rh - 0.22475541f * tF * rh
         - 0.00683783f * tF * tF - 0.05481717f * rh * rh + 0.00122874f * tF * tF * rh
         + 0.00085282f * tF * rh * rh - 0.00000199f * tF * tF * rh * rh;
    if (rh < 13 && tF >= 80 && tF <= 112) hi -= ((13 - rh) / 4) * sqrtf((17 - fabsf(tF - 95)) / 17);
    else if (rh > 85 && tF >= 80 && tF <= 87) hi += ((rh - 85) / 10) * ((87 - tF) / 5);
  }
  return (hi - 32.0f) * 5.0f / 9.0f;
}

static Trend computeTrend() {
  if (tempHistN < 4) return TR_STABLE;
  // least-squares slope over the last (up to) 10 readings, deg C per reading
  int n = tempHistN;
  float sx = 0, sy = 0, sxy = 0, sxx = 0;
  for (int i = 0; i < n; i++) { sx += i; sy += tempHist[i]; sxy += i * tempHist[i]; sxx += i * i; }
  float slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
  if (slope > 0.05f) return TR_RISING;     // > 0.6 C per minute
  if (slope < -0.05f) return TR_FALLING;
  return TR_STABLE;
}

// Called from the sensor task with a fresh DHT11 reading
void onDhtReading(float t, float h) {
  bool plausible = t >= 0 && t <= 50 && h >= 20 && h <= 90;
  LOCK();
  if (!plausible) {
    S.qualityGood = false;      // impossible value: flagged bad, never used, status kept
    UNLOCK();
    return;
  }
  S.tempC = t; S.rh = h; S.hi = heatIndexC(t, h);
  S.qualityGood = true;
  if (tempHistN < 10) tempHist[tempHistN++] = t;
  else { memmove(tempHist, tempHist + 1, 9 * sizeof(float)); tempHist[9] = t; }
  S.trend = computeTrend();      // display only - never touches light/buzzer/door
  UNLOCK();
}

// Hysteresis rules. Danger only steps down to Warning (never straight to Safe).
static HeatStatus nextHeatStatus(HeatStatus cur, float t, float h, float warn, float danger) {
  switch (cur) {
    case ST_DANGER:
      return (t <= danger - HYST_C) ? ST_WARNING : ST_DANGER;
    case ST_WARNING:
      if (t >= danger) return ST_DANGER;
      if (t <= warn - HYST_C && h <= 80.0f) return ST_SAFE;
      return ST_WARNING;
    default:
      if (t >= danger) return ST_DANGER;
      if (t >= warn || h >= 85.0f) return ST_WARNING;
      return ST_SAFE;
  }
}

const char* statusName(HeatStatus s) {
  switch (s) { case ST_WARNING: return "WARNING"; case ST_DANGER: return "DANGER"; case ST_FIRE: return "FIRE"; default: return "SAFE"; }
}
const char* trendName(Trend t) { return t == TR_RISING ? "RISING" : t == TR_FALLING ? "FALLING" : "STABLE"; }

void setFireLatch(bool on) {
  prefs.putBool("fire", on);
  prefs.putBool("fireSil", false);
}

// Teacher tag tapped (from t_access)
void teacherSilence() {
  LOCK();
  bool fire = S.fireLatched, flame = S.flame, sil = S.fireSilenced;
  if (fire && flame) S.cannotSilenceUntil = millis() + 5000;
  UNLOCK();
  if (!fire) return;
  if (flame) {
    Serial.println("[EVENT] SILENCE_REFUSED CANNOT SILENCE - FLAME PRESENT");
    return;
  }
  if (!sil) {
    LOCK(); S.fireSilenced = true; UNLOCK();
    prefs.putBool("fireSil", true);
    emitEvent("SILENCED", "teacher tag");
  }
}

void applySettings(float warn, float danger) {
  if (!(warn >= 0 && warn <= 50 && danger >= 0 && danger <= 50 && warn < danger)) {
    Serial.printf("[ERROR] settings refused warn=%.1f danger=%.1f\n", warn, danger);
    return;
  }
  LOCK(); S.warnC = warn; S.dangerC = danger; UNLOCK();
  prefs.putFloat("warnC", warn);
  prefs.putFloat("dangerC", danger);
  char d[48]; snprintf(d, sizeof(d), "warn=%.1f danger=%.1f", warn, danger);
  emitEvent("SETTINGS_APPLIED", d);
}

void safetyTask(void*) {
  esp_task_wdt_add(nullptr);
  uint32_t flameSince = 0, flameGoneSince = 0, touchSince = 0, lastTelemetry = millis();
  bool panicFired = false, flameReported = false;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();

    LOCK();
    bool flame = S.flame;
    bool fire = S.fireLatched;
    float t = S.tempC, h = S.rh, warn = S.warnC, danger = S.dangerC;
    bool q = S.qualityGood;
    HeatStatus cur = S.status == ST_FIRE ? ST_SAFE : S.status;
    UNLOCK();

    // ---------- FIRE: flame continuously for 2 s -> FIRE at once ----------
    if (flame) {
      flameGoneSince = 0;
      if (!flameSince) flameSince = now;
      if (!fire && now - flameSince >= FIRE_HOLD_MS) {
        LOCK();
        S.fireLatched = true; S.fireSilenced = false; S.doorUnlocked = true;
        HeatStatus old = S.status; S.status = ST_FIRE;
        UNLOCK();
        setFireLatch(true);
        emitEvent("FIRE", "flame 2s");
        emitEvent("STATUS", String(statusName(old)) + "->FIRE");
        flameReported = true;
        fire = true;
      }
    } else {
      flameSince = 0;
      if (fire && flameReported) {
        if (!flameGoneSince) flameGoneSince = now;
        if (now - flameGoneSince >= 2000) { emitEvent("FIRE_CLEARED", "flame gone"); flameReported = false; }
      }
    }
    if (fire && !flameReported && flame) flameReported = true;   // flame back after clear / after reboot

    // ---------- Reset fire (Teacher/Admin from the dashboard) ----------
    if (gResetFireReq) {
      gResetFireReq = false;
      if (fire && !flame) {
        LOCK();
        S.fireLatched = false; S.fireSilenced = false; S.doorUnlocked = false; S.status = ST_SAFE;
        UNLOCK();
        setFireLatch(false);
        candidate = ST_SAFE; candidateSince = now;
        emitEvent("STATUS", "FIRE->SAFE");
        Serial.println("[EVENT] FIRE_RESET door=LOCKED");
        fire = false;
      } else if (fire && flame) {
        Serial.println("[ERROR] Reset fire refused: flame present");
      }
    }

    // ---------- Heat status (3 s hold; bad data keeps the last status) ----------
    if (!fire && q && !isnan(t)) {
      HeatStatus want = nextHeatStatus(cur, t, h, warn, danger);
      if (want != candidate) { candidate = want; candidateSince = now; }
      if (want != cur && now - candidateSince >= STATUS_HOLD_MS) {
        LOCK(); S.status = want; UNLOCK();
        emitEvent("STATUS", String(statusName(cur)) + "->" + statusName(want));
      }
    }

    // ---------- Touch panic: >= 2 s -> PANIC; < 1 s ignored ----------
    if (touchActive) {
      if (!touchSince) touchSince = now;
      if (!panicFired && now - touchSince >= PANIC_TOUCH_MS) {
        panicFired = true;
        LOCK(); S.panicUntil = now + PANIC_DURATION_MS; UNLOCK();
        emitEvent("PANIC", "touch pad");
      }
    } else { touchSince = 0; panicFired = false; }

    // ---------- Door (fail-safe): fire overrides every other rule ----------
    LOCK();
    if (S.fireLatched) S.doorUnlocked = true;
    // start-up flame keeps the door open until the flame is gone (or FIRE latches)
    else { if (gStartupFlame && !S.flame) gStartupFlame = false;
           S.doorUnlocked = (now < S.grantedUntil) || gStartupFlame; }
    UNLOCK();

    // ---------- Telemetry every 5 s ----------
    if (now - lastTelemetry >= TELEMETRY_PERIOD_MS || gTelemetryNow) {
      lastTelemetry = now; gTelemetryNow = false;
      queueTelemetry();     // t_outbox.ino
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
