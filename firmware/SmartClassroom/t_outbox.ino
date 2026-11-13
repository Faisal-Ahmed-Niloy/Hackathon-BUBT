// ============================================================================
//  seq + persistent outbox.
//  - seq only ever increases, even across restarts: a ceiling is written to NVS
//    in blocks of 100 before those numbers are used.
//  - Every telemetry reading and event is appended to LittleFS segment files
//    (/q/<n>.q), one record per line:  <crc32 hex>|<json>\n
//    A line with a missing newline or a bad CRC (half-written by a power cut)
//    is discarded. The read pointer (segment, offset) lives in NVS, so records
//    are sent oldest first, each exactly once, after any restart.
//  - 24 segments x 32 KB = 768 KB  (~2.5 h of telemetry, spec needs 30 min).
// ============================================================================
#define SEG_MAX_BYTES 32768
#define SEG_MAX_COUNT 24

static uint32_t seqNext = 1, seqCeil = 0;
static uint32_t segW = 0, segR = 0, offR = 0;

void seqInit() {
  seqNext = prefs.getULong("seqCeil", 1);
  seqCeil = seqNext + 100;
  prefs.putULong("seqCeil", seqCeil);
}

uint32_t nextSeq() {
  xSemaphoreTake(gSeqLock, portMAX_DELAY);
  uint32_t s = seqNext++;
  if (seqNext >= seqCeil) { seqCeil += 100; prefs.putULong("seqCeil", seqCeil); }
  xSemaphoreGive(gSeqLock);
  return s;
}

static uint32_t crc32Str(const char* s, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint8_t)s[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (-(int32_t)(c & 1)));
  }
  return ~c;
}

static String segPath(uint32_t n) { return String("/q/") + n + ".q"; }

static uint32_t countLines(uint32_t seg, uint32_t fromOff) {
  File f = LittleFS.open(segPath(seg), "r");
  if (!f) return 0;
  f.seek(fromOff);
  uint32_t n = 0;
  while (f.available()) if (f.read() == '\n') n++;
  f.close();
  return n;
}

void outboxInit() {
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  if (!LittleFS.exists("/q")) LittleFS.mkdir("/q");
  segR = prefs.getULong("segR", 1);
  offR = prefs.getULong("offR", 0);
  segW = prefs.getULong("segW", 1);
  if (segW < segR) segW = segR;
  // Always start a fresh segment at boot: a half-written tail of the old one
  // can never be glued onto a new record.
  if (LittleFS.exists(segPath(segW))) segW++;
  prefs.putULong("segW", segW);
  uint32_t pending = 0;
  for (uint32_t s = segR; s < segW; s++) pending += countLines(s, s == segR ? offR : 0);
  xSemaphoreGive(gFsLock);
  LOCK(); S.pending = pending; UNLOCK();
  Serial.printf("[NET] outbox ready, %lu records waiting\n", (unsigned long)pending);
}

// Fill in ts for records taken before NTP sync (same boot) using the uptime.
time_t tsNowOrZero() { return gTimeSynced ? time(nullptr) : 0; }

void outboxAppend(const String& json) {
  char head[12];
  snprintf(head, sizeof(head), "%08lx|", (unsigned long)crc32Str(json.c_str(), json.length()));
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  File f = LittleFS.open(segPath(segW), "a");
  if (f) {
    f.print(head); f.print(json); f.print('\n');
    size_t sz = f.size();
    f.close();
    if (sz >= SEG_MAX_BYTES) { segW++; prefs.putULong("segW", segW); }
  }
  // overflow: drop the oldest segment
  if (segW - segR >= SEG_MAX_COUNT) {
    uint32_t dropped = countLines(segR, offR);
    LittleFS.remove(segPath(segR));
    segR++; offR = 0;
    prefs.putULong("segR", segR); prefs.putULong("offR", 0);
    LOCK(); S.pending = S.pending > dropped ? S.pending - dropped : 0; UNLOCK();
  }
  xSemaphoreGive(gFsLock);
  LOCK(); S.pending++; UNLOCK();
}

