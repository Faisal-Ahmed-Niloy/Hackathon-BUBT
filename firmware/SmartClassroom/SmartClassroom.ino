// ============================================================================
//  FastRobox 1.0 - Smart Classroom - main file
//  Arduino IDE settings (Tools menu):
//    Board            : ESP32S3 Dev Module   (esp32 by Espressif, version 2.0.17)
//    USB CDC On Boot  : Disabled  (Serial = UART port, 115200 baud)
//    Flash Size       : 8MB
//    PSRAM            : OPI PSRAM
//    Partition Scheme : (ignored - partitions.csv in this folder is used)
//    CPU Frequency    : 240 MHz
// ============================================================================
#include "app.h"

DeviceState S;
SemaphoreHandle_t gLock, gFsLock, gSeqLock;
Preferences prefs;
QueueHandle_t qFaceReq;
volatile bool gResetFireReq = false;
volatile bool gWeatherBeepReq = false;
volatile bool gTelemetryNow = false;
String gRoomId, gToken, gServerUrl, gApPass;
String gResetReason = "POWER_ON";
uint32_t gBootCount = 0;
bool gTimeSynced = false;
time_t gSyncEpoch = 0;
uint32_t gSyncMillis = 0;
TaskInfo gTasks[12];
int gTaskCount = 0;

static void startTask(TaskFunction_t fn, const char* name, uint32_t stack, UBaseType_t prio, BaseType_t core) {
  TaskHandle_t h = nullptr;
  xTaskCreatePinnedToCore(fn, name, stack, nullptr, prio, &h, core);
  gTasks[gTaskCount++] = {name, h};
}

static String resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "POWER_ON";
    case ESP_RST_SW:       return "SOFTWARE";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:      return "WATCHDOG";
    case ESP_RST_PANIC:    return "CRASH";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "POWER_ON";   // EXT / USB / unknown -> treated as power-on
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  gLock = xSemaphoreCreateMutex();
  gFsLock = xSemaphoreCreateMutex();
  gSeqLock = xSemaphoreCreateMutex();
  qFaceReq = xQueueCreate(2, 24);

  gResetReason = resetReasonName(esp_reset_reason());

  // ---- persistent config (NVS) ----
  prefs.begin("sc", false);
  gBootCount = prefs.getULong("boots", 0) + 1;
  prefs.putULong("boots", gBootCount);
  loadConfig();          // t_net.ino   (Wi-Fi, room, token, server, AP password)
  loadSafetyConfig();    // t_safety.ino (limits, fire latch, teacher tags)
  seqInit();             // t_outbox.ino

  // ---- LittleFS: web files + offline outbox ----
  if (!LittleFS.begin(true)) Serial.println("[ERROR] LittleFS mount failed");
  installWebAssets();    // t_web.ino  (writes the status/setup pages into LittleFS)
  outboxInit();          // t_outbox.ino

  // ---- hardware ----
  ledBuzzerInit();       // t_led.ino
  sensorsInit();         // t_sensors.ino  (flame first -> door, touch baseline)

  // Level 1: every start-up prints the reset reason and queues a BOOT event.
  emitEvent("BOOT", String("reason=") + gResetReason);
  otaBootCheck();        // t_cloud.ino  (OTA confirm / rollback bookkeeping)

  // ---- task watchdog: any registered task that stops feeding -> reboot ----
  esp_task_wdt_init(WDT_TIMEOUT_S, true);

  // Core 1: real-time device work
  startTask(ledTask,     "led",     4096,  5, 1);
  startTask(sensorTask,  "sensors", 6144,  4, 1);
  startTask(safetyTask,  "safety",  8192,  4, 1);
  startTask(serialTask,  "serial",  8192,  2, 1);   // the "main" task frozen by `hang`
  // Core 0: connectivity and vision
  startTask(netTask,     "net",     8192,  3, 0);
  startTask(cloudTask,   "cloud",   12288, 2, 0);
  startTask(webTask,     "web",     6144,  1, 0);
  startTask(cameraTask,  "camera",  32768, 1, 0);
  startTask(csiTask,     "csi",     4096,  1, 0);
}

void loop() {
  // Everything runs in FreeRTOS tasks; the Arduino loop task is removed.
  vTaskDelete(nullptr);
}
