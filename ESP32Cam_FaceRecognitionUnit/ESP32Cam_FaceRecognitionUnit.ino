// Project  : ESP32-CAM Face Recognition
// SoftAP   : CamUnit-Setup
// Board    : ESP32 v1.0.4
// Features : Pengenalan wajah, pendaftaran wajah (permanen), manajemen pengguna, integrasi MQTT

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>    // 2.0.3-alpha
#include "FS.h"
#include "SD_MMC.h"
#include <PubSubClient.h>   // 2.8.0
#include <ArduinoJson.h>    // 0.2.0

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ==================== PIN CONFIG ====================
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// ==================== MQTT CONFIG ====================
const char* mqtt_server ="0c82a64bb24146519d9b6df720d08792.s1.eu.hivemq.cloud";
const int mqtt_port     = 8883;
const char* mqtt_user   = "SmartLockSystem";
const char* mqtt_pass   = "2FactorAuthentication";

WiFiClientSecure mqttSecureClient;
PubSubClient mqttClient(mqttSecureClient);
// ---------------------- PUBLISH ------------------------
const char* topic_ip_cam         = "camera/ip_address";     // publish IP kamera ke ESP32_AuthUnit
const char* topic_enroll_confirm = "camera/enroll_confirm"; // konfirmasi enroll wajah selesai ke ESP32_AuthUnit
const char* topic_face_id        = "camera/face_id";        // hasil face recognition ke ESP32_AuthUnit
// --------------------- SUBSCRIBE -----------------------
const char* topic_stream_cam     = "camera/start_stream";   // perintah start strem dari ESP32_AuthUnit
const char* topic_enroll_face    = "camera/enroll_face";    // perintah mode enroll face dari ESP32_AuthUnit
const char* topic_delete_face    = "camera/delete_face";    // perintah hapus face ID dari ESP32_AuthUnit

// ==================== GLOBAL VARIABLES ====================
bool webServerStarted = false;

// Status face recogntion
bool matchFaceStatus = false;
int matchFaceId = 0;
bool lastFaceState = false;  // Status wajah terakhir (true/false)

// Trigger sistem
bool streamTrigger = false;
bool enrollTrigger = false; 

// Mqtt reconnect
unsigned long lastMqttReconnectAttempt = 0;      
const unsigned long mqttRetryInterval = 2000;   

// Publish control
unsigned long lastPublish = 0;      
const unsigned long publishCooldown = 3000;   

// Session 2FA
int activeAuthID = 0;
String activeNonce = "";
bool sessionActive = false;
unsigned long sessionStartMs = 0;
const unsigned long SESSION_TIMEOUT_MS = 30000; 
bool publishedThisSession = false;

// ==================== FUNCTION PROTOTYPES ====================
void startCameraServer();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void reconnectMQTTNonBlocking();
void publishInfoUser(int userId);
void publishCameraIP();

extern void clear_faces_in_ram();
extern void load_faces_from_sd();

// Helper function
String getLocalIpString() {
  IPAddress ip = WiFi.localIP();
  return ip.toString();
}

// ==================== SETUP ====================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(9600);
  Serial.setDebugOutput(true);
  Serial.println();

  // ==================== WIFI ====================
  WiFiManager wm;
  //wm.resetSettings();
  wm.setConfigPortalTimeout(180);

  if (!wm.autoConnect("CamUnit-Setup")) {
    Serial.println("Gagal koneksi WiFi, restart...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi terhubung!");

  // ==================== SD CARD INIT ====================
  if (!SD_MMC.begin()) {
    Serial.println("SD Card gagal mount");
    return;
  }
  Serial.println("SD Card berhasil dimount");

  // ==================== CAMERA CONFIG ====================
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // BUFFER & QUALITY
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
  }

  // Inisialisasi kamera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("❌ Camera init gagal: 0x%x\n", err);
    return;
  }

  // Set ukuran frame untuk face recognition
  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
    s->set_brightness(s, 2);
    s->set_contrast(s, 1);
    s->set_saturation(s, -1);
    s->set_agc_gain(s, 400);
    s->set_gain_ctrl(s, 0);
    s->set_ae_level(s, 2);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 0);
    s->set_exposure_ctrl(s, 1);
    s->set_lenc(s, 0);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_raw_gma(s, 1);
  }
  s->set_framesize(s, FRAMESIZE_QVGA);

  // ==================== MQTT INIT ====================
  //mqttSecureClient.setInsecure();
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqtt_callback);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(512);

  reconnectMQTTNonBlocking();

  // ==================== START CAMERA SERVER ====================
  if (!webServerStarted) {
    startCameraServer();
    webServerStarted = true;
    Serial.print("📷 Camera Ready: http://");
    Serial.println(WiFi.localIP());
  }
  
}

