// // ============================================================================
// //  Camera task (core 0).
// //   - Streams VGA JPEG frames (~3 fps) to the platform for the live view,
// //     people counting, seat zones, hand-raise, Night watch and tamper checks.
// //     The server never stores these frames.
// //   - On a card tap (queue from t_access): ON-DEVICE face detection with esp-dl
// //     (MSR01 + MNP01 two-stage detector). Only the cropped face + card UID is
// //     sent to the server. No face within 3 s -> NO FACE, decided on the device.
// // ============================================================================
// #include "human_face_detect_msr01.hpp"
// #include "human_face_detect_mnp01.hpp"

// static WiFiClientSecure camTls;
// static HTTPClient camHttp;

// void setCameraFault(const char* f) {
//   LOCK();
//   bool changed = strcmp(S.camFault, f) != 0;
//   if (changed) strncpy(S.camFault, f, sizeof(S.camFault) - 1);
//   UNLOCK();
//   if (!changed) return;
//   if (strcmp(f, "OK") == 0) Serial.println("[EVENT] CAMERA_OK");
//   else Serial.printf("[EVENT] CAMERA_FAULT %s\n", f);
// }

// static bool cameraInit() {
//   camera_config_t c = {};
//   c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
//   c.pin_d0 = CAM_Y2; c.pin_d1 = CAM_Y3; c.pin_d2 = CAM_Y4; c.pin_d3 = CAM_Y5;
//   c.pin_d4 = CAM_Y6; c.pin_d5 = CAM_Y7; c.pin_d6 = CAM_Y8; c.pin_d7 = CAM_Y9;
//   c.pin_xclk = CAM_XCLK; c.pin_pclk = CAM_PCLK; c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF;
//   c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC; c.pin_pwdn = CAM_PWDN; c.pin_reset = CAM_RESET;
//   c.xclk_freq_hz = 20000000;
//   c.pixel_format = PIXFORMAT_JPEG;
//   c.frame_size = FRAMESIZE_VGA;
//   c.jpeg_quality = 14;
//   c.fb_count = 2;
//   c.fb_location = CAMERA_FB_IN_PSRAM;
//   c.grab_mode = CAMERA_GRAB_LATEST;
//   esp_err_t e = esp_camera_init(&c);
//   if (e != ESP_OK) { Serial.printf("[CAM] init failed 0x%x\n", e); return false; }
//   sensor_t* s = esp_camera_sensor_get();
//   if (s) { s->set_vflip(s, 1); s->set_hmirror(s, 0); }
//   Serial.println("[CAM] camera ready (VGA JPEG)");
//   return true;
// }

// static int camPost(const String& path, const uint8_t* data, size_t len, const String& uid, int w, int h, String& resp) {
//   if (!WiFi.isConnected() || strlen(ROOT_CA) < 100) return -1;
//   camHttp.setReuse(true);
//   camHttp.setTimeout(6000);
//   if (!camHttp.begin(camTls, gServerUrl + path)) return -3;
//   camHttp.addHeader("Content-Type", "image/jpeg");
//   camHttp.addHeader("Authorization", "Bearer " + gToken);
//   camHttp.addHeader("X-Device", DEVICE_ID);
//   if (uid.length()) { camHttp.addHeader("X-UID", uid); camHttp.addHeader("X-Width", String(w)); camHttp.addHeader("X-Height", String(h)); }
//   int code = camHttp.POST((uint8_t*)data, len);
//   resp = code > 0 ? camHttp.getString() : "";
//   camHttp.end();
//   return code;
// }

