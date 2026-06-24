// Project  : ESP32 Authetication Unit
// SoftAP   : AuthUnit-Setup
// Board    : ESP32 versi 3.3.3

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>                // 1.4.12
#include <Adafruit_GFX.h>           // 1.12.4
#include <Adafruit_SSD1306.h>       // 2.5.16
#include <Adafruit_Fingerprint.h>   // 2.1.4
#include <EEPROM.h>                 //
#include <WiFiManager.h>            // 2.0.17
#include <HTTPClient.h>             // 0.6.1
#include <time.h>       
#include <UniversalTelegramBot.h>   // 1.3.0
#include <PubSubClient.h>           // 2.8.0
#include <ArduinoJson.h>            // 0.2.0

// ==================== PIN CONFIG ====================
// RFID (SPI)
#define SS_PIN  4
#define RST_PIN 15
MFRC522 rfid(SS_PIN, RST_PIN);

// Buzzer
#define BUZZER_PIN 26

// Button reset WiFi
#define RESET_BTN 13

// Oled display
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// =============== Fingerprint LED control values ===============
#define FINGERPRINT_LED_RED   0x01
#define FINGERPRINT_LED_BLUE  0x06
#define FINGERPRINT_LED_GREEN 0x04

HardwareSerial mySerial(2);
Adafruit_Fingerprint finger(&mySerial);

// ==================== EEPROM CONFIG ====================
#define EEPROM_SIZE  4096
#define NAME_LENGTH  20
#define UID_LENGTH   16
#define RECORD_SIZE  (NAME_LENGTH + UID_LENGTH)
#define MAX_ID       100
#define RECORD_START 1

// ==================== FIREBASE CONFIG ====================
#define FIREBASE_URL    "https://smart-lock-system-60262-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH   "z2C4gDGbwxk82ObL0BRok8pFqHmijmKUMqvCaAZI"
#define NODE_LOG        "log"
#define NODE_USERS      "users"
#define NODE_DOOR_STS   "door_status"

// ==================== TELEGRAM CONFIG ====================
#define BOT_TOKEN_ADMIN "8419291391:AAFqxD-k1n9-TMleOZVKR1a2BXbFN0mMzCE"
#define BOT_TOKEN_LOG   "7893459259:AAGAwjOYaLDlPb6yiS51q9kmNX53cti5yHk"
#define CHAT_ID         "8385631359"

WiFiClientSecure client;
UniversalTelegramBot botAdmin(BOT_TOKEN_ADMIN, client);
UniversalTelegramBot botLog(BOT_TOKEN_LOG, client);

// ==================== MQTT CONFIG ====================
const char* mqtt_server = "0c82a64bb24146519d9b6df720d08792.s1.eu.hivemq.cloud";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "SmartLockSystem";
const char* mqtt_pass   = "2FactorAuthentication";

WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);

// ----------------------- PUBLISH ------------------------
const char* topic_stream_cam     = "camera/start_stream";   // perintah start stream ke ESP32-CAM
const char* topic_enroll_face    = "camera/enroll_face";    // perintah mode enroll ke ESP32-CAM
const char* topic_delete_face    = "camera/delete_face";    // perintah hapus face ID di ESP32-CAM
const char* topic_door_lock      = "solenoid/door_unlock";  // perintah buka pintu ke ESP32_DoorUnit
// ---------------------- SUBSCRIBE -----------------------
const char* topic_ip_cam         = "camera/ip_address";     // IP kamera ke ESP32-CAM
const char* topic_enroll_confirm = "camera/enroll_confirm"; // konfirmasi enroll wajah selesai dari ESP32-CAM
const char* topic_face_id        = "camera/face_id";        // hasil face recognition dari ESP32-CAM
const char* topic_door_status    = "switch/door_status";    // status door switch dari ESP32_DoorUnit
const char* topic_exit_button    = "switch/exit_button";    // konfirmasi exit button


// ==================== GLOBAL VARIABLES ====================
// Sinkron time NTP
bool timeSynced = false;

// MQTT status
bool mqttReady = false;
unsigned long lastMqttReconnectAttempt = 0;

// Button reset WiFi
bool wifiResetBtnLastReading = HIGH;
bool wifiResetBtnStableState = HIGH;
unsigned long wifiResetBtnLastDebounceMs = 0;
unsigned long wifiResetBtnPressedMs = 0;
const unsigned long WIFI_RESET_DEBOUNCE_MS = 50;
const unsigned long WIFI_RESET_HOLD_MS = 3000; // 3 detik
bool wifiResetTriggered = false;
bool wifiPortalActive = false;
bool wifiWasConnected = false;

// Status door switch
bool authorizedOpen = false;           // true jika pintu dibuka oleh akses sah
String lastSwitchStatus = "";          // status pintu terakhir dari sensor
unsigned long authorizedTimer = 0;     // timer durasi authorizedOpen
const unsigned long AUTH_DOOR = 10000; // durasi pintu dianggap "legal open" setelah autentikasi

// Telegram polling
long lastUpdateID = 0;
unsigned long lastTimeBotCheck = 0;
const unsigned long BOT_CHECK_INTERVAL = 1000;

// Scan interval (fingerprint & RFID)
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL_MS = 80;

// LED update timestamp
unsigned long lastLedUpdate = 0;

// 2FA Session (Fingerprint/RFID -> Face)
bool waitingForFace = false;                // sedang menunggu face response
String sessionMethod = "";                  // metode faktor pertama
int sessionAuthID = 0;                      // ID user yang lolos faktor pertama
String sessionUID = "";                     // UID yang yang lolos faktor pertama
String sessionNonce = "";                   // nonce untuk mencegah replay
String cameraIP = "";                       // IP camera
unsigned long sessionStartTime = 0;
const unsigned long SESSION_TIMEOUT = 15000;

// Enrollment state machine via Telegram
enum EnrollmentState {
    STATE_IDLE,                 // mode scan RFID/Fingerprint
    STATE_WAITING_NAME,         // tunggu nama dari Telegram
    STATE_WAITING_FP_1,         // tunggu scan fingerprint tahap 1
    STATE_WAITING_FP_2,         // tunggu scan fingerprint tahap 2
    STATE_WAITING_RFID_CARD,    // tunggu tap kartu RFID
    STATE_WAITING_FACE_ENROLL,  // Menunggu enroll wajah selesai (MQTT enroll_confirm)
    STATE_WAITING_DELETE_ID     // tunggu input ID untuk delete
};
EnrollmentState currentEnrollState = STATE_IDLE;
uint8_t currentEnrollID = 0;
String currentEnrollUID = "";
String currentEnrollName = "";
String currentAdminChatID = "";
String currentAdminChatID_Delete = "";