// Reads up to maxRec valid records starting at the read pointer.
// Returns the JSON array text; endSeg/endOff = where the pointer goes after ack.
int outboxPeek(String& out, int maxRec, uint32_t& endSeg, uint32_t& endOff) {
  int n = 0;
  out = "[";
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  uint32_t s = segR, o = offR;
  // The writer appends whole lines under gFsLock, so a line without '
'
  // can only be the torn tail left by a power cut -> discarded.
  while (n < maxRec && out.length() < 7000 && s <= segW) {
    File f = LittleFS.open(segPath(s), "r");
    if (!f) { if (s < segW) { s++; o = 0; continue; } break; }
    f.seek(o);
    String line;
    while (n < maxRec && out.length() < 7000) {
      int c = f.read();
      if (c < 0) {                         // EOF
        if (line.length()) o = f.size();   // torn line: skip it
        break;
      }
      if (c != '
') { line += (char)c; continue; }
      o = f.position();
      if (line.length() > 9 && line[8] == '|') {
        String js = line.substring(9);
        uint32_t crc = strtoul(line.substring(0, 8).c_str(), nullptr, 16);
        if (crc == crc32Str(js.c_str(), js.length())) {   // bad CRC -> discarded
          if (n) out += ",";
          out += js; n++;
        }
      }
      line = "";
    }
    bool atEnd = o >= f.size();
    f.close();
    if (atEnd && s < segW) { s++; o = 0; continue; }
    break;
  }
  xSemaphoreGive(gFsLock);
  out += "]";
  endSeg = s; endOff = o;
  return n;
}

void outboxAck(uint32_t endSeg, uint32_t endOff, int n) {
  xSemaphoreTake(gFsLock, portMAX_DELAY);
  for (uint32_t s = segR; s < endSeg; s++) LittleFS.remove(segPath(s));
  segR = endSeg; offR = endOff;
  prefs.putULong("segR", segR); prefs.putULong("offR", offR);
  xSemaphoreGive(gFsLock);
  LOCK(); S.pending = S.pending > (uint32_t)n ? S.pending - n : 0; UNLOCK();
}

static void addCommon(JsonDocument& d, const char* kind, uint32_t seq) {
  d["kind"] = kind;
  d["device_id"] = DEVICE_ID;
  d["room_id"] = gRoomId;
  d["seq"] = seq;
  time_t ts = tsNowOrZero();
  d["ts"] = (uint32_t)ts;
  d["boot"] = gBootCount;
  d["up_ms"] = millis();
}

// Every event: printed as [EVENT] <type> <detail> and queued for the cloud.
void emitEvent(const char* type, const String& detail) {
  Serial.printf("[EVENT] %s %s\n", type, detail.c_str());
  JsonDocument d;
  addCommon(d, "event", nextSeq());
  d["type"] = type;
  d["detail"] = detail;
  String js; serializeJson(d, js);
  outboxAppend(js);
}

void queueTelemetry() {
  JsonDocument d;
  addCommon(d, "telemetry", nextSeq());
  LOCK();
  d["uptime_s"] = millis() / 1000;
  if (!isnan(S.tempC)) { d["temp_c"] = roundf(S.tempC * 10) / 10.0; d["rh"] = roundf(S.rh); d["heat_index_c"] = roundf(S.hi * 10) / 10.0; }
  else { d["temp_c"] = nullptr; d["rh"] = nullptr; d["heat_index_c"] = nullptr; }
  d["flame"] = S.flame;
  d["door"] = S.doorUnlocked ? "UNLOCKED" : "LOCKED";
  d["status"] = statusName(S.status);
  d["heat_trend"] = trendName(S.trend);
  d["quality"] = (S.qualityGood && S.dhtOk && S.rfidOk) ? "good" : "bad";
  d["warn_c"] = S.warnC; d["danger_c"] = S.dangerC;
  d["fire_silenced"] = S.fireSilenced;
  d["pending"] = S.pending;
  UNLOCK();
  d["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  d["heap_kb"] = ESP.getFreeHeap() / 1024;
  d["psram_kb"] = ESP.getFreePsram() / 1024;
  d["chip_temp_c"] = roundf(temperatureRead() * 10) / 10.0;
  d["fw"] = FW_VERSION;
  d["reset_reason"] = gResetReason;
  String js; serializeJson(d, js);
  outboxAppend(js);
}
