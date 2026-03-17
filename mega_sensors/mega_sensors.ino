/*
 * ============================================================
 *  SmartJunction AI — mega_sensors.ino  (4-Junction Edition)
 *  Board  : Arduino Mega 2560
 *  Role   : Sensor hub — reads Ultrasonic HC-SR04, IR beam,
 *           and PIR motion sensor; streams JSON to ESP32 via
 *           UART (Serial1) at 5 Hz.
 *
 *  Wiring (Mega → ESP32):
 *    Mega TX1 (pin 18) ──┬── 1kΩ ── ESP32 RX2 (GPIO 16)
 *                         └── 2kΩ ── GND
 *    (Voltage divider: 5V Mega TX → 3.3V ESP32 RX safe level)
 *    Shared GND between Mega and ESP32 REQUIRED.
 *
 *  Sensor Pins:
 *    HC-SR04 TRIG  → D9
 *    HC-SR04 ECHO  → D10
 *    IR module OUT → D7   (active-LOW: LOW = object detected)
 *    PIR OUT       → D6   (active-HIGH: HIGH = motion)
 *
 *  Libraries needed (Sketch → Include Library → Manage Libraries):
 *    - NewPing      by Tim Eckel        (ultrasonic)
 *    - ArduinoJson  by Benoit Blanchon  (v6.x)
 *
 *  JSON output format (one line per packet, newline-terminated):
 *    {"dist":85,"ir":0,"pir":1,"cong":30}
 *
 *  Field reference:
 *    dist : distance in cm (0 = no echo / out of range)
 *    ir   : 1 = object on crosswalk beam, 0 = clear
 *    pir  : 1 = motion detected, 0 = still
 *    cong : 0-100 congestion score derived from distance
 * ============================================================
 */

#include <NewPing.h>
#include <ArduinoJson.h>

// ---- Pin Definitions ----------------------------------------
#define TRIG_PIN      9    // HC-SR04 Trigger
#define ECHO_PIN     10    // HC-SR04 Echo
#define IR_PIN        7    // IR proximity (active-LOW module)
#define PIR_PIN       6    // PIR motion (active-HIGH)
#define MAX_DISTANCE 200   // Max measurable distance (cm)

// ---- Objects ------------------------------------------------
NewPing sonar(TRIG_PIN, ECHO_PIN, MAX_DISTANCE);

// ---- Congestion score (0-100) from distance -----------------
// Short distance  → many vehicles close → high congestion
int calcCongestion(int dist) {
  if (dist <= 0)   return 0;   // no echo — clear or out of range
  if (dist <  20)  return 95;  // < 20 cm  — bumper-to-bumper
  if (dist <  50)  return 70;  // 20-50 cm — heavy traffic
  if (dist < 100)  return 40;  // 50-100 cm — moderate
  if (dist < 150)  return 15;  // 100-150 cm — light
  return 0;                    // > 150 cm — clear road
}

// ---- setup() ------------------------------------------------
void setup() {
  Serial.begin(115200);    // USB — debug / Serial Monitor
  Serial1.begin(115200);   // TX1 (pin 18) → ESP32 RX2 (GPIO 16)

  pinMode(IR_PIN,  INPUT);
  pinMode(PIR_PIN, INPUT);

  Serial.println(F("[Mega] SmartJunction Sensor Hub v2 started."));
  Serial.println(F("[Mega] Format: {\"dist\":cm,\"ir\":0/1,\"pir\":0/1,\"cong\":0-100}"));
}

// ---- loop() -------------------------------------------------
void loop() {
  // Read sensors
  int dist    = (int)sonar.ping_cm();          // 0 if out of range
  int ir_raw  = digitalRead(IR_PIN);
  int pir_val = digitalRead(PIR_PIN);

  // Most IR modules are active-LOW: LOW = object present
  int ir_detected = (ir_raw == LOW) ? 1 : 0;

  int congestion = calcCongestion(dist);

  // Build JSON
  StaticJsonDocument<128> doc;
  doc["dist"] = dist;
  doc["ir"]   = ir_detected;
  doc["pir"]  = pir_val;
  doc["cong"] = congestion;

  // Send to ESP32 via UART
  String output;
  output.reserve(64);
  serializeJson(doc, output);
  Serial1.println(output);

  // Mirror to USB Serial Monitor for debugging
  Serial.println(output);

  delay(200);   // 5 Hz update rate
}