// Enrollment timeout
unsigned long enrollStartTime = 0;
const unsigned long ENROLL_TIMEOUT = 30000;

// Merapikan UI
enum UiMode {
    UI_BOOT,
    UI_READY,
    UI_WIFI_WAIT,
    UI_MQTT_WAIT,
    UI_AUTH1_OK,
    UI_FACE_WAIT,
    UI_GRANTED,
    UI_DENIED,
    UI_TIMEOUT,
    UI_ENROLL
};
UiMode uiMode = UI_BOOT;

struct UiLock {
    bool locked = false;        // jika true, standby tidak menimpa display
    unsigned long untilMs = 0;  // auto-unlock setelah waktu ini
} uiLock;

String lastOledL1="", lastOledL2="", lastOledL3="", lastOledL4 = ""; // anti-flicker OLED

// Telegram anti-spam (NEW)
unsigned long lastTgSentMs = 0;
String lastTgHash = "";
const unsigned long TG_MIN_INTERVAL_MS = 2500; // minimal jeda antar pesan yg duplikat


// ==================== FUNCTION PROTOTYPES ====================
// WiFi & time
void wifiConnect();
void setupTime();
void resetWiFi();
void handleWiFiReset();
void startReconnectPortal();
String getTimeString();
String getTimestampKey();

// MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnectNonBlocking();

// Firebase
void fbPUT(const String& path, const String& json);
void fbDELETE(const String& path);
void fbLog(uint8_t id, const String& name, const String& uid, const String& method, const String& eventType);
String getLogsFromFirebase(int limit);
String escapeJsonString(String s);

// EEPROM
void saveUserToEEPROM(uint8_t id, const String& name, const String& uid);
String readNameFromEEPROM(uint8_t id);
String readUIDFromEEPROM(uint8_t id);

// Display
void ledStandby();
void ledSuccessful();
void ledFailed();
void ledAccessValid();
void centerDisplay(const String& text, int y);
void updateDisplay(const String& line1, const String& line2, const String& line3, const String& line4);
void uiSet(const String& l1, const String& l2, const String& l3, const String& l4, UiMode mode, unsigned long holdMs=0);
void uiTick();
void uiSetStandby();

// Users
int findEmptyID();
void deleteUser(uint8_t id, bool viaTelegram);

// Enroll timeout
void startEnrollState(EnrollmentState st);
void cancelEnrollment(const String& reason);
void finishEnrollment();

// Scan
void scanFingerprint();
void scanRFID();

// Telegram handlers
void handleNewMessages(int numNewMessages);
String getTelegramCommand(const String& text);
String listUsersTelegram();
void handleLogBot();
void tgSendLimited(const String& msg, const String& parseMode="Markdown");

// Enrollment step
void enrollStepFP1();
void enrollStepFP2();
void enrollRFID(uint8_t id, const String& name, const String& uid, const String& chat_id);

// Alarm
void triggerAlarm();

// Utilities base
void shortDelay(unsigned long ms);
String generateNonce();

// ==================== UTILITIES ====================
String generateNonce() {
  String nonce = "";
  for(int i=0;i<8;i++){
    nonce += String(random(0,16), HEX);
  }
  nonce.toUpperCase();
  return nonce;
}

// shortDelay, mengatasi delay panjang
void shortDelay(unsigned long ms) {
    unsigned long start = millis();
    while (millis() - start < ms) {
        if (mqttClient.connected()) mqttClient.loop();
        yield();
    }
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

    // tampilkan UI setup HANYA saat portal aktif
    wm.setAPCallback([](WiFiManager *wm){
        uiSet("WIFI SETUP", "Hubungkan WiFi", "ke Access Point", "AuthUnit-Setup", UI_WIFI_WAIT, 0);
    });

    // saat boot normal, cukup tampilkan menunggu WiFi
    uiSet("", "Memulai Sistem", "Menunggu WiFi", "", UI_BOOT, 0);

    bool res = wm.autoConnect("AuthUnit-Setup");  // AP Name  : AuthUnit-Setup

    wifiPortalActive = false;

    if (!res) {
        Serial.println("[WiFiManager] Gagal terhubung / timeout!");
        uiSet("WIFI FAILED", "Portal Ditutup", "Restart Perangkat", "", UI_WIFI_WAIT, 1200);
        shortDelay(500);
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

    Serial.println("[WiFi] Koneksi Terputus -> membuka kembali mode setup");
    wifiPortalActive = true;

    // batalkan jika ada sesi aktif
    waitingForFace = false;
    sessionAuthID = 0;
    sessionUID = "";
    sessionNonce = "";
    sessionMethod = "";
    sessionStartTime = 0;

    mqttClient.disconnect();
    mqttReady = false;

    uiSet("WIFI RECONNECT", "WiFi Tidak Tersedia", "Mode Setup", "AuthUnit-Setup", UI_WIFI_WAIT, 0);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);

    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setDebugOutput(true);

    bool res = wm.startConfigPortal("AuthUnit-Setup");

    wifiPortalActive = false;

    if (res && WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] Berhasil reconnect dari config portal");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());

        wifiWasConnected = true;
        setupTime();
    } else {
        Serial.println("[WiFi] Reconnect portal selesai tanpa koneksi, restart...");
        uiSet("WIFI FAILED", "Portal Ditutup", "Restart Perangkat", "", UI_WIFI_WAIT, 1200);
        shortDelay(1200);
        ESP.restart();
    }
}

void resetWiFi() {
    if (wifiPortalActive) return;

    Serial.println("[WiFi] Tombol reset ditekan -> hapus kredensial & kembali ke SoftAP");
    uiSet("RESET WIFI", "Kembali ke", "Mode Setup", "AuthUnit-Setup", UI_WIFI_WAIT, 1000);

    // batalkan jika ada sesi aktif
    waitingForFace = false;
    sessionAuthID = 0;
    sessionUID = "";
    sessionNonce = "";
    sessionMethod = "";
    sessionStartTime = 0;

    mqttClient.disconnect();
    mqttReady = false;

    WiFiManager wm;
    wm.resetSettings();          // hapus SSID & pass
    WiFi.disconnect(true, true); // putuskan koneksi & hapus config dari WiFi driver

    wifiWasConnected = false;

    shortDelay(1000);

    wifiPortalActive = true;
    bool res = wm.startConfigPortal("AuthUnit-Setup");
    wifiPortalActive = false;

    if (res && WiFi.status() == WL_CONNECTED) {
        Serial.println("[WiFi] WiFi baru berhasil dikonfigurasi");
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());

        wifiWasConnected = true;
        setupTime();
    } else {
        Serial.println("[WiFi] Portal reset selesai tanpa koneksi, restart...");
        uiSet("WIFI GAGAL", "Portal Ditutup", "Restart Perangkat", "", UI_WIFI_WAIT, 1200);
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
                Serial.println("[WiFi] Tombol reset mulai ditekan");
            } else {
                wifiResetBtnPressedMs = 0;
                wifiResetTriggered = false;
            }
        }
    }
    // hanya reset kalau tombol ditahan sesuai waktu dan WiFi sedang tersambung
    if (WiFi.status() == WL_CONNECTED && wifiResetBtnStableState == LOW && !wifiResetTriggered && wifiResetBtnPressedMs > 0) {
        unsigned long pressedFor = millis() - wifiResetBtnPressedMs;

        if (pressedFor >= WIFI_RESET_HOLD_MS) {
            wifiResetTriggered = true;
            resetWiFi();
        }
    }
}

