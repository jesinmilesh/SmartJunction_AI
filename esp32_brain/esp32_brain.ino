/*
 * ============================================================
 *  SmartJunction AI — esp32_brain.ino  (4-Junction Edition)
 *  Board  : ESP32 Dev Module (AI Thinker or generic 38-pin)
 *  Role   : One instance per junction (J1–J4).
 *           – Reads IR / Ultrasonic data from an Arduino Mega
 *             via UART2
 *           – Controls RED / GREEN / YELLOW traffic LEDs
 *           – Controls a servo-driven barrier gate
 *           – Drives active buzzer for emergency / alerts
 *           – Reports to central Edge-AI server via MQTT
 *           – Obeys remote commands from the dashboard
 *
 *  Pin Map:
 *    GPIO 16 (RX2) ← Mega TX1   (via 1kΩ/2kΩ voltage divider)
 *    GPIO 17 (TX2) → (unused)
 *    GPIO 25       → RED    traffic LED (via 220Ω)
 *    GPIO 26       → GREEN  traffic LED (via 220Ω)
 *    GPIO 33       → YELLOW traffic LED (via 220Ω)
 *    GPIO 27       → Buzzer (active, or NPN transistor)
 *    GPIO 14       → Servo signal wire
 *    GPIO 21 (SDA) → OLED SDA
 *    GPIO 22 (SCL) → OLED SCL
 *
 *  Libraries needed (Board Manager: esp32 by Espressif ≥2.0):
 *    - ArduinoJson     (v6.x)  — Benoit Blanchon
 *    - PubSubClient             — Nick O'Leary
 *    - ESP32Servo               — Kevin Harrington
 *    - Adafruit SSD1306
 *    - Adafruit GFX Library
 * ============================================================
 */

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <Wire.h>

// ============================================================
//  USER CONFIGURATION — fill in before flashing each node
// ============================================================
const char *WIFI_SSID = "Jesin";        // <-- change
const char *WIFI_PASS = "12345678@@";   // <-- change
const char *MQTT_HOST = "10.152.59.95"; // Your PC IP
const int MQTT_PORT = 1883;

// Each ESP32 represents one junction (J1 / J2 / J3 / J4)
// Change this to "J2", "J3", or "J4" when flashing other nodes
const char *JUNCTION_ID = "J1";

// MQTT topics — built from JUNCTION_ID at runtime
//   smartjunction/J1/sensors   → outgoing sensor/status
//   smartjunction/J1/cmd       → incoming commands
//   smartjunction/emergency    → broadcast emergency alerts
//   smartjunction/traffic      → cross-junction traffic data
// ============================================================

// ---- Pin Definitions ----
#define SERIAL2_RX 16
#define SERIAL2_TX 17
#define RED_LED 25
#define GREEN_LED 26
#define YELLOW_LED 33
#define BUZZER_PIN 27
#define SERVO_PIN 14

// ---- OLED ----
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// ---- Objects ----
Servo barrierServo;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// ---- MQTT topic strings ----
char topicSensors[64];
char topicCmd[64];
char topicVision[64]; // smartjunction/J1/vision  ← YOLO results from server

// ============================================================
//  State machine
// ============================================================
enum SignalState {
  SIG_RED,
  SIG_GREEN,
  SIG_YELLOW,
  SIG_EMERGENCY_GREEN, // this lane cleared for emergency vehicle
  SIG_FORCED_RED       // forced red from dashboard
};

SignalState currentState = SIG_GREEN;
bool emergencyActive = false; // set by MQTT broadcast
bool manualOverride = false;
int vehicleCount = 0; // from YOLO via MQTT
int trafficScore = 0; // 0-100
bool ambulanceHere = false;
bool accidentAlert = false;
unsigned long lastYellowMs = 0;
unsigned long emergencyStart = 0;

// ---- Servo (non-blocking) ----
int servoTargetDeg = 0; // 0 = raised, 90 = dropped
bool servoMovePending = false;

// Sensor readings from Mega
int sensorDist = 999;
int sensorIR = 0;
int sensorPIR = 0;
int sensorCong = 0;

// ============================================================
//  Hardware helpers
// ============================================================
void setLights(bool r, bool y, bool g) {
  digitalWrite(RED_LED, r ? HIGH : LOW);
  digitalWrite(YELLOW_LED, y ? HIGH : LOW);
  digitalWrite(GREEN_LED, g ? HIGH : LOW);
}

void showRed() { setLights(true, false, false); }
void showGreen() { setLights(false, false, true); }
void showYellow() { setLights(false, true, false); }
void showEmergencyGreen() { setLights(false, false, true); }

