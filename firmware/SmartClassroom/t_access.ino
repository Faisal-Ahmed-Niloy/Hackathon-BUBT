// ============================================================================
//  Card handling. Called from the sensor task once per tap.
//    Teacher tag  -> silence (only after the flame is gone); never opens door
//    Fire latched -> door already open; tap is only logged
//    OFFLINE      -> card-only entry for the cached class list (OFFLINE_ENTRY)
//    Camera fault / not at DOOR -> denied by the device
//    otherwise    -> face check request to the camera task
// ============================================================================
static String offlineCards;           // comma separated UIDs of the class running now
static uint32_t offlineClassEnd = 0;  // unix time when that class ends
static volatile bool checkBusy = false;

void loadOfflineCards() {
  offlineCards = prefs.getString("offCards", "");
  offlineClassEnd = prefs.getULong("offEnd", 0);
}
void setOfflineCards(const String& csv, uint32_t classEnd) {
  if (csv == offlineCards && classEnd == offlineClassEnd) return;
  offlineCards = csv; offlineClassEnd = classEnd;
  prefs.putString("offCards", csv);
  prefs.putULong("offEnd", classEnd);
}
static bool inOfflineList(const char* uid) {
  if (offlineClassEnd && gTimeSynced && (uint32_t)time(nullptr) > offlineClassEnd) return false;
  String key = String(",") + offlineCards + ",";
  return key.indexOf(String(",") + uid + ",") >= 0;
}

void setCheckResult(const char* name, const char* result, const char* reason) {
  LOCK();
  strncpy(S.checkName, name, sizeof(S.checkName) - 1);
  strncpy(S.checkResult, result, sizeof(S.checkResult) - 1);
  strncpy(S.checkReason, reason, sizeof(S.checkReason) - 1);
  S.checkTs = time(nullptr);
  UNLOCK();
}

void grantEntry(const char* name, const char* how) {
  LOCK();
  if (!S.fireLatched) { S.grantedUntil = millis() + GRANT_MS; S.doorUnlocked = true; }
  UNLOCK();
  beepShort();
  setCheckResult(name, "GRANTED", how);
}

void denyEntry(const char* reason) {
  LOCK(); S.deniedAt = millis(); UNLOCK();
  beepTwo();
  setCheckResult("-", "DENIED", reason);
  emitEvent("ENTRY_DENIED", reason);
}

void handleCardTap(const char* uid) {
  Serial.printf("[CARD] uid=%s\n", uid);
  LOCK();
  strncpy(S.lastUid, uid, sizeof(S.lastUid) - 1);
  bool fire = S.fireLatched;
  NetState net = S.net;
  bool camFault = strcmp(S.camFault, "OK") != 0;
  bool atDoor = strcmp(S.camPosition, "DOOR") == 0;
  bool camOk = S.camInitOk;
  UNLOCK();
  emitEvent("CARD_TAP", String("uid=") + uid);

  if (isTeacherTag(uid)) { teacherSilence(); return; }   // never opens the door
  if (fire) return;                                       // fire rule overrides all
  if (registerNextTag()) return;                          // `tag add` from the Serial Monitor

  bool online = (net == NET_LIVE || net == NET_SYNCING);
  if (!online) {
    if (inOfflineList(uid)) {
      grantEntry(uid, "OFFLINE");
      emitEvent("OFFLINE_ENTRY", String("uid=") + uid + " face_not_verified");
    } else {
      denyEntry("OFFLINE_NOT_ALLOWED");
    }
    return;
  }
  if (camFault || !camOk) { denyEntry("CAMERA_FAULT"); return; }
  if (!atDoor) { denyEntry("CAMERA_NOT_AT_DOOR"); return; }
  if (checkBusy) return;
  char buf[24]; strncpy(buf, uid, sizeof(buf));
  if (xQueueSend(qFaceReq, buf, 0) == pdTRUE) checkBusy = true;
}

void faceCheckDone() { checkBusy = false; }