// ==================== MQTT CONFIG ====================)
void reconnectMQTTNonBlocking() {
  if (mqttClient.connected()) return;
  if (millis() - lastMqttReconnectAttempt < mqttRetryInterval) return;
  lastMqttReconnectAttempt = millis();

  String clientId = "ESP32_CamUnit";
    clientId += String((uint32_t)ESP.getEfuseMac(), HEX);

  Serial.print("[MQTT] Menghubungkan ke HiveMQ Cloud...");
  if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
    Serial.println("Terhubung ✅");

    mqttClient.subscribe(topic_stream_cam);
    mqttClient.subscribe(topic_enroll_face);
    mqttClient.subscribe(topic_delete_face);

    mqttClient.setCallback(mqtt_callback);

    publishCameraIP();
  } else {
    Serial.print("Gagal, rc=");
    Serial.println(mqttClient.state());
  }
}

// MQTT Callback Publish & Subcribe
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

    Serial.print("[MQTT] ");
    Serial.println(topic);
    Serial.print("=> ");
    Serial.println(message);

    // Trigger start stream  
    if (String(topic) == topic_stream_cam) {
      DynamicJsonDocument doc(256);
      DeserializationError error = deserializeJson(doc, message);
      if(error){
          Serial.println("[2FA] JSON tidak valid");
          return;
      }

      activeAuthID = doc["auth_id"];
      activeNonce = doc["nonce"].as<String>();

      sessionActive = true;
      streamTrigger = true;
      sessionStartMs = millis();

      lastFaceState = false;
      matchFaceId = 0;
      publishedThisSession = false;

      Serial.println("[2FA] Session 2FA dimulai");
      return;
    }

    // Trigger mode enroll face
    if (String(topic) == topic_enroll_face && message == "true") {
        streamTrigger = true;
        enrollTrigger = true;
        Serial.println("[ENROLL] Mode enroll wajah aktif");
        return;
    }

    // Hapus face ID dari SD
    if (String(topic) == topic_delete_face) {
      if (message.startsWith("delete ")) {
        String idPart = message.substring(7);
        int idToDelete = idPart.toInt();

        if (idToDelete > 0) {
          char path[32];
          sprintf(path, "/face_%d.bin", idToDelete);

          if (SD_MMC.exists(path)) {
            SD_MMC.remove(path);
            Serial.printf("MQTT: Face ID %d dihapus dari SD\n", idToDelete);
            clear_faces_in_ram();
            load_faces_from_sd();
          }
        } else {
          Serial.println("ID tidak ditemukan di SD");
        }
      }
    }

}

// MQTT Publish Info User Status & ID
void publishInfoUser(int userId) {

  if(!sessionActive) return;

  DynamicJsonDocument doc(256);
  doc["face_id"] = userId;
  doc["nonce"] = activeNonce;

  String output;
  serializeJson(doc, output);

  mqttClient.publish(topic_face_id, output.c_str());
  Serial.println("[2FA] Face result dikirim: " + output);

  sessionActive = false;
  streamTrigger = false;
}

// MQTT Publish Info Enroll URL
void publishCameraIP() {
  String ip = getLocalIpString();
  mqttClient.publish(topic_ip_cam, ip.c_str(), true);  // retained supaya Auth langsung dapat IP terakhir
  Serial.println("[MQTT] Camera IP dikirim: " + ip);
}

// ==================== MAIN LOOP ====================
void loop() {
  reconnectMQTTNonBlocking();
  mqttClient.loop();

  // session timeout
  if (sessionActive && (millis() - sessionStartMs > SESSION_TIMEOUT_MS)) {
    Serial.println("[2FA] Session timeout → kirim face_id=0");
    publishInfoUser(0); // kirim "tidak match"
    publishedThisSession = true;
    matchFaceStatus = false;
    matchFaceId = 0;
    lastPublish = millis();
  }

  // publish ketika sessionActive + streamTrigger + belum publish
  if (sessionActive && streamTrigger && !publishedThisSession) {
    if (matchFaceStatus && matchFaceId > 0 && (millis() - lastPublish > publishCooldown)) {
      Serial.printf("✅ FACE MATCH (ID %d) → publish\n", matchFaceId);
      publishInfoUser(matchFaceId);

      publishedThisSession = true;
      matchFaceStatus = false;
      matchFaceId = 0;
      lastPublish = millis();
    }
  }
}