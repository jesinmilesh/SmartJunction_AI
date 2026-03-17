/*
 * ============================================================
 *  SmartJunction AI — esp32_brain.ino  (4-Junction Edition)
 *  Board  : ESP32 Dev Module (Standard 38-pin or AI Thinker)
 *  Role   : Junction Intelligence Node (J1–J4)
 *           - Receives sensor data from Mega aggregator
 *           - Controls Signal LEDs and Barrier Servo
 *           - Publishes telemetry to central Edge Server
 *           - Subscribes to YOLO AI Vision results
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

// ============================================================
//  USER CONFIGURATION
// ============================================================
const char* WIFI_SSID   = "Jesin";        
const char* WIFI_PASS   = "12345678@@";   
const char* MQTT_HOST   = "10.152.59.95"; 
const int   MQTT_PORT   = 1883;

// Identity of this node. Change to "J2", "J3", or "J4" for other boards.
const char* JUNCTION_ID = "J1";

// ============================================================

// ---- Pin Mapping ----
#define SERIAL2_RX  16
#define SERIAL2_TX  17
#define RED_LED     25
#define GREEN_LED   26
#define YELLOW_LED  33
#define BUZZER_PIN  27
#define SERVO_PIN   14

// ---- Displays & Objects ----
#define SCREEN_W   128
#define SCREEN_H   64
#define OLED_ADDR  0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

Servo barrierServo;
WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---- State Variables ----
enum SignalState { SIG_RED, SIG_GREEN, SIG_YELLOW, SIG_EMERGENCY };
SignalState currentState = SIG_GREEN;

char topicSensors[64], topicCmd[64], topicVision[64];

bool manualOverride  = false;
bool emergencyActive = false;
bool ambulanceHere   = false;
bool accidentAlert   = false;

int vehicleCount = 0;
int trafficScore = 0; // 0-100 derived from YOLO + physical sensors
int megaDist     = 999;
int megaIR       = 0;
int megaPIR      = 0;

unsigned long lastUpdateMs   = 0;
unsigned long lastYellowMs   = 0;
unsigned long emergencyStart = 0;

// ---- Hardware Control Helpers ----
void setLights(bool r, bool y, bool g) {
    digitalWrite(RED_LED,    r ? HIGH : LOW);
    digitalWrite(YELLOW_LED, y ? HIGH : LOW);
    digitalWrite(GREEN_LED,  g ? HIGH : LOW);
}

void showRed()    { setLights(1, 0, 0); barrierServo.write(90); }
void showGreen()  { setLights(0, 0, 1); barrierServo.write(0);  }
void showYellow() { setLights(0, 1, 0); }

// Buzzer pulse helper
void beep(int ms) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(ms);
    digitalWrite(BUZZER_PIN, LOW);
}

// ============================================================
//  Telemetry & Display
// ============================================================

void updateOLED() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.print(F("SmartJunction: ")); display.println(JUNCTION_ID);
    display.drawLine(0, 9, 127, 9, WHITE);

    display.setCursor(0, 14);
    display.print(F("Vehicles: ")); display.println(vehicleCount);
    display.print(F("Traffic : ")); display.print(trafficScore); display.println(F("%"));
    display.print(F("Sensors : ")); 
    display.print(megaIR); display.print(F("/")); display.print(megaPIR);
    
    display.setCursor(0, 48);
    if (emergencyActive) display.println(F(">> EMERGENCY ACTIVE <<"));
    else if (manualOverride) display.println(F("Mode: MANUAL OVERRIDE"));
    else display.println(F("Mode: AUTONOMOUS"));

    display.display();
}

void publishTelemetry() {
    StaticJsonDocument<256> doc;
    doc["junction"] = JUNCTION_ID;
    doc["vehicles"] = vehicleCount;
    doc["traffic_score"] = trafficScore;
    doc["signal"] = (currentState == SIG_GREEN) ? "GREEN" : (currentState == SIG_YELLOW ? "YELLOW" : "RED");
    doc["dist"] = megaDist;
    doc["ir"] = megaIR;
    doc["pir"] = megaPIR;
    doc["emergency"] = emergencyActive;
    doc["manual_override"] = manualOverride;

    char buffer[256];
    serializeJson(doc, buffer);
    mqtt.publish(topicSensors, buffer);
}

// ============================================================
//  Input Handlers
// ============================================================

void handleMQTT(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (int i = 0; i < length; i++) msg += (char)payload[i];
    
    Serial.printf("[MQTT] Recv @ %s: %s\n", topic, msg.c_str());

    // 1. Vision Result (from YOLO)
    if (String(topic) == topicVision) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, msg)) return;
        vehicleCount = doc["vehicles"] | 0;
        int yoloScore = doc["traffic_score"] | 0;
        if (yoloScore > trafficScore) trafficScore = yoloScore; // Use higher of sensors/vision

        bool amb = doc["ambulance"] | false;
        if (amb && !emergencyActive) {
            emergencyActive = true;
            emergencyStart = millis();
            currentState = SIG_EMERGENCY;
            showGreen();
            beep(500);
        }
    }

    // 2. Control Command (from Dashboard)
    else if (String(topic) == topicCmd) {
        manualOverride = true;
        if (msg == "RED")         { currentState = SIG_RED; showRed(); }
        else if (msg == "GREEN")  { currentState = SIG_GREEN; showGreen(); }
        else if (msg == "YELLOW") { currentState = SIG_YELLOW; showYellow(); }
        else if (msg == "AUTO")   { manualOverride = false; }
    }

    // 3. Global Emergency Broadcast
    else if (String(topic) == "smartjunction/emergency") {
        StaticJsonDocument<128> doc;
        if (deserializeJson(doc, msg)) return;
        String type = doc["type"] | "";
        if (type == "CLEAR") {
            emergencyActive = false;
            manualOverride = false;
        } else if (doc["source"] != JUNCTION_ID) {
            // Force red for all other lanes during emergency
            emergencyActive = true;
            currentState = SIG_RED;
            showRed();
        }
    }
}

void readMegaSensors() {
    if (Serial2.available()) {
        String line = Serial2.readStringUntil('\n');
        StaticJsonDocument<128> doc;
        if (!deserializeJson(doc, line)) {
            megaDist = doc["dist"] | 999;
            megaIR   = doc["ir"]   | 0;
            megaPIR  = doc["pir"]  | 0;
            int sensorSc = doc["cong"] | 0;
            // Weighted traffic score (60% YOLO, 40% Sensors)
            trafficScore = (vehicleCount * 10 > sensorSc) ? (vehicleCount * 10) : sensorSc;
        }
    }
}

// ============================================================
//  Logic & State Machine
// ============================================================

void runSignalLogic() {
    if (manualOverride || emergencyActive) return;

    static unsigned long stateStart = 0;
    unsigned long now = millis();
    unsigned long greenTime = 15000 + (trafficScore * 200); // Dynamic green up to 35s

    switch (currentState) {
        case SIG_GREEN:
            if (now - stateStart > greenTime) {
                currentState = SIG_YELLOW;
                stateStart = now;
                showYellow();
            }
            break;
        case SIG_YELLOW:
            if (now - stateStart > 3000) {
                currentState = SIG_RED;
                stateStart = now;
                showRed();
            }
            break;
        case SIG_RED:
            if (now - stateStart > 10000) {
                currentState = SIG_GREEN;
                stateStart = now;
                showGreen();
            }
            break;
    }
}

void setup() {
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);

    pinMode(RED_LED,    OUTPUT);
    pinMode(GREEN_LED,  OUTPUT);
    pinMode(YELLOW_LED, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    
    barrierServo.attach(SERVO_PIN);
    showGreen();

    snprintf(topicSensors, 64, "smartjunction/%s/sensors", JUNCTION_ID);
    snprintf(topicCmd,     64, "smartjunction/%s/cmd",     JUNCTION_ID);
    snprintf(topicVision,  64, "smartjunction/%s/vision",  JUNCTION_ID);

    Wire.begin();
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        display.clearDisplay();
        display.display();
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(handleMQTT);
    
    Serial.println("\n[NODE] Started: " + String(JUNCTION_ID));
}

void loop() {
    if (!mqtt.connected()) {
        if (mqtt.connect(String("Brain-" + String(JUNCTION_ID)).c_str())) {
            mqtt.subscribe(topicCmd);
            mqtt.subscribe(topicVision);
            mqtt.subscribe("smartjunction/emergency");
        }
    }
    mqtt.loop();

    readMegaSensors();
    runSignalLogic();

    // Reset emergency after 30s if not re-triggered
    if (emergencyActive && (millis() - emergencyStart > 30000)) {
        emergencyActive = false;
    }

    static unsigned long lastPub = 0;
    if (millis() - lastPub > 2000) {
        publishTelemetry();
        updateOLED();
        lastPub = millis();
    }
}