// // Detect a face in one frame. On success returns a JPEG of the cropped face.
// static bool detectAndCrop(camera_fb_t* fb, uint8_t** jpg, size_t* jpgLen, int* cw, int* ch) {
//   int W = fb->width, H = fb->height;
//   uint8_t* rgb = (uint8_t*)ps_malloc(W * H * 3);
//   if (!rgb) return false;
//   bool ok = false;
//   if (fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
//     HumanFaceDetectMSR01 s1(0.1F, 0.5F, 10, 0.2F);
//     HumanFaceDetectMNP01 s2(0.5F, 0.3F, 5);
//     std::list<dl::detect::result_t>& cand = s1.infer((uint8_t*)rgb, {H, W, 3});
//     std::list<dl::detect::result_t>& res = s2.infer((uint8_t*)rgb, {H, W, 3}, cand);
//     if (!res.empty()) {
//       // largest face
//       auto best = res.begin();
//       int bestArea = 0;
//       for (auto it = res.begin(); it != res.end(); ++it) {
//         int a = (it->box[2] - it->box[0]) * (it->box[3] - it->box[1]);
//         if (a > bestArea) { bestArea = a; best = it; }
//       }
//       int x1 = best->box[0], y1 = best->box[1], x2 = best->box[2], y2 = best->box[3];
//       int bw = x2 - x1, bh = y2 - y1;
//       x1 = max(0, x1 - bw / 4); y1 = max(0, y1 - bh / 3); x2 = min(W - 1, x2 + bw / 4); y2 = min(H - 1, y2 + bh / 5);
//       int w = x2 - x1, h = y2 - y1;
//       if (w >= 40 && h >= 40) {
//         uint8_t* crop = (uint8_t*)ps_malloc(w * h * 3);
//         if (crop) {
//           for (int y = 0; y < h; y++) memcpy(crop + y * w * 3, rgb + ((y1 + y) * W + x1) * 3, w * 3);
//           ok = fmt2jpg(crop, w * h * 3, w, h, PIXFORMAT_RGB888, 90, jpg, jpgLen);
//           free(crop);
//           *cw = w; *ch = h;
//         }
//       }
//     }
//   }
//   free(rgb);
//   return ok;
// }

// static void runFaceCheck(const char* uid) {
//   uint32_t start = millis();
//   uint8_t* jpg = nullptr; size_t jpgLen = 0; int w = 0, h = 0;
//   bool found = false;
//   while (millis() - start < FACE_TIMEOUT_MS && !found) {
//     esp_task_wdt_reset();
//     camera_fb_t* fb = esp_camera_fb_get();
//     if (!fb) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
//     found = detectAndCrop(fb, &jpg, &jpgLen, &w, &h);
//     esp_camera_fb_return(fb);
//   }
//   if (!found) {
//     // decided by the device itself, the server is not asked
//     LOCK(); S.deniedAt = millis(); UNLOCK();
//     beepTwo();
//     setCheckResult("-", "DENIED", "NO FACE");
//     emitEvent("NO_FACE", String("uid=") + uid);
//     return;
//   }
//   Serial.printf("[CAM] FACE w=%d h=%d (%u bytes)\n", w, h, (unsigned)jpgLen);
//   String resp;
//   int code = camPost("/api/device/checkin", jpg, jpgLen, uid, w, h, resp);
//   free(jpg);
//   if (code != 200) { denyEntry("SERVER_UNREACHABLE"); return; }
//   JsonDocument d;
//   if (deserializeJson(d, resp)) { denyEntry("BAD_REPLY"); return; }
//   String result = d["result"] | "DENIED";
//   String name = d["name"] | "-";
//   String reason = d["reason"] | "";
//   if (result == "GRANTED") {
//     grantEntry(name.c_str(), (d["detail"] | "FACE OK"));
//     emitEvent("ENTRY_GRANTED", name);
//   } else if (result == "REGISTERED") {
//     beepShort();
//     setCheckResult(name.c_str(), "REGISTERED", "card linked");
//   } else {
//     denyEntry(reason.c_str());
//   }
// }

// void cameraTask(void*) {
//   esp_task_wdt_add(nullptr);
//   bool ok = cameraInit();
//   LOCK(); S.camInitOk = ok; UNLOCK();
//   if (!ok) setCameraFault("NO_CAMERA");
//   camTls.setCACert(ROOT_CA);
//   camTls.setTimeout(6);
//   uint32_t lastFrame = 0;
//   for (;;) {
//     esp_task_wdt_reset();
//     char uid[24];
//     if (xQueueReceive(qFaceReq, uid, 0) == pdTRUE) {
//       if (ok) runFaceCheck(uid); else denyEntry("CAMERA_FAULT");
//       faceCheckDone();
//       continue;
//     }
//     LOCK(); NetState ns = S.net; UNLOCK();
//     bool online = ns == NET_LIVE || ns == NET_SYNCING;
//     if (ok && online && millis() - lastFrame >= 300) {
//       lastFrame = millis();
//       camera_fb_t* fb = esp_camera_fb_get();
//       if (fb) {
//         String resp;
//         int code = camPost("/api/device/frame", fb->buf, fb->len, "", 0, 0, resp);
//         esp_camera_fb_return(fb);
//         if (code == 200) handleServerJson(resp);
//       }
//     }
//     vTaskDelay(pdMS_TO_TICKS(20));
//   }
// }