// ==================== TIME/NTP ====================
void setupTime() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TIME] WiFi belum terhubung, tunda sinkron waktu...");
        return;
    }
    Serial.print("[TIME] Sinkron NTP WITA...");
    configTime(8 * 3600, 0, "pool.ntp.org", "time.google.com");

    for (int i = 0; i < 15; i++) {
        time_t now = time(nullptr);
        if (now > 1700000000) {
            timeSynced = true;
            Serial.println(" OK ✅");
            return;
        }
        Serial.print(".");
        shortDelay(500);
    }
    timeSynced = false;
    Serial.println(" GAGAL ❌ (fallback aktif)");
}

String getTimeString() {
    if (!timeSynced) return String("fallback_") + String(millis());
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return String("fallback_") + String(millis());
    char buf[40];
    strftime(buf, sizeof(buf), "%d-%m-%Y %H:%M:%S", &timeinfo);
    return String(buf) + " WITA";
}

String getTimestampKey() {
    if (!timeSynced) return String("fallback_") + String(millis());
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return String("fallback_") + String(millis());
    char buf[40];
    strftime(buf, sizeof(buf), "%d-%m-%Y|%H:%M:%S", &timeinfo);
    return String(buf);
}

// ==================== MQTT CONFIG ====================
void mqttReconnectNonBlocking() {
    if (mqttClient.connected()) return;
    if (millis() - lastMqttReconnectAttempt < 2000) return;
    lastMqttReconnectAttempt = millis();

    String clientId = "ESP32_AuthUnit";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

    Serial.print("[MQTT] Menghubungkan ke HiveMQ Cloud...");
    if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
        Serial.println("Terhubung ✅");
        mqttReady = true;

        mqttClient.subscribe(topic_door_status);
        mqttClient.subscribe(topic_ip_cam);
        mqttClient.subscribe(topic_enroll_confirm);
        mqttClient.subscribe(topic_face_id);
        mqttClient.subscribe(topic_exit_button);

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

    Serial.print("[MQTT] ");
    Serial.print(topic);
    Serial.print(" => ");
    Serial.println(msg);

    // Link enroll face
    if (String(topic) == topic_ip_cam) {
        cameraIP = msg;
        cameraIP.trim();
        Serial.println("[CAM] Akses kamera http://" + cameraIP);
        return;
    }

    // Confirm enroll face selesai
    if (String(topic) == topic_enroll_confirm && msg == "Enroll_Face_Finished") {
        if (currentEnrollState == STATE_WAITING_FACE_ENROLL) {
        finishEnrollment();
        }
        return;
    }

    // Respon face recognition untuk 2FA
    if(String(topic) == topic_face_id) {
        if (currentEnrollState != STATE_IDLE) {
            Serial.println("[2FA] Abaikan face_id (sedang enrollment)");
            return;
        }

        if(!waitingForFace) {
            Serial.println("[2FA] Tidak ada sesi aktif");
            return;
        }

        if(millis() - sessionStartTime > SESSION_TIMEOUT){
            Serial.println("[2FA] Sesi timeout");
            waitingForFace = false;
            return;
        }

        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, msg);

        if(error){
            Serial.println("[2FA] Gagal parse JSON");
            return;
        }

        int faceID = doc["face_id"];
        String receivedNonce = doc["nonce"].as<String>();

        Serial.printf("SessionID:%d | FaceID:%d\n", sessionAuthID, faceID);
        Serial.println("Expected Nonce: " + sessionNonce);
        Serial.println("Received Nonce: " + receivedNonce);

        if(receivedNonce != sessionNonce){
            Serial.println("[SECURITY] Nonce tidak cocok! Potensi replay attack.");
            waitingForFace = false;
            return;
        }

        String name = readNameFromEEPROM(sessionAuthID);
        if(faceID == sessionAuthID){
            Serial.println("[2FA] SUKSES -> buka pintu");
            mqttClient.publish(topic_door_lock, "open");
            authorizedOpen = true;
            authorizedTimer = millis();
            
            ledAccessValid();
            uiSet("AKSES DITERIMA", "Welcome", name, "Pintu Terbuka", UI_GRANTED, 4500);

            if (WiFi.status() == WL_CONNECTED) {
                fbLog(sessionAuthID, name, sessionUID, sessionMethod, "access_granted");
            } else {
                Serial.println("[FB] offline, skipping denied log");
            }
        } else {
            Serial.println("[2FA] GAGAL -> wajah tidak cocok");
            ledFailed();
            uiSet("AKSES DITOLAK", name, "Verifikasi Wajah", "Tidak Cocok", UI_DENIED, 4500);

            if (WiFi.status() == WL_CONNECTED) {
                fbLog(sessionAuthID, name, sessionUID, sessionMethod, "face_mismatch");
            } else {
                Serial.println("[FB] offline, skipping denied log");
            }          
        }
        waitingForFace = false;
        return;
    }

    // Door status
    if (String(topic) == topic_door_status) {
        lastSwitchStatus = msg;
        Serial.print("[DOOR] Status: ");
        Serial.println(msg);

        if (WiFi.status() == WL_CONNECTED) {
            String jsonStatus = "\"" + msg + "\"";
            fbPUT(NODE_DOOR_STS, jsonStatus);
        }

        if (msg == "open") {
            if (authorizedOpen) {
                Serial.println("[DOOR] Terbuka dengan akses sah");
            } else {
                Serial.println("[WARNING] Pintu terbuka tanpa autentikasi!");
                triggerAlarm();
            } 
        }
        return;
    }

    // Exit button
    if (String(topic) == topic_exit_button) {
      if (msg == "exit") {
        authorizedOpen = true;
        authorizedTimer = millis();
        Serial.print("[Exit] Tombol exit manual terdeteksi");

        ledAccessValid();
        uiSet("", "EXIT BUTTON", "Pintu Dibuka", "", UI_GRANTED, 2000);
        return;
      }
    }
}

