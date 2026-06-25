# Smart Lock Security System UPA TIK Undiksha

---

## Identitas Penulis
* **Nama** : Made Waradiana Aryadi
* **NIM** : 2115101040
* **Program Studi** : Ilmu Komputer
* **Jurusan** : Teknik Informatika
* **Fakultas** : Teknik dan Kejuruan
* **Instansi** : Universitas Pendidikan Ganesha

---

## Ringkasan Sistem
Sistem ini adalah perangkat *Smart Lock Security System* berbasis IoT yang mengimplementasikan Autentikasi Dua Faktor (2FA) untuk meningkatkan keamanan akses ruang server UPA TIK Undiksha. Sistem mengintegrasikan tiga unit mikrokontroler (ESP32 *Authentication Unit*, ESP32-CAM *Face Recognition Unit*, dan ESP32 *Door Lock Unit*) yang saling berkomunikasi secara *real-time* menggunakan protokol MQTT (HiveMQ Cloud). Akses masuk mengharuskan verifikasi tahap pertama (RFID atau *Fingerprint*) dan verifikasi tahap kedua (*Face Recognition* lokal). Jika valid, *solenoid door lock* akan terbuka, dan riwayat akses disinkronisasikan ke Firebase Realtime Database serta dikirim sebagai notifikasi instan melalui Telegram Bot Admin. Selain penguncian otomatis, sistem ini dilengkapi fitur anti-intrusi berbasis *magnetic door switch* yang memicu alarm *buzzer* dan peringatan dini jika mendeteksi pintu dibuka secara paksa (akses ilegal) tanpa autentikasi sah.

### Prototipe Sistem
<img width="2631" height="1366" alt="4" src="https://github.com/user-attachments/assets/eeaeca49-e395-4867-92f4-f1cbb247dc0e" />

---

## Komponen Utama:
1. **ESP32 Authentication Unit (`ESP32_AuthenticationUnit.ino`)**
   * Berfungsi sebagai unit autentikasi awal di pintu masuk.
   * Menangani pembacaan sidik jari (Fingerprint Sensor Adafruit), kartu RFID (MFRC522), display informasi (OLED SSD1306), serta integrasi Telegram Bot untuk manajemen user (enrollment).
2. **ESP32-CAM Face Recognition Unit (`ESP32Cam_FaceRecognitionUnit.ino`, `app_httpd.cpp`, `camera_pins.h`, `camera_index.h`)**
   * Berfungsi sebagai unit verifikasi wajah (Face Recognition) setelah autentikasi pertama berhasil (2FA).
   * Menangani *face detection* dan *matching* secara lokal serta komunikasi via MQTT untuk mengirimkan status verifikasi.
3. **ESP32 Door Lock Unit (`ESP32_DoorLockUnit.ino`)**
   * Berfungsi sebagai aktuator yang mengendalikan *solenoid door lock* melalui relay.
   * Memantau sensor magnetik pintu (*door switch sensor*) dan tombol keluar (*push-to-exit button*).

<img width="4095" height="1674" alt="Rangkaian RFID Smart Lock_bb" src="https://github.com/user-attachments/assets/b6ff8cb5-01de-4d75-a65a-34d18dd0bc04" />

---

## Alur Kerja Sistem

### Sistem Autentikasi Pengguna
* **Inisialisasi**: Sistem aktif, menghubungkan Wi-Fi via SoftAP, terkoneksi ke MQTT Broker, dan menginisialisasi modul RFID serta Fingerprint.
* **Autentikasi Tahap 1**: Pengguna melakukan pemindaian RFID atau Fingerprint. Jika data cocok dengan basis data, tahap pertama dinyatakan valid.
* **Autentikasi Tahap 2 (Verifikasi Wajah)**: ESP32-CAM melakukan pemindaian wajah sebagai validasi tambahan kredensial (Two-Factor Authentication).
* **Akses Diterima**: Jika wajah cocok, solenoid door lock terbuka selama 5 detik, akses dinyatakan sah, dan tercatat dalam riwayat akses Firebase.
* **Akses Ditolak**: Jika wajah tidak cocok, solenoid tetap terkunci, sistem mengirim notifikasi peringatan gagal akses ke Telegram, dan mencatatnya ke riwayat log.

<img width="968" height="532" alt="Diagram Tanpa Judul (2)" src="https://github.com/user-attachments/assets/e71ece3b-368c-4b26-8790-a1b123727630" />

### Sistem Deteksi Akses Ilegal
* **Pemantauan Pintu**: Sensor magnetic door switch mendeteksi status fisik pintu (terbuka/tertutup).
* **Akses Legal**: Jika sensor mendeteksi pintu terbuka setelah adanya proses autentikasi yang valid, akses dianggap sukses dan dicatat pada riwayat log.
* **Akses Ilegal (Intrusi)**: Jika sensor mendeteksi pintu terbuka tanpa adanya autentikasi resmi sebelumnya, sistem langsung mengaktifkan alarm buzzer, mengirim notifikasi peringatan intrusi ke Telegram Admin, dan merekam kejadian ilegal tersebut ke Firebase.

<img width="656" height="522" alt="Diagram Tanpa Judul (3)" src="https://github.com/user-attachments/assets/237f7a6b-d3e1-4e23-b6e3-2ea39ef28b7e" />

---

## Arsitektur & Arus Integrasi Sistem

Sistem ini mengintegrasikan tiga unit mikrokontroler ESP32, MQTT Broker, Firebase, dan Telegram Bot dengan rincian arsitektur sebagai berikut:

1. **Konfigurasi Jaringan (SoftAP):** Setiap ESP32 dilengkapi fitur *Captive Portal* menggunakan library `WiFiManager` untuk konfigurasi SSID dan password Wi-Fi secara dinamis.
2. **Komunikasi MQTT (HiveMQ):** Ketiga unit ESP32 bertindak sebagai *MQTT Client* yang terhubung ke *HiveMQ Broker* menggunakan pola *Publish/Subscribe* untuk pertukaran data antar-unit secara *real-time*.
3. **Integrasi Firebase:** *Authentication Unit* terhubung ke Firebase Realtime Database melalui REST API (HTTP PUT) untuk sinkronisasi data pengguna dan penyimpanan log riwayat akses.
4. **Integrasi Telegram Bot:** Menggunakan HTTP Client pada *Authentication Unit* untuk keperluan monitoring, pendaftaran (*enrollment*) pengguna baru, serta pengiriman notifikasi instan.

<img width="8080" height="5622" alt="Untitled" src="https://github.com/user-attachments/assets/f0cfeb04-cb25-4a24-996c-c868963ad5e5" />

---





