/*
 * ============================================================
 *  SmartJunction AI — esp32_brain.ino
 *  Board  : ESP32 Dev Module (AI Thinker or generic 38-pin)
 *  Role   : System brain — WiFi + MQTT + Motor + OLED +
 *           Roboflow vision API integration.
 *
 *  Pin Map:
 *    GPIO 16 (RX2) ← Mega TX1   (via 1kΩ/2kΩ voltage divider)
 *    GPIO 17 (TX2) → (unused / future use)
 *    GPIO 25       → RED   traffic LED (via 220Ω)
 *    GPIO 26       → GREEN traffic LED (via 220Ω)
 *    GPIO 27       → Buzzer (active buzzer or transistor)
 *    GPIO 14       → Servo signal wire
 *    GPIO 21 (SDA) → OLED SDA
 *    GPIO 22 (SCL) → OLED SCL
 *
 *  Libraries needed:
 *    - ArduinoJson    (v6.x)  — Benoit Blanchon
 *    - PubSubClient           — Nick O'Leary
 *    - ESP32Servo             — Kevin Harrington
 *    - Adafruit SSD1306
 *    - Adafruit GFX Library
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <base64.h>

// ============================================================
//  USER CONFIGURATION — fill in before flashing
// ============================================================
const char* WIFI_SSID      = "YOUR_WIFI_NAME";       // <-- change
const char* WIFI_PASSWORD  = "YOUR_WIFI_PASSWORD";   // <-- change
const char* MQTT_BROKER    = "192.168.1.100";         // laptop IP running Node-RED
const int   MQTT_PORT      = 1883;

// Roboflow credentials (Step 6 in the guide)
const char* RF_API_KEY     = "YOUR_ROBOFLOW_API_KEY"; // <-- change
const char* RF_MODEL_ID    = "your-model-name/1";     // <-- change  e.g. "vehicle-detect/1"
const char* CAM_STREAM_IP  = "192.168.1.105";          // <-- change to ESP32-CAM IP
// ============================================================

// ---- Pin Definitions ----
#define SERIAL2_RX    16
#define SERIAL2_TX    17
#define RED_LIGHT     25
#define GREEN_LIGHT   26
#define BUZZER_PIN    27
#define SERVO_PIN     14

// ---- OLED ----
#define SCREEN_W 128
#define SCREEN_H  64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---- Objects ----
Servo        barrierServo;
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ---- State Variables ----
int  congestionScore  = 0;
int  lastDist         = 999;
bool pedestrianAlert  = false;
bool emergencyMode    = false;
int  lastVehicleCount = 0;
int  lastPersonCount  = 0;

// ────────────────────────────────────────────────────────────
//  Hardware helpers
// ────────────────────────────────────────────────────────────

void setSignal(bool greenOn) {
  digitalWrite(GREEN_LIGHT, greenOn ? HIGH : LOW);
  digitalWrite(RED_LIGHT,   greenOn ? LOW  : HIGH);
}

void dropBarrier()  { barrierServo.write(90); delay(300); }
void raiseBarrier() { barrierServo.write(0);  delay(300); }

void buzzerOn()  { digitalWrite(BUZZER_PIN, HIGH); }
void buzzerOff() { digitalWrite(BUZZER_PIN, LOW);  }

// ────────────────────────────────────────────────────────────
//  OLED update
// ────────────────────────────────────────────────────────────
void updateOLED(int score, int dist, bool ped, int vehicles, int persons) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header
  display.setCursor(0, 0);
  display.println(F("== SmartJunction AI =="));
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Sensor data
  display.setCursor(0, 12);
  display.print(F("Cong: ")); display.print(score); display.println(F("%"));

  display.setCursor(0, 22);
  display.print(F("Dist: "));
  if (dist >= 999) display.println(F("---cm"));
  else { display.print(dist); display.println(F("cm")); }

  display.setCursor(0, 32);
  display.print(F("Ped : ")); display.println(ped ? F("DETECTED!") : F("clear"));

  // Vision
  display.setCursor(0, 42);
  display.print(F("Veh:")); display.print(vehicles);
  display.print(F("  Per:")); display.println(persons);

  // Mode indicator
  display.setCursor(0, 54);
  if (emergencyMode)    display.println(F("[!] EMERGENCY MODE"));
  else if (ped)         display.println(F("[*] PEDESTRIAN HOLD"));
  else if (score > 60)  display.println(F("    HIGH TRAFFIC"));
  else                  display.println(F("    Normal"));

  display.display();
}

// ────────────────────────────────────────────────────────────
//  MQTT helpers
// ────────────────────────────────────────────────────────────
void publishSensors(int dist, int ir, int pir, int cong) {
  StaticJsonDocument<128> doc;
  doc["dist"] = dist;
  doc["ir"]   = ir;
  doc["pir"]  = pir;
  doc["cong"] = cong;
  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish("smartjunction/sensors", buf);
}

void publishVision(int vehicles, int persons) {
  StaticJsonDocument<128> doc;
  doc["vehicles"] = vehicles;
  doc["persons"]  = persons;
  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish("smartjunction/vision", buf);
}

void publishStatus(const char* mode) {
  mqtt.publish("smartjunction/status", mode);
}

// ────────────────────────────────────────────────────────────
//  MQTT incoming callback — remote override from Node-RED
// ────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.print(F("[MQTT] topic="));
  Serial.print(topic);
  Serial.print(F(" msg="));
  Serial.println(msg);

  if (String(topic) == "smartjunction/override") {
    if      (msg == "GREEN")     { setSignal(true);  raiseBarrier(); publishStatus("MANUAL_GREEN"); }
    else if (msg == "RED")       { setSignal(false); dropBarrier();  publishStatus("MANUAL_RED");   }
    else if (msg == "EMERGENCY") { emergencyMode = true;  publishStatus("EMERGENCY"); }
    else if (msg == "CLEAR")     { emergencyMode = false; publishStatus("NORMAL");    }
  }
}

// ────────────────────────────────────────────────────────────
//  WiFi / MQTT connection
// ────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.print(F("[WiFi] Connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t > 20000) {          // 20-second timeout
      Serial.println(F("\n[WiFi] timeout — continuing offline"));
      return;
    }
  }
  Serial.print(F("\n[WiFi] Connected. IP: "));
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  int tries = 0;
  while (!mqtt.connected() && tries < 3) {
    Serial.print(F("[MQTT] Connecting..."));
    if (mqtt.connect("ESP32-SmartJunction")) {
      Serial.println(F(" OK"));
      mqtt.subscribe("smartjunction/override");
    } else {
      Serial.print(F(" failed rc="));
      Serial.println(mqtt.state());
      delay(2000);
      tries++;
    }
  }
}

// ────────────────────────────────────────────────────────────
//  Roboflow — grab a frame from ESP32-CAM and call the API
// ────────────────────────────────────────────────────────────
String callRoboflow() {
  if (WiFi.status() != WL_CONNECTED) return "offline";

  // Step 1 — grab JPEG from ESP32-CAM /capture endpoint
  String captureUrl = String("http://") + CAM_STREAM_IP + "/capture";
  HTTPClient http;
  http.begin(captureUrl);
  http.setTimeout(5000);
  int httpCode = http.GET();

  if (httpCode != 200) {
    Serial.print(F("[CAM] Failed to get frame, code="));
    Serial.println(httpCode);
    http.end();
    return "error";
  }

  int len = http.getSize();
  if (len <= 0 || len > 50000) {   // guard against garbage sizes
    http.end();
    return "error";
  }

  uint8_t* imgBuf = (uint8_t*)malloc(len);
  if (!imgBuf) { http.end(); return "error"; }

  WiFiClient* stream = http.getStreamPtr();
  stream->readBytes(imgBuf, len);
  http.end();

  // Step 2 — base64 encode
  String b64 = base64::encode(imgBuf, len);
  free(imgBuf);

  // Step 3 — POST to Roboflow inference endpoint
  String rfUrl = String("https://detect.roboflow.com/")
               + RF_MODEL_ID
               + "?api_key=" + RF_API_KEY
               + "&name=frame.jpg";

  HTTPClient rfHttp;
  rfHttp.begin(rfUrl);
  rfHttp.addHeader("Content-Type", "application/x-www-form-urlencoded");
  rfHttp.setTimeout(10000);

  int rfCode = rfHttp.POST(b64);
  String result = "";
  if (rfCode == 200) {
    result = rfHttp.getString();
    Serial.print(F("[RF] response: "));
    Serial.println(result.substring(0, 120));   // print first 120 chars
  } else {
    Serial.print(F("[RF] HTTP error="));
    Serial.println(rfCode);
    result = "error";
  }
  rfHttp.end();
  return result;
}

// Parse Roboflow JSON → act on detections
void handleRoboflowResult(const String& json) {
  if (json == "error" || json == "offline") return;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print(F("[RF] JSON parse error: "));
    Serial.println(err.f_str());
    return;
  }

  int vehicleCount = 0;
  int personCount  = 0;

  JsonArray preds = doc["predictions"].as<JsonArray>();
  for (JsonObject p : preds) {
    String cls = p["class"].as<String>();
    float  conf = p["confidence"] | 0.0f;
    if (conf > 0.5f) {
      if (cls == "vehicle") vehicleCount++;
      if (cls == "person")  personCount++;
    }
  }

  lastVehicleCount = vehicleCount;
  lastPersonCount  = personCount;

  Serial.printf("[RF] Vehicles=%d  Persons=%d\n", vehicleCount, personCount);

  // Publish vision results
  publishVision(vehicleCount, personCount);

  // Vision-based pedestrian override (only when not already in emergency)
  if (!emergencyMode && personCount > 0) {
    setSignal(false);
    dropBarrier();
    buzzerOn();
    pedestrianAlert = true;
  }
}

// ────────────────────────────────────────────────────────────
//  Traffic signal decision engine
// ────────────────────────────────────────────────────────────
void runSignalLogic(int dist, bool ped, int cong) {
  if (emergencyMode) {
    // ── EMERGENCY: all-red, barrier down, buzzer on
    setSignal(false);
    dropBarrier();
    buzzerOn();
    publishStatus("EMERGENCY");
    return;
  }

  if (ped) {
    // ── PEDESTRIAN: red, barrier holds traffic, buzzer 3 s
    setSignal(false);
    dropBarrier();
    buzzerOn();
    publishStatus("PEDESTRIAN_HOLD");
    delay(3000);
    buzzerOff();
    // After 3 sec safe gap, allow vehicles again if none are there
    if (!pedestrianAlert) {
      setSignal(true);
      raiseBarrier();
      publishStatus("NORMAL");
    }
    return;
  }

  if (dist > 0 && dist < 80) {
    // ── VEHICLE APPROACHING: predictive green, open barrier
    setSignal(true);
    raiseBarrier();
    buzzerOff();
    publishStatus(cong > 60 ? "HIGH_TRAFFIC" : "VEHICLE_APPROACHING");
    return;
  }

  // ── DEFAULT: green, open, quiet
  setSignal(true);
  raiseBarrier();
  buzzerOff();
  publishStatus("NORMAL");
}

// ────────────────────────────────────────────────────────────
//  setup()
// ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);

  // Outputs
  pinMode(RED_LIGHT,   OUTPUT);
  pinMode(GREEN_LIGHT, OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  setSignal(true);   // startup: green
  buzzerOff();

  // Servo
  barrierServo.attach(SERVO_PIN, 500, 2400);
  raiseBarrier();

  // OLED
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("[OLED] init FAILED — check wiring"));
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("SmartJunction AI"));
  display.println(F("  Initialising..."));
  display.display();

  // Network
  connectWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  Serial.println(F("[ESP32] Setup complete."));
}

// ────────────────────────────────────────────────────────────
//  loop()
// ────────────────────────────────────────────────────────────
void loop() {
  // Keep MQTT alive
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) connectMQTT();
  if (mqtt.connected()) mqtt.loop();

  // ── Read JSON from Arduino Mega via UART ──
  if (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();

    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, line);

    if (!err) {
      int dist = doc["dist"] | 999;
      int ir   = doc["ir"]   | 0;
      int pir  = doc["pir"]  | 0;
      int cong = doc["cong"] | 0;

      lastDist       = dist;
      congestionScore = cong;
      // Pedestrian = motion (PIR) OR object on crosswalk beam (IR)
      pedestrianAlert = (pir == 1 || ir == 1);

      runSignalLogic(dist, pedestrianAlert, cong);
      updateOLED(cong, dist, pedestrianAlert, lastVehicleCount, lastPersonCount);
      publishSensors(dist, ir, pir, cong);
    } else {
      Serial.print(F("[UART] JSON err: "));
      Serial.println(err.f_str());
    }
  }

  // ── Roboflow vision call every 2 seconds ──
  static unsigned long lastRFcall = 0;
  if (millis() - lastRFcall > 2000) {
    String rfResult = callRoboflow();
    handleRoboflowResult(rfResult);
    lastRFcall = millis();
  }

  delay(100);
}