// ==================== FIREBASE ====================
void fbPUT(const String& path, const String& json) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FB] PUT skipped: WiFi tidak terhubung");
        return;
    }
    HTTPClient http;
    String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_AUTH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(json);

    if (!(code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT)) {
        Serial.print("[FB] PUT gagal, kode: ");
        Serial.println(code);
    }
    http.end();
}

void fbDELETE(const String& path) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[FB] DELETE skipped: WiFi tidak terhubung");
        return;
    }
    HTTPClient http;
    String url = String(FIREBASE_URL) + path + ".json?auth=" + FIREBASE_AUTH;
    http.begin(url);
    int code = http.sendRequest("DELETE");

    if (!(code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT)) {
        Serial.print("[FB] PUT gagal, kode: ");
        Serial.println(code);
    }
    http.end();
}

void fbLog(uint8_t id, const String& name, const String& uid, const String& method, const String& eventType) {
    String key = getTimestampKey();
    String timeStr = getTimeString();
    String path = String(NODE_LOG) + "/" + key;

    // Simpan ke firebase
    String json = "{";
    json += "\"Waktu\": \"" + timeStr + "\",";
    json += "\"ID\": \"" + String(id) + "\",";
    json += "\"Nama\": \"" + escapeJsonString(name) + "\",";
    json += "\"Metode\":\"" + escapeJsonString(method) + "\",";
    json += "\"Event\": \"" + eventType + "\"";
    json += "}";

    fbPUT(path, json);
    Serial.print("[FB] Log ");
    Serial.print(eventType);
    Serial.print(" => ");
    Serial.println(key);

    // Log via telegram
    if (WiFi.status() == WL_CONNECTED) {
        String logMsg;
        if (eventType == "access_granted") {
            logMsg = "*✅ Akses Berhasil*\n";
            logMsg += "ID: `" + String(id) + "` \n";
            logMsg += "Nama: *" + name + "*\n";
            logMsg += "Metode: " + method + "\n";
        } else if (eventType == "auth_failed") {
            logMsg = "❌ *Akses Ditolak*\n";
            logMsg += "Autentikasi tahap awal gagal\n";
            logMsg += "Motode: *" + method + "*\n";
        } else if (eventType == "face_mismatch") {
            logMsg  = "❌ *Akses Ditolak*\n";
            logMsg += "Verifikasi wajah gagal\n";
            logMsg += "ID: `" + String(id) + "`\n";
            logMsg += "Nama: *" + name + "*\n";
            logMsg += "Metode: *" + method + "*\n";
        } else {
            logMsg  = "📌 *Log Event*\n";
            logMsg += "Event: *" + eventType + "*\n";
        }
        logMsg += "Waktu: " + timeStr;
        
        tgSendLimited(logMsg, "Markdown");
        Serial.println("[Telegram]");
    }
}

String getLogsFromFirebase(int limit) {
    if (WiFi.status() != WL_CONNECTED) {
        return "❌  WiFi tidak terhubung. Gagal mengambil log.";
    }

    HTTPClient http;
    String url = String(FIREBASE_URL) + NODE_LOG + ".json?auth=" + FIREBASE_AUTH + "&orderBy=\"$key\"&limitToLast=" + String(limit);
    http.begin(url);
    int httpCode = http.GET();

    if (httpCode > 0) {
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();

            if (payload == "null" || payload.length() <= 2) {
                http.end();
                return "📋 *Log Terakhir (0)*\n\nBelum ada data log di Firebase.";
            }

            payload.remove(0, 1);
            payload.remove(payload.length() - 1);

            int start = 0;
            int counter = 0;
            String formattedLog = "📋 *Log Terakhir (" + String(limit) + ")*:\n\n"; 

            while (start < payload.length() && counter < limit) {
                int keyStart = payload.indexOf("\"", start);
                if (keyStart == -1) break;
                int keyEnd = payload.indexOf("\":", keyStart + 1);
                if (keyEnd == -1) break;

                String fullKey = payload.substring(keyStart + 1, keyEnd);
                int dataStart = payload.indexOf("{", keyEnd);
                if (dataStart == -1) break;
                int dataEnd = payload.indexOf("}", dataStart);
                if (dataEnd == -1) break;

                String data = payload.substring(dataStart + 1, dataEnd);
                data.replace("\",\"", "\n");
                data.replace("\":", ": ");
                data.replace("\"", "");
                data.replace(",", "");
                data.replace("access_granted", "Akses Berhasil");
                data.replace("auth_failed", "Autentikasi awal gagal");
                data.replace("face_mismatch", "Verifikasi wajah gagal");

                formattedLog += data + "\n=========================\n";
                start = dataEnd + 1;
                if (start < payload.length() && payload.charAt(start) == ',') start++;
                counter++;
            }            
            http.end();
            return formattedLog;
        } else {
            http.end();
            return "❌  Gagal mengambil log Firebase. HTTP Code: " + String(httpCode);
        }
    } else {
        http.end();
        return "❌ Gagal terhubung ke Firebase.";
    }
}

// ==================== EEPROM CONFIG ====================
void saveUserToEEPROM(uint8_t id, const String& name, const String& uid) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;

    for (int i = 0; i < NAME_LENGTH; i++) {
        EEPROM.write(addr + i, (i < name.length()) ? name[i] : 0);
    }
    for (int i = 0; i < UID_LENGTH; i++) {
        EEPROM.write(addr + NAME_LENGTH + i, (i < uid.length()) ? uid[i] : 0);
    }
    EEPROM.commit();
}

String readNameFromEEPROM(uint8_t id) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    String name = "";
    for (int i = 0; i < NAME_LENGTH; i++) {
        char c = EEPROM.read(addr + i);
        if (c == 0) break;
        name += c;
    }
    return name;
}

String readUIDFromEEPROM(uint8_t id) {
    int addr = RECORD_START + (id - 1) * RECORD_SIZE;
    String uid = "";
    for (int i = 0; i < UID_LENGTH; i++) {
        char c = EEPROM.read(addr + NAME_LENGTH + i);
        if (c == 0) break;
        uid += c;
    }
    return uid;
}

String escapeJsonString(String s) {
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    return s;
}

// ==================== UI HELPERS ====================
void uiSet(const String& l1, const String& l2, const String& l3, const String& l4, UiMode mode, unsigned long holdMs) {
    uiMode = mode;

    // Anti flicker: update jika ada perubahan
    if (l1 != lastOledL1 || l2 != lastOledL2 || l3 != lastOledL3 || l4 != lastOledL4) {
        updateDisplay(l1, l2, l3, l4);
        lastOledL1 = l1;
        lastOledL2 = l2;
        lastOledL3 = l3;
        lastOledL4 = l4;
    }

    if (holdMs > 0) {
        uiLock.locked = true;
        uiLock.untilMs = millis() + holdMs;
    }
}

