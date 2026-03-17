/*
 * ============================================================
 *  SmartJunction AI — esp32cam_stream.ino  (4-Junction Edition)
 *  Board  : AI Thinker ESP32-CAM
 *  Role   : Live MJPEG video stream + single-frame capture
 *           Used by the Edge-AI server (YOLOv8) for:
 *             • Vehicle counting / traffic estimation
 *             • Emergency vehicle detection (ambulance, fire truck)
 *             • Accident / illegal activity detection
 *
 *  Flash settings (Arduino IDE):
 *    Board      : AI Thinker ESP32-CAM
 *    Partition  : Huge APP (3MB No OTA)
 *    PSRAM      : Enabled
 *    Upload Spd : 115200
 *
 *  After flashing, open Serial Monitor @ 115200 baud.
 *  URLs printed:
 *    Live stream  → http://<ip>/stream
 *    Single JPEG  → http://<ip>/capture
 *    Camera info  → http://<ip>/info
 *
 *  Each camera node reports its junction ID in HTTP headers
 *  and via MQTT so the Edge-AI server knows which lane it is.
 * ============================================================
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ============================================================
//  USER CONFIGURATION
// ============================================================
const char *WIFI_SSID = "Jesin";        // <-- change
const char *WIFI_PASS = "12345678@@";   // <-- change
const char *MQTT_HOST = "10.152.59.95"; // Your PC IP
const int MQTT_PORT = 1883;

// Change for each camera node: "J1" / "J2" / "J3" / "J4"
const char *JUNCTION_ID = "J1";

// Camera resolution — FRAMESIZE_VGA (640×480) requires PSRAM
// FRAMESIZE_QVGA (320×240) works without PSRAM
#define PREFER_VGA true
// ============================================================

// ---- AI Thinker ESP32-CAM GPIO Map ----------------------
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// Flash LED (GPIO 4 on AI Thinker)
#define LED_GPIO 4

// ---- MQTT ------------------------------------------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char topicStatus[64];

// ---- Stats -----------------------------------------------
unsigned long framesServed = 0;
unsigned long capturesServed = 0;
unsigned long startTimeMs = 0;

// ============================================================
//  Camera initialisation
// ============================================================
bool initCamera() {
  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM;
  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;
  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;
  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;
  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM;
  cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM;
  cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;

  if (PREFER_VGA && psramFound()) {
    cfg.frame_size = FRAMESIZE_VGA; // 640×480
    cfg.jpeg_quality = 10;          // 0-63, lower = better
    cfg.fb_count = 2;
  } else {
    cfg.frame_size = FRAMESIZE_QVGA; // 320×240
    cfg.jpeg_quality = 12;
    cfg.fb_count = 1;
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }

  // Optimise sensor for traffic / outdoor scene
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 1);
    s->set_denoise(s, 1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_wb_mode(s, 0); // 0 = Auto
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1); // AEC DSP
    s->set_gain_ctrl(s, 1);
    s->set_agc_gain(s, 0);
    s->set_gainceiling(s, (gainceiling_t)2);
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

// ============================================================
//  HTTP Handlers
// ============================================================

// ---- /capture — single JPEG frame ---
static esp_err_t captureHandler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  capturesServed++;

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Junction-ID", JUNCTION_ID);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ---- /stream — continuous MJPEG ---
#define BOUNDARY "--frame"
static const char *CTYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *BNDRY = "\r\n--frame\r\n";
static const char *PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, CTYPE);
  if (res != ESP_OK)
    return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "X-Junction-ID", JUNCTION_ID);

  char hdr[96];
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(10);
      continue;
    }
    framesServed++;

    res = httpd_resp_send_chunk(req, BNDRY, strlen(BNDRY));
    if (res == ESP_OK) {
      size_t n = snprintf(hdr, sizeof(hdr), PART, fb->len);
      res = httpd_resp_send_chunk(req, hdr, n);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    if (res != ESP_OK)
      break;                       // client disconnected
    vTaskDelay(pdMS_TO_TICKS(33)); // ~30 fps cap
  }
  return res;
}

// ---- /info — JSON info page for dashboard ---
static esp_err_t infoHandler(httpd_req_t *req) {
  char json[512];
  uint32_t uptimeSec = (millis() - startTimeMs) / 1000;
  snprintf(json, sizeof(json),
           "{"
           "\"junction\":\"%s\","
           "\"ip\":\"%s\","
           "\"psram\":%s,"
           "\"resolution\":\"%s\","
           "\"frames_served\":%lu,"
           "\"captures_served\":%lu,"
           "\"uptime_sec\":%u,"
           "\"stream_url\":\"http://%s/stream\","
           "\"capture_url\":\"http://%s/capture\""
           "}",
           JUNCTION_ID, WiFi.localIP().toString().c_str(),
           psramFound() ? "true" : "false",
           psramFound() ? "VGA(640x480)" : "QVGA(320x240)", framesServed,
           capturesServed, uptimeSec, WiFi.localIP().toString().c_str(),
           WiFi.localIP().toString().c_str());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

// ---- / — HTML info page ---
static esp_err_t rootHandler(httpd_req_t *req) {
  char html[1024];
  snprintf(html, sizeof(html),
           "<html><head>"
           "<meta charset='UTF-8'>"
           "<title>SmartJunction %s Camera</title>"
           "<style>"
           "body{font-family:sans-serif;background:#0d1117;color:#e6edf3;"
           "padding:24px}"
           "h1{color:#58a6ff}a{color:#79c0ff}"
           ".badge{display:inline-block;background:#1f6feb;color:#fff;padding:"
           "2px 10px;"
           "border-radius:12px;font-size:.85em;margin-left:8px}"
           "</style></head><body>"
           "<h1>SmartJunction AI <span class='badge'>%s</span></h1>"
           "<p>IP: <strong>%s</strong></p>"
           "<p>PSRAM: <strong>%s</strong> | Resolution: <strong>%s</strong></p>"
           "<p>"
           "<a href='/stream'>&#9654; Live Stream (MJPEG)</a> &nbsp;|&nbsp;"
           "<a href='/capture'>&#128247; Single Frame (JPEG)</a> &nbsp;|&nbsp;"
           "<a href='/info'>&#128196; JSON Info</a>"
           "</p>"
           "<img src='/stream' width='640' style='border:2px solid "
           "#30363d;border-radius:8px;margin-top:16px'>"
           "</body></html>",
           JUNCTION_ID, JUNCTION_ID, WiFi.localIP().toString().c_str(),
           psramFound() ? "Yes" : "No",
           psramFound() ? "VGA (640×480)" : "QVGA (320×240)");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, strlen(html));
}

// ============================================================
//  HTTP server start
// ============================================================
void startCameraServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.max_uri_handlers = 16;
  cfg.stack_size = 8192;

  httpd_handle_t server = NULL;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    Serial.println(F("[HTTP] Server start failed"));
    return;
  }

  httpd_uri_t uris[] = {
      {"/", HTTP_GET, rootHandler, NULL},
      {"/capture", HTTP_GET, captureHandler, NULL},
      {"/stream", HTTP_GET, streamHandler, NULL},
      {"/info", HTTP_GET, infoHandler, NULL},
  };
  for (auto &u : uris)
    httpd_register_uri_handler(server, &u);
  Serial.println(F("[HTTP] Server started → /, /stream, /capture, /info"));
}

// ============================================================
//  MQTT
// ============================================================
void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "ESP32CAM-%s", JUNCTION_ID);
  int tries = 0;
  while (!mqtt.connected() && tries < 3) {
    if (mqtt.connect(clientId)) {
      Serial.println("[MQTT] Connected");
      // Report camera online
      StaticJsonDocument<128> doc;
      doc["junction"] = JUNCTION_ID;
      doc["ip"] = WiFi.localIP().toString();
      doc["stream_url"] =
          String("http://") + WiFi.localIP().toString() + "/stream";
      doc["capture_url"] =
          String("http://") + WiFi.localIP().toString() + "/capture";
      doc["status"] = "online";
      char buf[256];
      serializeJson(doc, buf);
      mqtt.publish(topicStatus, buf, true); // retained
    } else {
      delay(2000);
      tries++;
    }
  }
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.printf("\n[ESP32-CAM] SmartJunction Camera Node — Junction %s\n",
                JUNCTION_ID);

  // Build MQTT topic
  snprintf(topicStatus, sizeof(topicStatus), "smartjunction/%s/camera",
           JUNCTION_ID);

  // Flash LED
  pinMode(LED_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, LOW);

  // Camera
  if (!initCamera()) {
    Serial.println(F("[ESP32-CAM] Camera init failed — halting"));
    while (true) {
      digitalWrite(LED_GPIO, !digitalRead(LED_GPIO));
      delay(200);
    }
  }
  Serial.println(F("[ESP32-CAM] Camera OK"));

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("[WiFi] Connecting"));
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t > 30000) {
      Serial.println(F("\n[WiFi] Timeout — restarting"));
      ESP.restart();
    }
  }
  Serial.printf("\n[WiFi] Connected → IP: %s\n",
                WiFi.localIP().toString().c_str());

  startCameraServer();

  // MQTT
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  connectMQTT();

  startTimeMs = millis();
  Serial.printf("[CAM] Stream  → http://%s/stream\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("[CAM] Capture → http://%s/capture\n",
                WiFi.localIP().toString().c_str());
  Serial.printf("[CAM] Info    → http://%s/info\n",
                WiFi.localIP().toString().c_str());
}

void loop() {
  // MQTT keep-alive
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected())
    connectMQTT();
  if (mqtt.connected())
    mqtt.loop();

  // Heartbeat LED blink — non-blocking 50 ms pulse every 2 s
  static unsigned long lastBlink = 0;
  static bool ledPulseOn = false;
  unsigned long now = millis();
  if (!ledPulseOn && now - lastBlink > 2000) {
    digitalWrite(LED_GPIO, HIGH); // start ON phase
    ledPulseOn = true;
    lastBlink = now;
  } else if (ledPulseOn && now - lastBlink > 50) {
    digitalWrite(LED_GPIO, LOW); // end ON phase after 50 ms
    ledPulseOn = false;
  }

  // Periodic MQTT heartbeat every 30 s
  static unsigned long lastHb = 0;
  if (millis() - lastHb > 30000 && mqtt.connected()) {
    StaticJsonDocument<128> doc;
    doc["junction"] = JUNCTION_ID;
    doc["status"] = "heartbeat";
    doc["uptime_sec"] = (millis() - startTimeMs) / 1000;
    doc["frames"] = framesServed;
    char buf[192];
    serializeJson(doc, buf);
    mqtt.publish(topicStatus, buf, true);
    lastHb = millis();
  }
}
