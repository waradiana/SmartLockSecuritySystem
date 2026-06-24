// Project  : ESP32 Door Lock Unit
// SoftAP   : DoorUnit-Setup
// Board    : ESP32 versi 3.3.3

#include <WiFi.h>               
#include <WiFiClientSecure.h>  
#include <WiFiManager.h>        // 2.0.17
#include <PubSubClient.h>       // 2.8.0
#include <Adafruit_GFX.h>       // 1.12.4
#include <Adafruit_SSD1306.h>   // 2.5.16

// ==================== PIN CONFIG ====================
#define DOOR_SENSOR_PIN 4
#define EXIT_BTN 5
#define RELAY_PIN 33
#define RESET_BTN 13

// Oled display
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==================== MQTT CONFIG ====================
const char* mqtt_server ="0c82a64bb24146519d9b6df720d08792.s1.eu.hivemq.cloud";
const int mqtt_port     = 8883;
const char* mqtt_user   = "SmartLockSystem";
const char* mqtt_pass   = "2FactorAuthentication";

WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

// --------------------- PUBLISH ---------------------
const char* topic_door_status = "switch/door_status";   // status door switch dari ESP32_DoorUnit
const char* topic_exit_button = "switch/exit_button";   // konfirmasi exit button
// --------------------- SUBSCRIBE ---------------------
const char* topic_door_lock   = "solenoid/door_unlock"; // perintah buka pintu ke ESP32_DoorUnit

// =================== GLOBAL VARIABLES ===================
// MQTT status
bool mqttReady = false;
unsigned long lastMqttReconnectAttempt = 0;

// WiFi state
bool wifiPortalActive = false;
bool wifiWasConnected = false;

// Relay
bool relayActive = false;
unsigned long relayStartTime = 0;
const unsigned long relayDuration = 5000;

// Door switch
int lastDoorState                   = -1;
unsigned long lastDoorReport        = 0;

// Button exit
unsigned long lastButtonPress       = 0;

// Button reset WiFi
bool wifiResetBtnLastReading = HIGH;
bool wifiResetBtnStableState = HIGH;
unsigned long wifiResetBtnLastDebounceMs = 0;
unsigned long wifiResetBtnPressedMs = 0;
bool wifiResetTriggered = false;
const unsigned long WIFI_RESET_DEBOUNCE_MS = 50;
const unsigned long WIFI_RESET_HOLD_MS = 3000;

// OLED anti flicker
String lastL1 = "", lastL2 = "";

// ================ FUNCTION PROTOTYPES ================
void wifiConnect();
void startReconnectPortal();
void resetWiFi();
void handleWiFiReset();
void mqttReconnectNonBlocking();
void mqttCallback(char* topic, byte* payload, unsigned int length); 
void shortDelay(unsigned long ms);
void uiSet(const String& l1, const String& l2);

// ==================== UTILITIES ====================
void shortDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        if (mqttClient.connected()) mqttClient.loop();
        yield();
    }
}

void uiSet(const String& l1, const String& l2) {
    if (l1 == lastL1 && l2 == lastL2) return;

    lastL1 = l1;
    lastL2 = l2;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    int16_t x1, y1;
    uint16_t w, h;

    display.getTextBounds(l1, 0, 0, &x1, &y1, &w, &h);
    int xTop = (SCREEN_WIDTH - w) / 2;
    if (xTop < 0) xTop = 0;
    display.setCursor(xTop, 6);
    display.println(l1);

    display.getTextBounds(l2, 0, 0, &x1, &y1, &w, &h);
    int xBot = (SCREEN_WIDTH - w) / 2;
    if (xBot < 0) xBot = 0;
    display.setCursor(xBot, 25);
    display.println(l2);

    display.display();
}

// ==================== WIFI CONFIG ====================
void wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return;
    if (wifiPortalActive) return;

    Serial.println("\n[WiFiManager] Memulai konfigurasi WiFi...");
    wifiPortalActive = true;

    WiFiManager wm;
    wm.setConfigPortalTimeout(180); // 3 menit
    wm.setDebugOutput(true);

    wm.setAPCallback([](WiFiManager *wmPtr) {
        uiSet("WIFI SETUP", "AP: DoorUnit-Setup");
    });

    uiSet("Memulai Sistem", "Menunggu WiFi");

    bool res = wm.autoConnect("DoorUnit-Setup"); // AP Name  :DoorUnit-Setup
    wifiPortalActive = false;

    if (!res) {
        Serial.println("[WiFiManager] Gagal terhubung / timeout!");
        uiSet("WIFI FAILED", "Restart Perangkat");
        delay(1000);
        ESP.restart();
    } else {
        Serial.println("[WiFiManager] WiFi terhubung!");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        shortDelay(50);
    }
}

void startReconnectPortal() {
    if (wifiPortalActive) return;

    Serial.println("[WiFi] Koneksi terputus -> membuka mode setup");
    wifiPortalActive = true;

    mqttClient.disconnect();
    mqttReady = false;

    uiSet("WIFI RECONNECT", "AP: DoorUnit-Setup");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setDebugOutput(true);

    wm.setAPCallback([](WiFiManager *wmPtr) {
        uiSet("WIFI RECONNECT", "AP: DoorUnit-Setup");
    });

    bool res = wm.startConfigPortal("DoorUnit-Setup");
    wifiPortalActive = false;

    if (res && WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] Berhasil reconnect dari config portal");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        wifiWasConnected = true;
    } else {
        Serial.println("[WiFi] Reconnect portal selesai tanpa koneksi, restart...");
        uiSet("WIFI FAILED", "Restart Device");
        shortDelay(1200);
        ESP.restart();
    }
}

