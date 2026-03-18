/*
 * ============================================================
 *  SmartJunction AI — esp32_brain.ino  (Integrated v3.2)
 *  Board  : ESP32 Dev Module (Standard 38-pin)
 *  Role   : Junction Intelligence Node
 *           - Receives sensor data from Mega aggregator (UART)
 *           - Fetches Vision frames from Camera Node
 *           - Runs Signal Cycle (Auto/Manual/Emergency)
 *           - Real-time Node-RED & OLED Dashboard
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <base64.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================
//  USER CONFIGURATION
// ============================================================
const char* WIFI_SSID      = "Jesin";       
const char* WIFI_PASSWORD  = "12345678@@";   
const char* MQTT_BROKER    = "192.168.1.100";  // Your PC IP (Mosquitto)
const int   MQTT_PORT      = 1883;

// Node Identity
const char* JUNCTION_ID    = "J1";
const char* CAM_IP         = "192.168.1.102";  // IP of the ESP32-CAM (Node-J1)

// Roboflow credentials
const char* RF_API_KEY     = "x4eWsXXfGZUxZ2PrVifr"; 
const char* RF_MODEL_ID    = "smartjunction-traffic/1"; 
// Note: Ensure your Roboflow model includes "ambulance" and "fire brigade" classes.

// Hardware Pins (Standard Dev Module)
#define SERIAL2_RX  16
#define SERIAL2_TX  17
#define RED_LED     25
#define YELLOW_LED  33
#define GREEN_LED   26
#define BUZZER_PIN  27
#define SERVO_PIN   14

// OLED
#define SCREEN_W   128
#define SCREEN_H   64
#define OLED_ADDR  0x3C

// Global Objects
WiFiClient espClient;
PubSubClient mqtt(espClient);
Servo barrierServo;
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// System State
bool emergencyActive = false;
unsigned long emergencyStart = 0;
unsigned long lastSignalChange = 0;
String signalState = "RED"; // RED, YELLOW, GREEN, EMERGENCY
int vehicleCount = 0;
int personCount = 0;
int megaDist = 0;
int megaCong = 0;
int megaIR   = 0;
int megaPIR  = 0;
bool manualOverride = false;
bool isAmbulance = false;
bool isFireTruck = false;
bool violationDetected = false;

// MQTT Topics (Node-RED Compatible)
const char* topicStatus = "smartjunction/J1/status";
const char* topicSensors = "smartjunction/J1/sensors";
const char* topicCmd = "smartjunction/J1/cmd";
const char* topicEmergency = "smartjunction/emergency";
const char* topicAlerts = "smartjunction/alerts";

// ────────────────────────────────────────────────────────────
//  Actuator Functions
// ────────────────────────────────────────────────────────────
void setSignal(String state) {
    if (emergencyActive && state != "EMERGENCY") return;
    
    signalState = (state == "EMERGENCY") ? "EMERGENCY" : state;
    lastSignalChange = millis();
    
    // Physical Signals
    digitalWrite(RED_LED, (state == "RED" ? HIGH : LOW));
    digitalWrite(YELLOW_LED, (state == "YELLOW" ? HIGH : LOW));
    digitalWrite(GREEN_LED, (state == "GREEN" || state == "EMERGENCY" ? HIGH : LOW));
    
    // Buzzer Logic
    if (state == "EMERGENCY") {
        // High frequency continuous for emergency
        analogWrite(BUZZER_PIN, 200); 
    } else if (state == "GREEN" && (megaPIR || megaIR)) {
        // Warning beep if pedestrians are nearby while shifting
        tone(BUZZER_PIN, 1000, 200);
    } else {
        noTone(BUZZER_PIN);
        analogWrite(BUZZER_PIN, 0);
    }
    
    if (state == "GREEN" || state == "EMERGENCY") {
        barrierServo.write(90); // Open
    } else {
        barrierServo.write(0);  // Close
    }
}

void reportViolation() {
    violationDetected = true;
    Serial.println(F("[SECURITY] TRAFFIC VIOLATION DETECTED"));
    
    StaticJsonDocument<128> doc;
    doc["source"] = JUNCTION_ID;
    doc["type"] = "VIOLATION";
    doc["details"] = "Vehicle/Pedestrian moved during RED signal";
    char buf[128];
    serializeJson(doc, buf);
    mqtt.publish(topicAlerts, buf);
    
    // Strobe buzzer
    for(int i=0; i<3; i++) {
        digitalWrite(BUZZER_PIN, HIGH); delay(100);
        digitalWrite(BUZZER_PIN, LOW); delay(100);
    }
}

void forceEmergency(String type) {
    if (emergencyActive && signalState == "EMERGENCY") return;
    
    emergencyActive = true;
    emergencyStart = millis();
    setSignal("EMERGENCY");
    Serial.printf("[ALERT] %s PRIORITY DETECTED\n", type.c_str());
    
    // Broadcast to Central Alert Feed
    StaticJsonDocument<128> doc;
    doc["source"] = JUNCTION_ID;
    doc["type"] = type; // "AMBULANCE" or "FIRE_TRUCK"
    char buf[128];
    serializeJson(doc, buf);
    mqtt.publish(topicEmergency, buf);
    
    if (type == "AMBULANCE") isAmbulance = true;
    if (type == "FIRE_TRUCK") isFireTruck = true;
}

void updateOLED() {
    display.clearDisplay();
    display.setCursor(0,0);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    display.printf("NODE: %s | %s\n", JUNCTION_ID, manualOverride ? "MAN" : "AI");
    display.printf("SIG: [%s]\n", signalState.c_str());
    display.println("--------------");
    
    // Visual Progress Bar for Signal
    int prog = map(millis() - lastSignalChange, 0, 10000, 0, 128);
    display.drawRect(0, 32, 128, 6, SSD1306_WHITE);
    display.fillRect(0, 32, constrain(prog, 0, 128), 6, SSD1306_WHITE);
    
    display.setCursor(0, 42);
    display.printf("LOAD: %d%% | V:%d\n", megaCong, vehicleCount);
    
    if (emergencyActive) {
        display.setCursor(0, 54);
        display.println("!! EMERGENCY !!");
    } else if (violationDetected) {
        display.setCursor(0, 54);
        display.println("!! VIOLATION !!");
    }
    
    display.display();
}

// ────────────────────────────────────────────────────────────
//  Vision Intelligence (Pull Architecture)
// ────────────────────────────────────────────────────────────
void callRoboflow() {
    if (WiFi.status() != WL_CONNECTED) return;
    
    HTTPClient http;
    String captureUrl = "http://" + String(CAM_IP) + "/capture";
    http.begin(captureUrl);
    http.setTimeout(3000);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        size_t len = http.getSize();
        WiFiClient* stream = http.getStreamPtr();
        uint8_t* buffer = (uint8_t*)malloc(len);
        if (buffer) {
            stream->readBytes(buffer, len);
            String b64 = base64::encode(buffer, len);
            free(buffer);
            http.end();

            String rfUrl = String("https://detect.roboflow.com/") + RF_MODEL_ID + "?api_key=" + RF_API_KEY + "&name=frame.jpg";
            http.begin(rfUrl);
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
            int rfCode = http.POST(b64);
            
            if (rfCode == 200) {
                String res = http.getString();
                StaticJsonDocument<2048> doc;
                if (!deserializeJson(doc, res)) {
                    int v = 0; 
                    String emType = "";
                    for (JsonObject pred : doc["predictions"].as<JsonArray>()) {
                        String cls = pred["class"].as<String>();
                        float conf = pred["confidence"].as<float>();
                        if (conf > 0.45) {
                            if (cls == "car" || cls == "bus" || cls == "truck" || cls == "vehicle") v++;
                            if (cls == "ambulance") emType = "AMBULANCE";
                            if (cls == "fire truck" || cls == "fire brigade") emType = "FIRE_TRUCK";
                        }
                    }
                    vehicleCount = v;
                    if (emType != "") forceEmergency(emType);
                }
            }
        }
    }
    http.end();
}

// ────────────────────────────────────────────────────────────
//  Logic & Networking
// ────────────────────────────────────────────────────────────
void handleMqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (int i=0; i<(int)length; i++) msg += (char)payload[i];
    
    if (String(topic) == topicCmd) {
        if (msg == "RED" || msg == "GREEN" || msg == "YELLOW") {
            manualOverride = true;
            setSignal(msg);
        } else if (msg == "AUTO") {
            manualOverride = false;
        } else if (msg == "ALERT_PATROL") {
            violationDetected = true; // Use flags to trigger visual warning
        }
    } else if (String(topic) == topicEmergency && msg.indexOf("CLEAR") != -1) {
        emergencyActive = false;
        violationDetected = false;
        isAmbulance = false;
        isFireTruck = false;
        setSignal("RED");
    }
}

void readMegaSensors() {
    if (Serial2.available()) {
        String data = Serial2.readStringUntil('\n');
        StaticJsonDocument<256> doc;
        if (!deserializeJson(doc, data)) {
            megaDist = doc["dist"];
            megaCong = doc["cong"];
            megaIR   = doc["ir"];
            megaPIR  = doc["pir"];
            
            // Security Logic: Movement during RED
            if (signalState == "RED" && megaPIR && !violationDetected) {
                reportViolation();
            }
        }
    }
}

void publishTelemetry() {
    StaticJsonDocument<512> doc;
    doc["junction"] = JUNCTION_ID;
    doc["vehicles"] = vehicleCount;
    doc["distance"] = megaDist;
    doc["traffic_score"] = megaCong;
    doc["ir"] = megaIR;
    doc["pir"] = megaPIR;
    doc["signal"] = signalState;
    doc["emergency"] = emergencyActive;
    doc["violation"] = violationDetected;
    doc["uptime"]    = millis() / 1000;
    
    char buf[512];
    serializeJson(doc, buf);
    mqtt.publish(topicSensors, buf);
}

// ────────────────────────────────────────────────────────────
//  Setup & Loop
// ────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial2.begin(115200, SERIAL_8N1, SERIAL2_RX, SERIAL2_TX);
    
    pinMode(RED_LED, OUTPUT);
    pinMode(GREEN_LED, OUTPUT);
    pinMode(YELLOW_LED, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    barrierServo.attach(SERVO_PIN);
    
    Wire.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println(F("OLED failed"));
    } else {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.println("SMARTJUNCTION AI");
        display.println("BOOTING...");
        display.display();
    }

    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(handleMqttCallback);
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        delay(500);
        return;
    }

    if (!mqtt.connected()) {
        if (mqtt.connect(String("Brain-" + String(JUNCTION_ID)).c_str())) {
            mqtt.subscribe(topicCmd);
            mqtt.subscribe(topicEmergency);
            mqtt.publish(topicStatus, "online");
        }
    }
    mqtt.loop();

    readMegaSensors();

    // Signal Logic (Adaptive Cycle)
    if (!manualOverride && !emergencyActive) {
        static unsigned long lastCycle = 0;
        unsigned long greenTime = 8000;
        unsigned long redTime   = 8000;
        
        // AI Weighting: Increase green duration if high congestion
        if (megaCong > 60) greenTime = 15000;
        if (megaCong > 85) greenTime = 25000;
        
        // Pedestrian Hold: If PIR active, don't switch from GREEN to YELLOW yet
        bool pedWait = (signalState == "GREEN" && (megaPIR || megaIR));

        if (millis() - lastCycle > (signalState == "GREEN" ? greenTime : redTime) && !pedWait) {
            if (signalState == "RED") setSignal("GREEN");
            else if (signalState == "GREEN") setSignal("YELLOW");
            else if (signalState == "YELLOW") setSignal("RED");
            lastCycle = millis();
        }
    }

    // Vision Analysis (Smart Throttle)
    static unsigned long lastAI = 0;
    int aiInterval = emergencyActive ? 2000 : 6000; 
    if (millis() - lastAI > aiInterval) {
        callRoboflow();
        lastAI = millis();
    }

    // Clear violation after some time
    if (violationDetected && (millis() - lastSignalChange > 15000)) {
        violationDetected = false;
    }

    // Emergency Timeout (safety)
    if (emergencyActive && (millis() - emergencyStart > 45000)) {
        emergencyActive = false;
        setSignal("RED");
    }

    // Performance UI
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 1000) {
        publishTelemetry();
        updateOLED();
        lastLog = millis();
    }
}