// Non-blocking barrier: schedules a servo move without blocking the MQTT loop
void dropBarrier() {
  servoTargetDeg = 90;
  servoMovePending = true;
}
void raiseBarrier() {
  servoTargetDeg = 0;
  servoMovePending = true;
}

// Called every loop() iteration to apply pending servo motion
void updateServo() {
  if (!servoMovePending)
    return;
  barrierServo.write(servoTargetDeg);
  servoMovePending = false;
}

// Non-blocking buzzer beep state machine
struct Beeper {
  int times = 0;
  int ms = 200;
  int count = 0;
  bool beepOn = false;
  unsigned long nextMs = 0;
} _beeper;

void buzzerBeep(int times, int ms) {
  _beeper.times = times * 2; // ON+OFF pairs
  _beeper.ms = ms;
  _beeper.count = 0;
  _beeper.beepOn = false;
  _beeper.nextMs = millis();
}

void updateBuzzer() {
  if (_beeper.count >= _beeper.times)
    return;
  if (millis() < _beeper.nextMs)
    return;
  _beeper.beepOn = !_beeper.beepOn;
  digitalWrite(BUZZER_PIN, _beeper.beepOn ? HIGH : LOW);
  _beeper.count++;
  _beeper.nextMs = millis() + _beeper.ms;
  // Stop when continuous buzzerOn() was called — don't interfere
}

void buzzerOn() {
  _beeper.times = 0;
  digitalWrite(BUZZER_PIN, HIGH);
}
void buzzerOff() {
  _beeper.times = 0;
  _beeper.count = _beeper.times;
  digitalWrite(BUZZER_PIN, LOW);
}

// ============================================================
//  OLED display
// ============================================================
void updateOLED() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header row
  display.setCursor(0, 0);
  display.print(F("SmartJunction "));
  display.println(JUNCTION_ID);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Traffic
  display.setCursor(0, 12);
  display.print(F("Vehicles: "));
  display.println(vehicleCount);
  display.setCursor(0, 22);
  display.print(F("Traffic : "));
  display.print(trafficScore);
  display.println(F("%"));
  display.setCursor(0, 32);
  display.print(F("Dist    : "));
  if (sensorDist >= 999)
    display.println(F("---cm"));
  else {
    display.print(sensorDist);
    display.println(F("cm"));
  }

  // Signal state
  display.setCursor(0, 44);
  if (emergencyActive && ambulanceHere)
    display.println(F("[!!] EMRG GREEN "));
  else if (emergencyActive)
    display.println(F("[!!] EMRG RED   "));
  else if (accidentAlert)
    display.println(F("[!]  ACCIDENT   "));
  else if (manualOverride)
    display.println(F("[M]  MANUAL     "));
  else if (trafficScore > 70)
    display.println(F("[H]  HIGH TRAFFIC"));
  else
    display.println(F("     Normal      "));

  // Separator & PIR
  display.setCursor(0, 54);
  display.print(F("PIR:"));
  display.print(sensorPIR);
  display.print(F(" IR:"));
  display.print(sensorIR);
  display.print(F(" A:"));
  display.print(accidentAlert ? F("Y") : F("N"));

  display.display();
}

// ============================================================
//  MQTT publish helpers
// ============================================================
void publishTelemetry() {
  StaticJsonDocument<256> doc;
  doc["junction"] = JUNCTION_ID;
  doc["vehicles"] = vehicleCount;
  doc["traffic_score"] = trafficScore;
  doc["ambulance"] = ambulanceHere;
  doc["accident"] = accidentAlert;
  doc["dist"] = sensorDist;
  doc["ir"] = sensorIR;
  doc["pir"] = sensorPIR;
  doc["emergency"] = emergencyActive;
  doc["manual"] = manualOverride;

  // Signal color string
  if (currentState == SIG_GREEN || currentState == SIG_EMERGENCY_GREEN)
    doc["signal"] = "GREEN";
  else if (currentState == SIG_YELLOW)
    doc["signal"] = "YELLOW";
  else
    doc["signal"] = "RED";

  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish(topicSensors, buf, true); // retained
}

void publishEmergencyAlert(const char *type, const char *details) {
  StaticJsonDocument<256> doc;
  doc["source"] = JUNCTION_ID;
  doc["type"] = type;
  doc["details"] = details;
  doc["ts"] = millis();
  char buf[256];
  serializeJson(doc, buf);
  mqtt.publish("smartjunction/alerts", buf);
  Serial.printf("[ALERT] type=%s details=%s\n", type, details);
}