void uiTick() {
    if (uiLock.locked && millis() > uiLock.untilMs) {
        uiLock.locked = false;
    }
}

void uiSetStandby() {
    uiTick();
    if (!uiLock.locked && currentEnrollState == STATE_IDLE && !waitingForFace) {
        ledStandby();
    }
}

// Telegram send limited (NEW)
void tgSendLimited(const String& msg, const String& parseMode) {
    if (WiFi.status() != WL_CONNECTED) return;

    // hash sederhana berbasis msg (cukup untuk anti-spam basic)
    int take = min(18, (int)msg.length());
    String h = String(msg.length()) + "|" + msg.substring(0, min(18, (int)msg.length()));

    if (h == lastTgHash && (millis() - lastTgSentMs < TG_MIN_INTERVAL_MS)) return;

    botLog.sendMessage(CHAT_ID, msg, parseMode);
    lastTgSentMs = millis();
    lastTgHash = h;
}

// ==================== DISPLAY ====================
void centerDisplay(const String& text, int y) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH -w) / 2;
    display.setCursor(x, y);
    display.println(text);
}

void updateDisplay(const String& line1, const String& line2, const String& line3, const String& line4) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    centerDisplay(line1, 0);
    centerDisplay(line2, 16);
    centerDisplay(line3, 32);
    centerDisplay(line4, 48);

    display.display();
}

void ledStandby() {
    static unsigned long lastUI = 0;
    if (millis() - lastUI < 900) return; // update max ~1x per 0.9 detik
    lastUI = millis();

    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_BLUE);
    uiSet("SMART LOCK SYSTEM", "READY", "", "Silakan Scan", UI_READY, 0);
}

void ledSuccessful() {
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN);
    digitalWrite(BUZZER_PIN, HIGH);
    shortDelay(200);
    digitalWrite(BUZZER_PIN, LOW);
}

void ledAccessValid() {
    for (int i = 0; i < 2; i++) {
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_GREEN);
        digitalWrite(BUZZER_PIN, HIGH);
        shortDelay(100);
        digitalWrite(BUZZER_PIN, LOW);
        shortDelay(50);
    }
}

void ledFailed() {
    for (int i = 0; i < 3; i++){
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
        digitalWrite(BUZZER_PIN, HIGH);
        shortDelay(250);
        finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
        digitalWrite(BUZZER_PIN, LOW);
        shortDelay(250);
    }
    finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
    shortDelay(250);
}

// ==================== ALARM ====================
void triggerAlarm() {
    String alertMsg = "⚠️ *WARNING* ⚠️\nPintu terbuka tanpa autentikasi!";
    uiSet("WARNING", "", "Pintu Terbuka", "Tanpa Autentikasi", UI_DENIED, 0);
    
    tgSendLimited(alertMsg, "Markdown");
    Serial.println("[Telegram] WARNING");

    for (int i = 0; i < 10; i++) {
        finger.LEDcontrol(FINGERPRINT_LED_ON, 0, FINGERPRINT_LED_RED, 0);
        digitalWrite(BUZZER_PIN, HIGH);
        shortDelay(800);
        finger.LEDcontrol(FINGERPRINT_LED_OFF, 0, 0, 0);
        digitalWrite(BUZZER_PIN, LOW);
        shortDelay(200);
    }
}

// ==================== TELEGRAM HANDLERS ====================
String getTelegramCommand(const String& text) {
    if (text.startsWith("/")) {
        int spaceIndex = text.indexOf(' ');
        if (spaceIndex == -1) return text;
        return text.substring(0, spaceIndex);
    }
    return "";
}

void startEnrollState(EnrollmentState st) {
    currentEnrollState = st;
    enrollStartTime = millis();
}

void cancelEnrollment(const String& reason) {
    if (currentAdminChatID.length()) {
        botAdmin.sendMessage(currentAdminChatID, "❌ Enrollment Dibatalkan.\n" + reason, "Markdown");
    }

    // hapus fingerprint jika sudah tersimpan
    if (currentEnrollID > 0) {
        finger.deleteModel(currentEnrollID);
        // hapus dari EEPRON & Firebase
        saveUserToEEPROM(currentEnrollID, "", "");

        if (WiFi.status() == WL_CONNECTED) {
            fbDELETE(String(NODE_USERS) + "/" + String(currentEnrollID));
        }

        // hapus wajah di ESP32_CAM
        String delMsg = "delete " + String(currentEnrollID);
        mqttClient.publish(topic_delete_face, delMsg.c_str());
    }

    mqttClient.publish(topic_enroll_face, "false");

    uiSet("ENROLL GAGAL", "", "Waktu Habis", "Data Gagal Disimpan", UI_ENROLL, 3500);

    currentEnrollID = 0;
    currentEnrollUID = "";
    currentEnrollName = "";
    currentAdminChatID ="";
    enrollStartTime = 0;
    currentEnrollState = STATE_IDLE;
}

void finishEnrollment() {
    saveUserToEEPROM(currentEnrollID, currentEnrollName, currentEnrollUID);

    String json = "{";
    json += "\"name\":\"" + escapeJsonString(currentEnrollName) + "\",";
    json += "\"uid\":\"" + currentEnrollUID + "\",";
    json += "\"time_registered\":\"" + getTimeString() + "\"";
    json += "}";

    if (WiFi.status() == WL_CONNECTED) {
        fbPUT(String(NODE_USERS) + "/" + String(currentEnrollID), json);
    }

    String confirmMsg = "✅ Enrollment Selesai\n";
    confirmMsg += "Nama: *" + currentEnrollName + "*\n";
    confirmMsg += "ID: *" + String(currentEnrollID) + "*\n";
    confirmMsg += "UID RFID: *" + currentEnrollUID + "*\n";
    
    botAdmin.sendMessage(CHAT_ID, confirmMsg, "Markdown");
    ledSuccessful();
    uiSet("ENROLL SELESAI", "ID " + String(currentEnrollID), currentEnrollName, "Berhasil Terdaftar", UI_ENROLL, 4000);

    currentEnrollID = 0;
    currentEnrollName = "";
    currentEnrollUID = "";
    currentAdminChatID = "";
    enrollStartTime = 0;
    currentEnrollState = STATE_IDLE;
}

