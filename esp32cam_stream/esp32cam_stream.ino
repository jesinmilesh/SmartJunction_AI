/*
 * ============================================================
 *  SmartJunction AI — esp32cam_stream.ino
 *  Board  : AI Thinker ESP32-CAM
 *  Role   : Live MJPEG video stream over WiFi +
 *           /capture endpoint (single JPEG) for Roboflow.
 *
 *  Flash settings (Arduino IDE):
 *    Board      : AI Thinker ESP32-CAM
 *    Partition  : Huge APP (3MB No OTA)
 *    PSRAM      : Enabled
 *    Upload Spd : 115200 (use lower if uploads fail)
 *
 *  After flashing, open Serial Monitor @ 115200 baud.
 *  It will print: Camera stream at: http://<ip>
 *    Live stream  → http://<ip>/stream
 *    Single frame → http://<ip>/capture
 * ============================================================
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>

// ============================================================
//  USER CONFIGURATION
// ============================================================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";      // <-- change
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";  // <-- change
// ============================================================

// ---- AI Thinker ESP32-CAM GPIO Map ----
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ────────────────────────────────────────────────────────────
//  Camera initialisation
// ────────────────────────────────────────────────────────────
bool initCamera() {
  camera_config_t config;
  config.ledc_channel   = LEDC_CHANNEL_0;
  config.ledc_timer     = LEDC_TIMER_0;
  config.pin_d0         = Y2_GPIO_NUM;
  config.pin_d1         = Y3_GPIO_NUM;
  config.pin_d2         = Y4_GPIO_NUM;
  config.pin_d3         = Y5_GPIO_NUM;
  config.pin_d4         = Y6_GPIO_NUM;
  config.pin_d5         = Y7_GPIO_NUM;
  config.pin_d6         = Y8_GPIO_NUM;
  config.pin_d7         = Y9_GPIO_NUM;
  config.pin_xclk       = XCLK_GPIO_NUM;
  config.pin_pclk       = PCLK_GPIO_NUM;
  config.pin_vsync      = VSYNC_GPIO_NUM;
  config.pin_href       = HREF_GPIO_NUM;
  config.pin_sscb_sda   = SIOD_GPIO_NUM;
  config.pin_sscb_scl   = SIOC_GPIO_NUM;
  config.pin_pwdn       = PWDN_GPIO_NUM;
  config.pin_reset      = RESET_GPIO_NUM;
  config.xclk_freq_hz   = 20000000;
  config.pixel_format   = PIXFORMAT_JPEG;

  // Use PSRAM if available for larger frames
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;   // 640×480
    config.jpeg_quality = 10;              // 0–63, lower = better quality
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;  // 320×240
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  // Fine-tune sensor settings
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);     //  -2 to  2
    s->set_contrast(s, 0);       //  -2 to  2
    s->set_saturation(s, 0);     //  -2 to  2
    s->set_sharpness(s, 0);
    s->set_denoise(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0);        // 0=Auto
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 0);
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_lenc(s, 1);
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_dcw(s, 1);
    s->set_colorbar(s, 0);
  }
  return true;
}

// ────────────────────────────────────────────────────────────
//  HTTP handlers
// ────────────────────────────────────────────────────────────

// --- /capture — returns a single JPEG frame ---
static esp_err_t captureHandler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// --- /stream — continuous MJPEG stream ---
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  char partBuf[128];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { Serial.println(F("[CAM] frame capture failed")); continue; }

    // Send boundary
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hdrLen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, partBuf, hdrLen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;   // client disconnected
    vTaskDelay(pdMS_TO_TICKS(33));  // ~30 fps cap
  }
  return res;
}

// --- root / — simple info page ---
static esp_err_t rootHandler(httpd_req_t* req) {
  const char* html =
    "<html><body style='font-family:sans-serif;background:#111;color:#eee;'>"
    "<h2>SmartJunction AI — ESP32-CAM</h2>"
    "<p><a href='/stream' style='color:#4af'>Live Stream (MJPEG)</a></p>"
    "<p><a href='/capture' style='color:#4af'>Single Frame (JPEG)</a></p>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, strlen(html));
}

// ────────────────────────────────────────────────────────────
//  HTTP server start
// ────────────────────────────────────────────────────────────
void startCameraServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port    = 80;
  cfg.max_uri_handlers = 8;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    Serial.println(F("[HTTP] Server start failed"));
    return;
  }

  // Register URIs
  httpd_uri_t rootUri    = { "/",        HTTP_GET, rootHandler,    NULL };
  httpd_uri_t captureUri = { "/capture", HTTP_GET, captureHandler, NULL };
  httpd_uri_t streamUri  = { "/stream",  HTTP_GET, streamHandler,  NULL };

  httpd_register_uri_handler(server, &rootUri);
  httpd_register_uri_handler(server, &captureUri);
  httpd_register_uri_handler(server, &streamUri);

  Serial.println(F("[HTTP] Server started — routes: /, /capture, /stream"));
}

// ────────────────────────────────────────────────────────────
//  setup / loop
// ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[ESP32-CAM] SmartJunction Camera Node starting..."));

  if (!initCamera()) {
    Serial.println(F("[ESP32-CAM] Camera init failed — halting"));
    while (true) delay(1000);
  }
  Serial.println(F("[ESP32-CAM] Camera OK"));

  // Connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connecting"));
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t > 30000) {
      Serial.println(F("\n[WiFi] Could not connect — check credentials"));
      while (true) delay(1000);
    }
  }
  Serial.print(F("\n[WiFi] Connected. IP: "));
  Serial.println(WiFi.localIP());

  startCameraServer();

  Serial.print(F("[CAM] Stream  at: http://"));
  Serial.print(WiFi.localIP()); Serial.println(F("/stream"));
  Serial.print(F("[CAM] Capture at: http://"));
  Serial.print(WiFi.localIP()); Serial.println(F("/capture"));
}

void loop() {
  // HTTP server is interrupt-driven — nothing to do here.
  // Blink on-board LED every 2 s to show the board is alive.
  digitalWrite(4, HIGH); delay(50);
  digitalWrite(4, LOW);  delay(1950);
}