// ============================================================
//  MQTT incoming command handler
//  Topic: smartjunction/<JunctionID>/cmd
//  Topic: smartjunction/emergency   (broadcast)
// ============================================================
void mqttCallback(char *topic, byte *payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++)
    msg += (char)payload[i];

  Serial.printf("[MQTT] topic=%s  msg=%s\n", topic, msg.c_str());

  String t(topic);

  // ───── Vision / YOLO results from edge server ──────────────
  if (t == String(topicVision)) {
    handleVisionData(msg);
    return;
  }

  // ───── Emergency broadcast from any junction or server ─────
  if (t == "smartjunction/emergency") {
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, msg)) {
      String srcJunction = doc["source"] | "";
      String emType = doc["type"] | "";

      if (emType == "AMBULANCE" || emType == "FIRE_TRUCK") {
        emergencyActive = true;
        emergencyStart = millis();

        // If the emergency vehicle is AT THIS junction → go green
        ambulanceHere = (srcJunction == JUNCTION_ID);

        if (ambulanceHere) {
          currentState = SIG_EMERGENCY_GREEN;
          showGreen();
          raiseBarrier();
          buzzerOn();
          publishTelemetry();
        } else {
          // All other junctions → forced red
          currentState = SIG_RED;
          showRed();
          dropBarrier();
          buzzerOff();
          publishTelemetry();
        }
      } else if (emType == "CLEAR") {
        emergencyActive = false;
        ambulanceHere = false;
        currentState = SIG_GREEN;
        showGreen();
        raiseBarrier();
        buzzerOff();
      }
    }
    return;
  }

  // ───── Direct command to this junction ─────
  if (t == String(topicCmd)) {
    if (msg == "GREEN") {
      manualOverride = true;
      currentState = SIG_GREEN;
      showGreen();
      raiseBarrier();
      buzzerOff();
    } else if (msg == "RED") {
      manualOverride = true;
      currentState = SIG_RED;
      showRed();
      dropBarrier();
    } else if (msg == "YELLOW") {
      manualOverride = true;
      currentState = SIG_YELLOW;
      showYellow();
    } else if (msg == "AUTO") {
      manualOverride = false;
      emergencyActive = false;
      ambulanceHere = false;
      currentState = SIG_GREEN;
      showGreen();
      raiseBarrier();
      buzzerOff();
    } else if (msg == "ALERT_PATROL") {
      publishEmergencyAlert("PATROL_NEEDED",
                            "Manual patrol request from dashboard");
      buzzerBeep(3);
    }
    publishTelemetry();
  }
}

// ============================================================
//  Network helpers
// ============================================================
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
    if (millis() - t > 20000) {
      Serial.println("\n[WiFi] timeout — continuing offline");
      return;
    }
  }
  Serial.printf("\n[WiFi] Connected → IP: %s\n",
                WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "ESP32-%s", JUNCTION_ID);
  int tries = 0;
  while (!mqtt.connected() && tries < 5) {
    Serial.printf("[MQTT] Connecting as %s ...\n", clientId);
    if (mqtt.connect(clientId)) {
      Serial.println("[MQTT] Connected!");
      mqtt.subscribe(topicCmd);
      mqtt.subscribe(topicVision); // YOLO results from edge server
      mqtt.subscribe(
          "smartjunction/emergency"); // cross-junction emergency broadcast
      Serial.printf("[MQTT] Subscribed: %s | %s | smartjunction/emergency\n",
                    topicCmd, topicVision);
    } else {
      Serial.printf("[MQTT] failed rc=%d  retry in 3s\n", mqtt.state());
      delay(3000);
      tries++;
    }
  }
}

// ============================================================
//  Traffic signal logic (autonomous, runs when no override)
// ============================================================
void runAutoSignalLogic() {
  if (manualOverride || emergencyActive)
    return;

  static unsigned long greenStart = 0;
  static unsigned long redStart = 0;

  // Dynamic green time: 15s base + up to 25s for heavy traffic
  unsigned long greenTime = 15000UL + (unsigned long)(trafficScore * 250);
  unsigned long redTime = 12000UL;

  switch (currentState) {
  case SIG_GREEN:
    if (millis() - greenStart > greenTime) {
      currentState = SIG_YELLOW;
      lastYellowMs = millis();
      showYellow();
    }
    break;

  case SIG_YELLOW:
    if (millis() - lastYellowMs > 3000) {
      currentState = SIG_RED;
      redStart = millis();
      showRed();
      dropBarrier();
    }
    break;

  case SIG_RED:
    if (millis() - redStart > redTime) {
      currentState = SIG_GREEN;
      greenStart = millis();
      showGreen();
      raiseBarrier();
    }
    break;

  default:
    break;
  }
}

