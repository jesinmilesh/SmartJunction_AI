/*
 * ============================================================
 *  SmartJunction AI — mega_sensors.ino
 *  Board  : Arduino Mega 2560
 *  Role   : Sensor hub — reads Ultrasonic, IR, PIR sensors
 *           and streams JSON to ESP32 via UART (Serial1).
 *
 *  Wiring (Mega → ESP32):
 *    Mega TX1 (pin 18) ──┬── 1kΩ ── ESP32 RX2 (GPIO 16)
 *                        └── 2kΩ ── GND
 *    (Voltage divider: 5V Mega TX → 3.3V ESP32 RX)
 *    Shared GND
 *
 *  Libraries needed (Sketch → Include Library → Manage Libraries):
 *    - NewPing   by Tim Eckel
 *    - ArduinoJson by Benoit Blanchon (v6.x)
 * ============================================================
 */

#include <NewPing.h>
#include <ArduinoJson.h>

// ---- Pin Definitions ----
#define TRIG_PIN     9    // Ultrasonic HC-SR04 Trigger
#define ECHO_PIN     10   // Ultrasonic HC-SR04 Echo
#define IR_PIN       7    // IR proximity sensor (LOW = object detected on most modules)
#define PIR_PIN      6    // PIR motion sensor (HIGH = motion detected)
#define MAX_DISTANCE 200  // Maximum measurable distance in cm

// ---- Objects ----
NewPing sonar(TRIG_PIN, ECHO_PIN, MAX_DISTANCE);

// ---- Congestion Scoring Helper ----
/*
 * Maps raw ultrasonic distance to a 0-100 congestion score.
 * dist = 0 means no echo (out of range) → treat as 0 congestion.
 */
int calcCongestion(int dist) {
  if (dist <= 0)               return 0;   // no reading
  if (dist < 20)               return 90;  // very close → heavy traffic
  if (dist < 50)               return 60;  // moderate distance
  if (dist < 100)              return 30;  // light traffic
  return 0;                               // clear road
}

void setup() {
  Serial.begin(115200);   // USB — for debug / Serial Monitor
  Serial1.begin(115200);  // TX1 / RX1 → to ESP32

  pinMode(IR_PIN,  INPUT);
  pinMode(PIR_PIN, INPUT);

  Serial.println(F("[Mega] SmartJunction Sensor Hub started."));
}

void loop() {
  // ---- Read sensors ----
  unsigned int dist    = sonar.ping_cm();        // 0 if out of range
  int          ir_val  = digitalRead(IR_PIN);    // 0 = blocked (active-LOW modules)
  int          pir_val = digitalRead(PIR_PIN);   // 1 = motion detected

  // IR active-LOW modules report 0 when an object IS present.
  // Normalise: ir_detected = 1 when object present.
  int ir_detected = (ir_val == LOW) ? 1 : 0;

  int congestion = calcCongestion(dist);

  // ---- Build JSON payload ----
  StaticJsonDocument<128> doc;
  doc["dist"] = (int)dist;        // cm
  doc["ir"]   = ir_detected;      // 0 or 1
  doc["pir"]  = pir_val;          // 0 or 1
  doc["cong"] = congestion;       // 0-100 score

  // ---- Transmit ----
  String output;
  serializeJson(doc, output);
  Serial1.println(output);   // ESP32 reads this line

  // ---- Debug to USB Serial Monitor ----
  Serial.println(output);

  delay(200);   // 5 Hz update rate
}
