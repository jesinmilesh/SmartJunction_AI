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
const char* RF_MODEL_ID    = "smartjunction-traffic/3"; 
// Note: Ensure your Roboflow model includes "ambulance" and "fire truck" classes.

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
const char* topicVision = "smartjunction/J1/vision"; // AI data from Server
const char* topicEmergency = "smartjunction/emergency";
const char* topicAlerts = "smartjunction/alerts";

// ────────────────────────────────────────────────────────────
//  Actuator Functions
// ────────────────────────────────────────────────────────────
void setSignal(String state) {
    if (emergencyActive && state != "EMERGENCY") return;
    
    signalState = (state == "EMERGENCY") ? "EMERGENCY" : state;
    lastSignalChange = millis();
    
    // Signal Indication LEDs
    digitalWrite(RED_LED, (state == "RED" ? HIGH : LOW));
    digitalWrite(YELLOW_LED, (state == "YELLOW" ? HIGH : LOW));
    digitalWrite(GREEN_LED, (state == "GREEN" || state == "EMERGENCY" ? HIGH : LOW));
    
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

// Vision Intelligence handled by Edge Server via MQTT for maximum speed

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
            violationDetected = true; 
        }
    } else if (String(topic) == topicVision) {
        // High-speed Vision data from Edge Server
        StaticJsonDocument<512> doc;
        if (!deserializeJson(doc, msg)) {
            vehicleCount = doc["vehicles"] | 0;
            personCount  = doc["persons"] | 0;
            megaCong     = doc["traffic_score"] | 0;
            
            if (doc["ambulance"]) forceEmergency("AMBULANCE");
            if (doc["fire_truck"]) forceEmergency("FIRE_TRUCK");
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
            mqtt.subscribe(topicVision);
            mqtt.subscribe(topicEmergency);
            mqtt.publish(topicStatus, "online");
        }
    }
    mqtt.loop();

    readMegaSensors();

    // Signal Logic (Neural-Adaptive Pulse)
    if (!manualOverride && !emergencyActive) {
        static unsigned long lastCycle = 0;
        unsigned long greenTime = 8000;
        unsigned long redTime   = 8000;
        
        // --- AI Congestion Analysis ---
        // We use the higher value from either physical ultrasonic sensors or AI camera feed
        int currentCong = (vehicleCount * 10 > megaCong) ? (vehicleCount * 10) : megaCong;

        // 🟢 Green Duration Logic: Extend if AI sees many vehicles
        if (currentCong > 50) greenTime = 15000;
        if (currentCong > 85) greenTime = 30000; // Extra long green for congestion clearance
        
        // 🔴 Red Acceleration Logic: Switch to green faster if queue is large
        if (currentCong > 70 && signalState == "RED") {
            redTime = 3000; // Force-clear red lane after only 3s if jammed
        }

        // Pedestrian Hold Protocol
        bool pedWait = (signalState == "GREEN" && (megaPIR || megaIR || personCount > 0));

        if (millis() - lastCycle > (signalState == "GREEN" ? greenTime : redTime) && !pedWait) {
            if (signalState == "RED") {
                setSignal("GREEN");
                Serial.println("[AI] Queue detected — Switching to GREEN");
            }
            else if (signalState == "GREEN") setSignal("YELLOW");
            else if (signalState == "YELLOW") setSignal("RED");
            lastCycle = millis();
        }
    }

    // Vision Analysis handled via MQTT (Server-Driven)

    // Clear violation after some time
    if (violationDetected && (millis() - lastSignalChange > 15000)) {
        violationDetected = false;
    }

    // Emergency Timeout (safety)
    if (emergencyActive && (millis() - emergencyStart > 45000)) {
        emergencyActive = false;
        setSignal("RED");
    }

    // --- Acoustic Navigation Protocol (Buzzer) ---
    if (emergencyActive) {
        if (isAmbulance) {
            // Rapid Hi-Lo Siren (Ambulance)
            tone(BUZZER_PIN, (millis() % 400 < 200) ? 1100 : 800);
        } else if (isFireTruck) {
            // Broad Frequency Sweep (Fire Truck)
            int sweep = 400 + (millis() % 1000) * 1.5;
            tone(BUZZER_PIN, sweep > 1500 ? 1500 : sweep);
        } else {
            tone(BUZZER_PIN, 1000); 
        }
    } else if (signalState == "GREEN" && (megaPIR || megaIR)) {
        // Warning beacon for near-miss pedestrian detection
        if (millis() % 2000 < 100) tone(BUZZER_PIN, 1200);
        else noTone(BUZZER_PIN);
    } else {
        noTone(BUZZER_PIN);
    }

    // Performance UI
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 1000) {
        publishTelemetry();
        updateOLED();
        lastLog = millis();
    }
}
