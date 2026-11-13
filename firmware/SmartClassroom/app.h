// ============================================================================
//  FastRobox 1.0 - Smart Classroom firmware (Team T15)
//  Board : ESP32-S3-WROOM CAM (Freenove-style, 8 MB flash, 8 MB OPI PSRAM)
//  Core  : arduino-esp32 2.0.17 (needed for esp-dl on-device face detection)
//  All shared types, pins, constants and globals live here. Every .ino tab in
//  this folder is concatenated after SmartClassroom.ino by the Arduino IDE, so
//  they all see these declarations.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <Update.h>
#include <SPI.h>
#include <MFRC522.h>          // Library Manager: "MFRC522" by GithubCommunity (miguelbalboa)
#include <DHT.h>              // Library Manager: "DHT sensor library" by Adafruit
#include <ArduinoJson.h>      // Library Manager: "ArduinoJson" by Benoit Blanchon (v7)
#include <BLEDevice.h>
#include <BLEAdvertising.h>
//#include "esp_camera.h"
//#include "img_converters.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"

// ----------------------------------------------------------------------------
// Identity (not secrets). Room ID, Wi-Fi and device token come from the setup
// page and live in NVS only.
// ----------------------------------------------------------------------------
#define TEAM_ID        "T15"
#define TEAM_ID_LC     "t15"
#define DEVICE_ID      "T15-D1"
#define FW_VERSION     "1.0.0"            // bump to 1.0.1 for the Level 5 OTA test
#define MDNS_NAME      "smartclass-" TEAM_ID_LC
#define AP_SSID        "SmartClass-" TEAM_ID

// Public HTTPS address of the platform. Pre-filled on the setup page and can be
// changed there. It is an address, not a secret.
#define DEFAULT_SERVER_URL "https://smartclass-t15.example.com"

// Level 5 rollback test: uncomment to build the *faulty* firmware that installs
// fine but can never reach the server.
// #define FAULT_TEST_SERVER_URL "https://wrong-server.invalid"

// ----------------------------------------------------------------------------
// Pins (verified against the board pinout; camera uses 4-13,15-18; PSRAM 35-37)
// ----------------------------------------------------------------------------
#define PIN_RFID_SS    21
#define PIN_RFID_SCK   47
#define PIN_RFID_MOSI  2
#define PIN_RFID_MISO  14
#define PIN_RFID_RST   42
#define PIN_DHT        1
#define PIN_FLAME      38   // digital DO
#define PIN_BUZZER     41   // active buzzer (+)
#define PIN_RGB        48   // onboard WS2812
#define PIN_BOOT_BTN   0
#define PIN_TOUCH      3    // T3 - bare jumper wire = panic pad

#define FLAME_ACTIVE_LOW 1  // most IR flame modules pull DO LOW when they see a flame

// Camera (Freenove ESP32-S3 WROOM CAM / OV2640)
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD  4
#define CAM_SIOC  5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4    8
#define CAM_Y3    9
#define CAM_Y2    11
#define CAM_VSYNC 6
#define CAM_HREF  7
#define CAM_PCLK  13

// ----------------------------------------------------------------------------
// Timing / limits (printed spec values)
// ----------------------------------------------------------------------------
#define DHT_PERIOD_MS        5000
#define FLAME_PERIOD_MS      100
#define RFID_PERIOD_MS       200
#define TELEMETRY_PERIOD_MS  5000
#define STATUS_LINE_MS       1000
#define WDT_TIMEOUT_S        8       // restart within 10 s
#define STATUS_HOLD_MS       3000
#define FIRE_HOLD_MS         2000
#define PANIC_TOUCH_MS       2000
#define PANIC_DURATION_MS    10000
#define GRANT_MS             5000
#define FACE_TIMEOUT_MS      3000
#define WIFI_SETUP_TIMEOUT   12000   // < 15 s back in setup mode
#define OFFLINE_AP_AFTER_MS  30000
#define SERVER_LIVE_MS       15000   // no server success for 15 s -> OFFLINE
#define OTA_CONFIRM_MS       60000
#define DEFAULT_WARN_C       32.0f
#define DEFAULT_DANGER_C     40.0f
#define HYST_C               2.0f

// ----------------------------------------------------------------------------
// Types
// ----------------------------------------------------------------------------
enum HeatStatus : uint8_t { ST_SAFE = 0, ST_WARNING, ST_DANGER, ST_FIRE };
enum NetState   : uint8_t { NET_SETUP = 0, NET_CONNECTING, NET_LIVE, NET_OFFLINE, NET_SYNCING };
enum Trend      : int8_t  { TR_FALLING = -1, TR_STABLE = 0, TR_RISING = 1 };