void resetWiFi() {
    if (wifiPortalActive) return;

    Serial.println("[WiFi] Reset WiFi -> hapus kredensial");
    uiSet("RESET WIFI", "AP: DoorUnit-Setup");

    mqttClient.disconnect();
    mqttReady = false;

    WiFiManager wm;
    wm.resetSettings();
    WiFi.disconnect(true, true);

    wifiWasConnected = false;

    shortDelay(1000);

    wifiPortalActive = true;
    wm.setConfigPortalTimeout(180);
    wm.setDebugOutput(true);

    wm.setAPCallback([](WiFiManager *wmPtr) {
        uiSet("RESET WIFI", "AP: DoorUnit-Setup");
    });

    bool res = wm.startConfigPortal("DoorUnit-Setup");
    wifiPortalActive = false;

    if (res && WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] WiFi baru berhasil dikonfigurasi");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        wifiWasConnected = true;
    } else {
        Serial.println("[WiFi] Portal reset selesai tanpa koneksi, restart...");
        uiSet("WIFI FAILED", "Restart Device");
        shortDelay(1200);
        ESP.restart();
    }
}

void handleWiFiReset() {
    bool reading = digitalRead(RESET_BTN);

    if (reading != wifiResetBtnLastReading) {
        wifiResetBtnLastDebounceMs = millis();
        wifiResetBtnLastReading = reading;
    }

    if ((millis() - wifiResetBtnLastDebounceMs) > WIFI_RESET_DEBOUNCE_MS) {
        if (reading != wifiResetBtnStableState) {
            wifiResetBtnStableState = reading;

            if (wifiResetBtnStableState == LOW) {
                wifiResetBtnPressedMs = millis();
                wifiResetTriggered = false;
            } else {
                wifiResetBtnPressedMs = 0;
                wifiResetTriggered = false;
            }
        }
    }

    if (WiFi.status() == WL_CONNECTED &&
        wifiResetBtnStableState == LOW &&
        !wifiResetTriggered &&
        wifiResetBtnPressedMs > 0) {

        if (millis() - wifiResetBtnPressedMs >= WIFI_RESET_HOLD_MS) {
            wifiResetTriggered = true;
            resetWiFi();
        }
    }
}

// ==================== MQTT CONFIG ====================
void mqttReconnectNonBlocking() {
    if (mqttClient.connected()) return;
    if (millis() - lastMqttReconnectAttempt < 2000) return;
    lastMqttReconnectAttempt = millis();

    String clientId = "ESP32_DoorUnit";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("[MQTT] Menghubungkan ke HiveMQ Cloud...");
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println("Terhubung ✅");
        mqttReady = true;

        mqttClient.subscribe(topic_door_lock);
        mqttClient.setCallback(mqttCallback);
    } else {
        Serial.print("Gagal rc=");
        Serial.println(mqttClient.state());
        mqttReady = false;
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();

    Serial.print("[MQTT]  ");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(msg);

    // Kontrol relay + solenoid lock
    if (String(topic) == topic_door_lock && msg == "open") {
        digitalWrite(RELAY_PIN, HIGH);
        relayActive = true;
        relayStartTime = millis();
        uiSet("DOOR EXIT", "Pintu Terbuka");
    }
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(9600);

    pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
    pinMode(EXIT_BTN, INPUT_PULLUP);
    pinMode(RESET_BTN, INPUT_PULLUP);

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    // Display
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED tidak terdeteksi!");
    } else {
        uiSet("Memulai Sistem", "Menunggu WiFi"); 
    }

    wifiConnect();
    wifiWasConnected = (WiFi.status() == WL_CONNECTED);

    mqttSecureClient.setInsecure();
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
}

// ==================== MAIN LOOP ====================
void loop() {
    handleWiFiReset();

    if (!wifiPortalActive) {
        wifiConnect();
    }

    // MQTT reconnect & loop
    if (WiFi.status() == WL_CONNECTED) {
        mqttReconnectNonBlocking();
        if (mqttClient.connected()) {
            mqttReady = true;
            mqttClient.loop();
        } else {
            mqttReady = false;
        }
    } else {
        mqttReady = false;
    }

    // OLED + WiFi state
    if (WiFi.status() == WL_CONNECTED) {
        wifiWasConnected = true;

        if (!mqttReady) {
            uiSet("WIFI CONNECTED", "Menunggu MQTT...");
        } else if (!relayActive) {
            uiSet("SYSTEM READY", "PUSH TO EXIT");
        }
    } else {
        if (wifiWasConnected && !wifiPortalActive) {
            startReconnectPortal();
        }
    }

    // Relay Timer
    if (relayActive && millis() - relayStartTime >= relayDuration) {
        digitalWrite(RELAY_PIN, LOW);
        relayActive = false;
        Serial.println("[Relay] Auto OFF");
        uiSet("SYSTEM READY", "PUSH TO EXIT");
    }

    // Door Switch Sensor
    int doorState = digitalRead(DOOR_SENSOR_PIN);
    if (doorState != lastDoorState && millis() - lastDoorReport > 100) {
        lastDoorState = doorState;
        lastDoorReport = millis();

        if (doorState == HIGH) {
            Serial.println("Door Open");
            mqttClient.publish(topic_door_status, "open");
        } else {
            Serial.println("Door Closed");
            mqttClient.publish(topic_door_status, "closed");
        }
    }

    // Button Exit
    if (digitalRead(EXIT_BTN) == LOW) {
        if (millis() - lastButtonPress > 300) {
            Serial.println("[Button] Manual OPEN");
            mqttClient.publish(topic_exit_button, "exit");
            digitalWrite(RELAY_PIN, HIGH);
            relayActive = true;
            relayStartTime = millis();
            uiSet("DOOR EXIT", "Pintu Terbuka");
            lastButtonPress = millis();
        }
    }
    shortDelay(5);
}