// ============================================================
//  YOLO / Vision data injection (arrives via MQTT from server)
//  Topic: smartjunction/J1/vision
// ============================================================
void handleVisionData(const String &json) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, json))
    return;

  vehicleCount = doc["vehicles"] | 0;
  trafficScore = doc["traffic_score"] | 0;
  bool amb = doc["ambulance"] | false;
  bool fire = doc["fire_truck"] | false;
  bool acc = doc["accident"] | false;

  // ── Emergency vehicle detected ──────────────────────────
  if ((amb || fire) && !emergencyActive) {
    emergencyActive = true;
    ambulanceHere = true;
    emergencyStart = millis();
    currentState = SIG_EMERGENCY_GREEN;
    showGreen();
    raiseBarrier();
    buzzerOn();

    // Broadcast to all other junctions
    StaticJsonDocument<128> alert;
    alert["source"] = JUNCTION_ID;
    alert["type"] = amb ? "AMBULANCE" : "FIRE_TRUCK";
    char alertBuf[128];
    serializeJson(alert, alertBuf);
    mqtt.publish("smartjunction/emergency", alertBuf);

    publishEmergencyAlert(amb ? "AMBULANCE" : "FIRE_TRUCK",
                          "Emergency vehicle detected — lane cleared");
  }

  // ── Accident or illegal activity ────────────────────────
  if (acc && !accidentAlert) {
    accidentAlert = true;
    publishEmergencyAlert(
        "ACCIDENT",
        "Accident / suspicious activity detected — patrol dispatched");
    buzzerBeep(5, 150);
  } else if (!acc) {
    accidentAlert = false;
  }

  // ── Auto-clear emergency after 60 s with no re-trigger ──
  if (emergencyActive && millis() - emergencyStart > 60000) {
    emergencyActive = false;
    ambulanceHere = false;
    currentState = SIG_GREEN;
    showGreen();
    raiseBarrier();
    buzzerOff();
    StaticJsonDocument<128> clr;
    clr["source"] = JUNCTION_ID;
    clr["type"] = "CLEAR";
    char clrBuf[128];
    serializeJson(clr, clrBuf);
    mqtt.publish("smartjunction/emergency", clrBuf);
  }
}

// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
  Serial2.setTimeout(50); // Prevent blocking the main loop if Mega is slow

  // Build MQTT topics from JUNCTION_ID
  snprintf(topicSensors, sizeof(topicSensors), "smartjunction/%s/sensors",
           JUNCTION_ID);
  snprintf(topicCmd, sizeof(topicCmd), "smartjunction/%s/cmd", JUNCTION_ID);
  snprintf(topicVision, sizeof(topicVision), "smartjunction/%s/vision",
           JUNCTION_ID);

  // GPIO setup
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  showGreen();
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
  display.print(F("SmartJunction AI\nJunction: "));
  display.println(JUNCTION_ID);
  display.println(F("Initialising..."));
  display.display();

  // Network
  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);
  connectMQTT();

  // Startup beep (non-blocking — triggers via updateBuzzer in loop)
  buzzerBeep(2, 100);
  Serial.printf("[%s] Setup complete\n", JUNCTION_ID);
}

// ============================================================
//  loop()
// ============================================================
void loop() {
  // Keep MQTT alive
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected())
    connectMQTT();
  if (mqtt.connected())
    mqtt.loop();

  // ── Non-blocking hardware updates ────────────────────────
  updateServo();
  updateBuzzer();

  // ── Read JSON from Arduino Mega via UART ──────────────────
  if (Serial2.available()) {
    String line = Serial2.readStringUntil('\n');
    line.trim();
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, line)) {
      sensorDist = doc["dist"] | 999;
      sensorIR = doc["ir"] | 0;
      sensorPIR = doc["pir"] | 0;
      sensorCong = doc["cong"] | 0;

      // Supplement traffic score with physical sensor data
      if (sensorCong > trafficScore)
        trafficScore = sensorCong;
    }
  }

  // ── Autonomous signal logic ───────────────────────────────
  runAutoSignalLogic();

  // ── Publish telemetry every 2 s ───────────────────────────
  static unsigned long lastPub = 0;
  if (millis() - lastPub > 2000) {
    updateOLED();
    if (mqtt.connected())
      publishTelemetry();
    lastPub = millis();
  }

  // No blocking delay() here — keeps MQTT loop responsive
}