void handleNewMessages(int numNewMessages) {
    Serial.print("[TELE] Pesan baru: "); 
    Serial.print(numNewMessages); 
    Serial.println(" new message.");
    
    for (int i = 0; i < numNewMessages; i++) {
        String chat_id = botAdmin.messages[i].chat_id; 
        String text = botAdmin.messages[i].text;      
        String sender_name = botAdmin.messages[i].from_name;  

        if (chat_id != CHAT_ID) {
            botAdmin.sendMessage(chat_id, "❌ Anda tidak punya izin admin.");
            continue;
        }
        
        String command = getTelegramCommand(text);

        // Enrollment state handling
        if (currentEnrollState != STATE_IDLE) {
            if (currentEnrollState == STATE_WAITING_NAME) {
                text.trim();
                currentEnrollName = text; 
                if (currentEnrollName.length() > 0) {                   
                    botAdmin.sendMessage(chat_id, "Nama tersimpan: *" + currentEnrollName + "*. ✅\n\nTempelkan jari anda ke sensor untuk *perekaman tahap 1* (maks 30 detik).", "Markdown");
                    uiSet("Enroll ID " + String(currentEnrollID), currentEnrollName, "Scan Jari Anda", "Tahap 1", UI_ENROLL, 0);
                    startEnrollState(STATE_WAITING_FP_1);
                } else {
                    botAdmin.sendMessage(chat_id, "❌ Nama tidak valid, coba lagi.");
                }
            }
            else if (currentEnrollState == STATE_WAITING_DELETE_ID) {
                text.trim();
                int idToDelete = text.toInt();

                if (idToDelete > 0 && idToDelete <= MAX_ID) {
                    botAdmin.sendMessage(chat_id, "Menghapus user ID *" + String(idToDelete) + "*...", "Markdown");
                    deleteUser((uint8_t)idToDelete, true);
                    currentEnrollState = STATE_IDLE;
                    currentAdminChatID_Delete = "";
                    uiSet("DELETE USER", "", "Selesai", "", UI_READY, 2500);
                } else {
                    botAdmin.sendMessage(chat_id, "❌ ID tidak ada");
                }
            }
            continue;
        }
        if (command == "/start") {
            String welcome = "Halo, " + sender_name + "!\n";
            welcome += "Perintah:\n";
            welcome += "/enroll - Registrasi user baru.\n";
            welcome += "/list   - Daftar user.\n";
            welcome += "/delete - Hapus user berdasarkan ID.\n";
            welcome += "/log    - Ambil 5 log terakhir dari Firebase\n"; 
            botAdmin.sendMessage(chat_id, welcome);
        }
        else if (command == "/enroll") {
            int id = findEmptyID();
            if (id <= 0) {
                botAdmin.sendMessage(chat_id, "❌ Penyimpanan penuh (Maks " + String(MAX_ID) + " ID).");
            } else {
                currentEnrollID = id;
                currentAdminChatID = chat_id;
                startEnrollState(STATE_WAITING_NAME);
                botAdmin.sendMessage(chat_id, "📌 ID tersedia: *" + String(id) + "*.\nKirim *Nama/Username*:", "Markdown");
                uiSet("ENROLL USER", "ID " + String(id), "Kirim Nama Anda", "Via Telegram", UI_ENROLL, 0);
            }
        } 
        else if (command == "/list") {
            String userList = listUsersTelegram();
            botAdmin.sendMessage(chat_id, userList);
        }
        else if (command == "/delete") {
            int sp = text.indexOf(' ');
            if (sp == -1) {
                botAdmin.sendMessage(chat_id, "Kirim ID yang ingin dihapus: ");
                currentEnrollState = STATE_WAITING_DELETE_ID;
                currentAdminChatID_Delete = chat_id;
                uiSet("DELETE USER", "Kirim ID yang", "Ingin Dihapus", "Via Telegram", UI_READY, 0);
                continue;
            }
            int id = text.substring(sp + 1).toInt();
            if (id <= 0 || id > MAX_ID) {
                botAdmin.sendMessage(chat_id, "❌ ID tidak ada");
                continue;
            }
            deleteUser((uint8_t)id, true); 
        }
        else if (command == "/log") {
            botAdmin.sendMessage(chat_id, "*Mengambil 5 log terakhir...*", "Markdown");
            String logs = getLogsFromFirebase(5);
            botAdmin.sendMessage(chat_id, logs, "Markdown");
        }
        else {
            botAdmin.sendMessage(chat_id, "Perintah tidak dikenal. Coba ulang /start.");
        }
    }
    if (numNewMessages > 0) {
        lastUpdateID = botAdmin.messages[numNewMessages - 1].update_id;
    }
}

void handleLogBot() {
    int num = botLog.getUpdates(botLog.last_message_received + 1);

    for (int i = 0; i < num; i++) {
        String command = botLog.messages[i].text;
        String chat_id = botLog.messages[i].chat_id;

        if (command == "/log") {
            botLog.sendMessage(chat_id, "*Mengambil 5 log terakhir...*", "Markdown");
            String logs = getLogsFromFirebase(5);
            botLog.sendMessage(chat_id, logs, "Markdown");
        } else {
            botLog.sendMessage(chat_id, "Perintah tidak dikenal. Coba ulang /log.");
        }
    }
}

// ==================== USERS ====================
// Find id
int findEmptyID() {
    for (int i = 1; i <= MAX_ID; i++) {
        if (readNameFromEEPROM(i).length() == 0 && finger.loadModel(i) != FINGERPRINT_OK) {
            return i;
        }
    }
    return 0;
}

// List user
String listUsersTelegram() {
    String list = "📋 *Daftar User Terdaftar*:\n\n";
    finger.getTemplateCount();
    int found = 0;
    for (int i = 1; i <= MAX_ID; i++) {
        if (finger.loadModel(i) == FINGERPRINT_OK) {
            String name = readNameFromEEPROM(i);
            String uid = readUIDFromEEPROM(i);
            list += "ID: *" + String(i) + "* | Nama: *" + name + "* | UID: `" + uid + "`\n";
            found++;
        } else {
            String uid = readUIDFromEEPROM(i);
            if(uid.length() == 8 || uid.length() == 10) {
                String name = readNameFromEEPROM(i);
                list += "ID: *" + String(i) + "* | Nama: *" + name + "* | UID: `" + uid + "` (FP kosong)\n";
                found++;
            }
        }
    }
    if (found == 0) return "Belum ada user terdaftar.";
    return list + "\n*Total: " + String(found) + " user.*";
}

