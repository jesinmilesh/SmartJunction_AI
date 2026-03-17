/*
 * ============================================================
 *  SmartJunction AI — esp32_brain.ino (Refactored)
 *  Board  : AI Thinker ESP32-CAM
 *  Role   : Smart Camera Node — WiFi + MQTT + Vision (Roboflow)
 *
 *  Function:
 *   - Hosts a live MJPEG stream for the Dashboard.
 *   - Captures frames and sends them to Roboflow for AI detection.
 *   - Publishes vision results (vehicles/persons) via MQTT.
 * ============================================================
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <base64.h>

// ============================================================
//  USER CONFIGURATION
// ============================================================
const char* WIFI_SSID      = "YOUR_WIFI_NAME";       // <-- change
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";   // <-- change
const char* MQTT_BROKER    = "192.168.1.100";         // laptop IP
const int   MQTT_PORT      = 1883;

// Roboflow credentials
const char* RF_API_KEY     = "YOUR_ROBOFLOW_API_KEY"; // <-- change
const char* RF_MODEL_ID    = "stars-ss3lr/projects/2";     // <-- Updated to your trained project
// ============================================================

// AI Thinker ESP32-CAM Pins
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

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ────────────────────────────────────────────────────────────
//  Camera Setup
// ────────────────────────────────────────────────────────────
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    return (err == ESP_OK);
}

// ────────────────────────────────────────────────────────────
//  HTTP Server Handlers
// ────────────────────────────────────────────────────────────
static esp_err_t captureHandler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t streamHandler(httpd_req_t* req) {
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    char partBuf[128];
    while (true) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hdrLen = snprintf(partBuf, sizeof(partBuf), STREAM_PART, fb->len);
            res = httpd_resp_send_chunk(req, partBuf, hdrLen);
        }
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(40)); // ~25 FPS
    }
    return res;
}

void startCameraServer() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t captureUri = { "/capture", HTTP_GET, captureHandler, NULL };
        httpd_uri_t streamUri  = { "/stream",  HTTP_GET, streamHandler,  NULL };
        httpd_register_uri_handler(server, &captureUri);
        httpd_register_uri_handler(server, &streamUri);
        Serial.println(F("[HTTP] Server started on port 80"));
    }
}

// ────────────────────────────────────────────────────────────
//  Vision & Logic
// ────────────────────────────────────────────────────────────
void publishVision(int vehicles, int persons) {
    StaticJsonDocument<128> doc;
    doc["vehicles"] = vehicles;
    doc["persons"]  = persons;
    char buf[128];
    serializeJson(doc, buf);
    mqtt.publish("smartjunction/vision", buf);
}

void callRoboflow() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

    String b64 = base64::encode(fb->buf, fb->len);
    esp_camera_fb_return(fb);

    String rfUrl = String("https://detect.roboflow.com/") + RF_MODEL_ID + "?api_key=" + RF_API_KEY + "&name=frame.jpg";
    HTTPClient http;
    http.begin(rfUrl);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(10000);

    int code = http.POST(b64);
    if (code == 200) {
        String res = http.getString();
        StaticJsonDocument<1024> doc;
        if (!deserializeJson(doc, res)) {
            int v = 0, p = 0;
            for (JsonObject pred : doc["predictions"].as<JsonArray>()) {
                String cls = pred["class"].as<String>();
                if (pred["confidence"].as<float>() > 0.5) {
                    if (cls == "cars" || cls == "vehicle") v++; // Handle "cars" from the new model
                    if (cls == "person") p++;
                }
            }
            publishVision(v, p);
            Serial.printf("[RF] Detected: %d vehicles, %d persons\n", v, p);
        }
    }
    http.end();
}

// ────────────────────────────────────────────────────────────
//  Setup & Loop
// ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    if (!initCamera()) {
        Serial.println(F("Camera init FAILED"));
        while (1) delay(1000);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.println(F("\nWiFi Connected"));

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    startCameraServer();
}

void loop() {
    if (!mqtt.connected()) {
        if (mqtt.connect("ESP32-SmartBrain")) {
            Serial.println(F("MQTT Connected"));
        }
    }
    mqtt.loop();

    static unsigned long lastRF = 0;
    if (millis() - lastRF > 5000) { // Call every 5 seconds
        callRoboflow();
        lastRF = millis();
    }
}