struct DeviceState {
  // sensors
  float tempC = NAN, rh = NAN, hi = NAN;
  bool  dhtOk = false;          // sensor answering
  bool  qualityGood = false;    // last reading trustworthy
  bool  rfidOk = true;
  bool  flame = false;
  // safety
  bool  doorUnlocked = false;
  HeatStatus status = ST_SAFE;  // committed overall status
  Trend trend = TR_STABLE;
  bool  fireLatched = false;
  bool  fireSilenced = false;
  uint32_t cannotSilenceUntil = 0;
  uint32_t panicUntil = 0;
  uint32_t grantedUntil = 0;
  uint32_t deniedAt = 0;
  float warnC = DEFAULT_WARN_C, dangerC = DEFAULT_DANGER_C;
  // cloud-fed
  bool  weatherDanger = false;
  char  weatherText[160] = "";
  char  weatherLine[96] = "";
  char  lastUid[24] = "-";
  char  checkName[40] = "-";
  char  checkResult[16] = "-";
  char  checkReason[32] = "-";
  time_t checkTs = 0;
  char  camPosition[8] = "DOOR";
  char  camFault[16] = "OK";
  bool  camInitOk = false;
  bool  nightMode = false;
  int   people = -1;
  bool  overCapacity = false;
  bool  intruder = false;
  char  presence[12] = "-";
  char  zone[8] = "-";
  char  handSeat[8] = "-";
  time_t handTs = 0;
  // network
  NetState net = NET_SETUP;
  uint32_t pending = 0;
};

// ----------------------------------------------------------------------------
// Globals (defined in SmartClassroom.ino)
// ----------------------------------------------------------------------------
extern DeviceState S;
extern SemaphoreHandle_t gLock;      // protects S
extern SemaphoreHandle_t gFsLock;    // protects LittleFS / outbox
extern SemaphoreHandle_t gSeqLock;
extern Preferences prefs;
extern QueueHandle_t qFaceReq;       // card UIDs that need a face check
extern volatile bool gResetFireReq;
extern volatile bool gWeatherBeepReq;
extern volatile bool gTelemetryNow;
extern String gRoomId, gToken, gServerUrl, gApPass;
extern String gResetReason;
extern uint32_t gBootCount;
extern bool gTimeSynced;
extern time_t gSyncEpoch;
extern uint32_t gSyncMillis;

#define LOCK()   xSemaphoreTake(gLock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(gLock)

// Task registry for the `tasks` command
struct TaskInfo { const char* name; TaskHandle_t h; };
extern TaskInfo gTasks[12];
extern int gTaskCount;

#include "ca_cert.h"          // ROOT_CA (generated by tools/make_ca_header.py)

// ----------------------------------------------------------------------------
// Cross-tab globals and functions (explicit, so tab order never matters)
// ----------------------------------------------------------------------------
extern bool gStartupFlame;
extern uint32_t touchBaseline, touchThreshold;
extern volatile uint32_t touchRaw;
extern volatile bool touchActive;
extern uint32_t lastServerOk, wifiDownSince;
extern String setupState, setupMsg;

// t_led
void ledBuzzerInit(); void ledTask(void*); void beepShort(); void beepTwo(); void beepThree();
// t_sensors
void sensorsInit(); void sensorTask(void*); bool readFlameRaw();
// t_safety
void loadSafetyConfig(); void safetyTask(void*); void onDhtReading(float t, float h);
bool isTeacherTag(const char* uid); void setTeacherTags(const String& csv); String getTeacherTags();
void teacherSilence(); void applySettings(float warn, float danger);
const char* statusName(HeatStatus s); const char* trendName(Trend t);
// t_access
void handleCardTap(const char* uid); void loadOfflineCards(); void setOfflineCards(const String& csv, uint32_t classEnd);
void setCheckResult(const char* name, const char* result, const char* reason);
void grantEntry(const char* name, const char* how); void denyEntry(const char* reason); void faceCheckDone();
// t_outbox
void seqInit(); uint32_t nextSeq(); void outboxInit(); void outboxAppend(const String& json);
int outboxPeek(String& out, int maxRec, uint32_t& endSeg, uint32_t& endOff);
void outboxAck(uint32_t endSeg, uint32_t endOff, int n);
void emitEvent(const char* type, const String& detail); void queueTelemetry(); time_t tsNowOrZero();
// t_net
void loadConfig(); void netTask(void*); void clearWifiAndRestart();
void requestSetupTrial(const String& ssid, const String& pass, const String& room, const String& token, const String& server);
// t_cloud
void otaBootCheck(); void otaRollback(const char* why); void cloudTask(void*); void handleServerJson(const String& body);
int httpPost(const String& path, const String& body, String& resp);
// t_web
void installWebAssets(); void webTask(void*);
// t_camera
void cameraTask(void*); void setCameraFault(const char* f);
// t_csi
void csiTask(void*); void csiPrint(); bool csiTakeFeatures(String& out);
// t_serial
void serialTask(void*); void printStatusLine(); bool registerNextTag();