// Delete user
void deleteUser(uint8_t id, bool viaTelegram) {
    String statusMsg = "";
    if (id < 1 || id > MAX_ID) {
        statusMsg = "ID tidak valid!";
        if (viaTelegram) botAdmin.sendMessage(CHAT_ID, statusMsg, "Markdown");
        else Serial.println(statusMsg);
        return;
    } 
    
    uint8_t fp_del_result = finger.deleteModel(id);
    saveUserToEEPROM(id, "", "");

    if (WiFi.status() == WL_CONNECTED) fbDELETE(String(NODE_USERS) + "/" + String(id));
    
    if (fp_del_result == FINGERPRINT_OK) {
        statusMsg = "✅ User ID *" + String(id) + "* berhasil dihapus";
        uiSet("DELETE USER", "ID " + String(id), "Berhasil", "", UI_READY, 2500);

        String delMsg = "delete " + String(id);
        mqttClient.publish(topic_delete_face, delMsg.c_str());
        Serial.println("MQTT Kirim hapus face: " + delMsg);
    } else {
        statusMsg = "⚠️ Fingerprint ID *" + String(id) + "* tidak ada di sensor. EEPROM/Firebase dibersihkan.";
        uiSet("DELETE USER", "ID " + String(id), "Tidak Ditemukan", "", UI_READY, 2500);
    }
    
    if (viaTelegram) {
        botAdmin.sendMessage(CHAT_ID, statusMsg, "Markdown");
    } else {
        Serial.println(statusMsg);
    }
}

// ==================== ENROLL HELPERS ====================
// Enroll fingerprint step 1
void enrollStepFP1() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(1);
        if (p == FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "Perekaman 1/2 berhasil ✅\n\nLepaskan jari, lalu tempelkan lagi untuk *perekaman tahap 2* (maks 30 detik).", "Markdown");
            uiSet("Enroll ID " + String(currentEnrollID), "Lepaskan & Scan Ulang", "Jari Anda", "Untuk Tahap 2", UI_ENROLL, 0);
            startEnrollState(STATE_WAITING_FP_2);
        } else {
            cancelEnrollment("Gagal konversi gambar sidik jari.");
            return;
        }
    }    
}

// Enroll fingerprint step 2
void enrollStepFP2() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_OK) {
        p = finger.image2Tz(2);
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Gagal konversi tahap 2. Ulangi `/enroll`.");
            currentEnrollState = STATE_IDLE;
            uiSet("ENROLL GAGAL", "Ulangi Perintah", "/enroll", "Via Telegram", UI_ENROLL, 3500);
            return;
        }

        p = finger.createModel();
        if (p != FINGERPRINT_OK) {
            botAdmin.sendMessage(currentAdminChatID, "❌ Sidik jari tidak cocok. Ulangi `/enroll`.");
            currentEnrollState = STATE_IDLE;
            uiSet("ENROLL GAGAL", "jari Tidak Cocok", "Ulang /enroll", "Via Telegram", UI_ENROLL, 3500);
            return;
        }

        p = finger.storeModel(currentEnrollID);
        if (p == FINGERPRINT_OK) {
            rfid.PCD_Init(); 
            botAdmin.sendMessage(currentAdminChatID, "✅ Fingerprint tersimpan!\n\nSekarang tempelkan kartu RFID (maks 30 detik).", "Markdown");
            uiSet("Enroll ID " + String(currentEnrollID), "Sidik Jari Tersimpan", "Langkah Selanjutnya", "Tempelkan RFID", UI_ENROLL, 3500);
            startEnrollState(STATE_WAITING_RFID_CARD);
        } else {
            botAdmin.sendMessage(currentAdminChatID, "❌ Gagal menyimpan fingerprint. Ulangi `/enroll`.");
            currentEnrollState = STATE_IDLE;
            uiSet("ENROLL GAGAL", "Ulangi Perintah", "/enroll", "Via Telegram", UI_ENROLL, 3500);
        }
    } 
}

// Enroll RFID
void enrollRFID(uint8_t id, const String& name, const String& uid, const String& chat_id) {
    for (uint8_t i = 1; i <= MAX_ID; i++) {
        if (i != id && readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
            botAdmin.sendMessage(chat_id, "❌ UID sudah terdaftar di ID " + String(i) + ". Enrollment dibatalkan.");
            cancelEnrollment("UID RFID sudah digunakan.");
            return;
        }
    }

    currentEnrollUID = uid;   // simpan sementara
    mqttClient.publish(topic_enroll_face, "true");

    String enrollMsg = "✅ RFID berhasil tersimpan!\n\n";
    if (cameraIP.length() > 0) {
        enrollMsg += "Selanjutnya daftarkan wajah anda melalui link berikut ini:\n";
        enrollMsg += "http://" + cameraIP + "/enroll";
    } else {
        enrollMsg += "IP kamera belum tersedia. Pastikan ESP32-CAM sudah aktif dan terhubung ke MQTT.";
    }

    botAdmin.sendMessage(CHAT_ID, enrollMsg, "Markdown");
    uiSet("Enroll ID " + String(currentEnrollID), "RFID Tersimpan", "Langkah Selanjutnya", "Daftarkan Wajah", UI_ENROLL, 3500);
    currentAdminChatID = chat_id;
}

// ==================== SCAN FUNCTIONS ====================
void scanFingerprint() {
    if (waitingForFace) return;
    if (currentEnrollState != STATE_IDLE) return; 

    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return;
    if (p != FINGERPRINT_OK) return;

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return;

    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK) {
        uint8_t id = finger.fingerID;
        String name = readNameFromEEPROM(id);
        String uid = readUIDFromEEPROM(id);

        Serial.printf("[FP] Cocok! ID=%d Nama=%s\n", id, name.c_str());

        // sesi 2FA
        sessionAuthID = id;
        sessionNonce = generateNonce();
        sessionStartTime = millis();
        sessionMethod = "Fingerprint";
        sessionUID = "";
        waitingForFace = true;

        DynamicJsonDocument doc(256);
        doc["auth_id"] = sessionAuthID;
        doc["nonce"] = sessionNonce;

        String payload;
        serializeJson(doc, payload);

        mqttClient.publish(topic_stream_cam, payload.c_str());
        ledSuccessful();
        Serial.println("[2FA] Challenge dikirim: " + payload);
        uiSet("VERIFIKASI WAJAH", "via " + cameraIP, "dan Arahkan Wajah", "ke Kamera", UI_FACE_WAIT, 5000);
    } else {
        Serial.println("[FP] Tidak terdaftar");
        ledFailed();
        uiSet("AKSES DITOLAK", "", "Sidik Jari Anda", "Tidak Terdaftar", UI_DENIED, 3500);

        sessionMethod = "Fingerprint";
        sessionUID = "";

        if (WiFi.status() == WL_CONNECTED) 
            fbLog(0, "UNKNOWN", "", "Fingerprint", "auth_failed");
        else 
            Serial.println("[FB] Offline, skipping denied log.");
    }
}

void scanRFID() {
    if (waitingForFace) return;
    if (currentEnrollState != STATE_IDLE) return; 
    if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    currentEnrollUID = uid;
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    for (uint8_t i = 1; i <= MAX_ID; i++) {
        if (readUIDFromEEPROM(i).equalsIgnoreCase(uid)) {
            String name = readNameFromEEPROM(i);
            
            Serial.printf("[RFID] Cocok! ID=%d Nama=%s\n", i, name.c_str());

            // sesi 2FA
            sessionAuthID = i;
            sessionNonce = generateNonce();
            sessionStartTime = millis();
            sessionMethod = "RFID";
            sessionUID = uid;
            waitingForFace = true;

            DynamicJsonDocument doc(256);
            doc["auth_id"] = sessionAuthID;
            doc["nonce"] = sessionNonce;

            String payload;
            serializeJson(doc, payload);

            mqttClient.publish(topic_stream_cam, payload.c_str());
            ledSuccessful();
            Serial.println("[2FA] Challenge dikirim: " + payload);
            uiSet("VERIFIKASI WAJAH", "Via " + cameraIP, "dan Arahkan Wajah", "ke Kamera", UI_FACE_WAIT, 5000);
            return;
        }
    }
    ledFailed();
    uiSet("AKSES DITOLAK", "", "RFID anda", "Tidak Terdaftar", UI_DENIED, 3500);

    sessionMethod = "RFID";
    sessionUID = "-";

    if (WiFi.status() == WL_CONNECTED) 
        fbLog(0, "UNKNOWN", "-", "RFID", "auth_failed");
    else 
        Serial.println("[FB] Offline, skipping denied log.");
}

// ==================== SETUP ====================
void setup() {
    Serial.begin(9600);
    while (!Serial) { shortDelay(10); }

    EEPROM.begin(EEPROM_SIZE);

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    pinMode(RESET_BTN, INPUT_PULLUP);

    // Display
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED tidak terdeteksi!");
    } else {
        uiSet("", "Memulai Sistem", "Menunggu WiFi", "", UI_BOOT, 0); 
    }

    // SPI init
    SPI.begin(18, 19, 23, SS_PIN);
    rfid.PCD_Init();

    // UART2 init
    mySerial.begin(57600, SERIAL_8N1, 16, 17);
    finger.begin(57600);

    if (!finger.verifyPassword()) {
        Serial.println("Sensor fingerprint tidak terdeteksi!");
    } else {
        finger.getTemplateCount();
        Serial.print("Jumlah template fingerprint: ");
        Serial.println(finger.templateCount);
    }

    // WiFi + Time connect
    wifiConnect();
    setupTime();
    wifiWasConnected = (WiFi.status() == WL_CONNECTED);

    // Telegram
    client.setInsecure();

    // MQTT
    mqttSecureClient.setInsecure();
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttReady = false;
    mqttClient.setKeepAlive(60);
    mqttClient.setBufferSize(512);

    randomSeed(micros());
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
            uiSet("WIFI CONNECTED", WiFi.SSID(), "Menunggu Terhubung", "Dengan MQTT...", UI_MQTT_WAIT, 0);
        } else {
            uiSetStandby();
        }    
    } else {
        if (wifiWasConnected && !wifiPortalActive) {
            startReconnectPortal();
        } else if (!wifiPortalActive) {
            uiSet("WIFI SETUP", "Hubungkan WiFi", "ke Access Point", "AuthUnit-Setup", UI_WIFI_WAIT, 0);
        }
    }

    // Keep Telegram polling roughly every BOT_CHECK_INTERVAL
    if (WiFi.status() == WL_CONNECTED && millis() - lastTimeBotCheck >= BOT_CHECK_INTERVAL) {
        int numNewMessages = botAdmin.getUpdates(lastUpdateID + 1);
        while (numNewMessages) {
            handleNewMessages(numNewMessages);
            numNewMessages = botAdmin.getUpdates(lastUpdateID + 1);
        }
        handleLogBot();
        lastTimeBotCheck = millis();
    }

    if ((currentEnrollState == STATE_WAITING_NAME ||
        currentEnrollState == STATE_WAITING_FP_1 ||
        currentEnrollState == STATE_WAITING_FP_2 ||
        currentEnrollState == STATE_WAITING_RFID_CARD ||
        currentEnrollState == STATE_WAITING_FACE_ENROLL) &&
        (millis() - enrollStartTime > ENROLL_TIMEOUT)) {
        cancelEnrollment("Tiap tahap maksimal 30 detik.");
    }

    // 2FA session timeout
    if (waitingForFace && (millis() - sessionStartTime > SESSION_TIMEOUT)) {
        Serial.println("[2FA] Timeout -> sesi dibatalkan");
        waitingForFace = false;

        ledFailed();
        uiSet("VERIFIKASI WAJAH", "Dibatalkan", "", "Waktu Habis", UI_TIMEOUT, 4500);

        String name = readNameFromEEPROM(sessionAuthID);
        if (WiFi.status() == WL_CONNECTED) {
            fbLog(sessionAuthID, name, sessionUID, sessionMethod, "face_mismatch");
        }
    }

    // Scan sensor setiap SCAN_INTERVAL_MS
    if (millis() - lastScanTime >= SCAN_INTERVAL_MS) {
        lastScanTime = millis();

        if (!mqttReady) {
        } else {
            if (currentEnrollState == STATE_IDLE) {
                scanFingerprint();
                scanRFID();
            } else {
                // Proses enrollment non-blocking
                if (currentEnrollState == STATE_WAITING_FP_1) enrollStepFP1();
                else if (currentEnrollState == STATE_WAITING_FP_2) enrollStepFP2();
                else if (currentEnrollState == STATE_WAITING_RFID_CARD) {
                    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                        String uid = "";
                        for (byte i = 0; i < rfid.uid.size; i++) {
                            if (rfid.uid.uidByte[i] < 0x10) uid += "0";
                            uid += String(rfid.uid.uidByte[i], HEX);
                        }
                        uid.toUpperCase();
                        rfid.PICC_HaltA();
                        rfid.PCD_StopCrypto1();

                        enrollRFID(currentEnrollID, currentEnrollName, uid, currentAdminChatID);
                        startEnrollState(STATE_WAITING_FACE_ENROLL);

                        waitingForFace = false;
                        sessionAuthID = 0;
                        sessionNonce = "";
                        sessionStartTime = 0;

                        uiSet("Enroll Wajah", "Buka link di Telegram", "dan Ikuti Proses", "Hingga Selesai...", UI_ENROLL, 5000);
                    }
                }
            }
        }
    }
    // Authorized open timeout (non-blocking)
    if (authorizedOpen && millis() - authorizedTimer > AUTH_DOOR) {
        authorizedOpen = false;
        Serial.println("[AUTH] Status legal-open berakhir, kembali pantau intrusi.");
    }
    shortDelay(5);
}

