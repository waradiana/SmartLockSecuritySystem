#include "Arduino.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "fd_forward.h"
#include "fr_forward.h"
#include "camera_index.h"
#include "FS.h"
#include "SD_MMC.h"
#include "PubSubClient.h"

// ----------------------- GLOBAL VARIABLES -----------------------
#define ENROLL_CONFIRM_TIMES  5  // Jumlah sample saat enroll wajah
#define FACE_ID_SAVE_NUMBER   5  // Maksimal wajah disimpan
#define FACE_COLOR_WHITE      0x00FFFFFF
#define FACE_COLOR_BLACK      0x00000000
#define FACE_COLOR_RED        0x000000FF
#define FACE_COLOR_GREEN      0x0000FF00
#define FACE_COLOR_BLUE       0x00FF0000
#define FACE_COLOR_YELLOW     (FACE_COLOR_RED | FACE_COLOR_GREEN)
#define FACE_COLOR_CYAN       (FACE_COLOR_BLUE | FACE_COLOR_GREEN)
#define FACE_COLOR_PURPLE     (FACE_COLOR_BLUE | FACE_COLOR_RED)
#define PART_BOUNDARY         "123456789000000000000987654321"

// ----------------------- VARIABEL DARI FILE .INO -----------------------
extern PubSubClient mqttClient;
extern const char* topic_enroll_confirm;
extern bool streamTrigger;    // false → stream TIDAK boleh diakses
extern bool enrollTrigger;    // false → enroll TIDAK boleh diakses
extern bool matchFaceStatus;  // Hasil recognition = false → wajah yang muncul TIDAK terdaftar
extern int matchFaceId;       // Hasil recognition = 0 → wajah yang muncul TIDAK terdaftar

// ----------------------- STRUCT TYPE VARIABLES -----------------------
// Moving average filter (untuk FPS)
typedef struct {
  size_t size;
  size_t index;
  size_t count;
  int sum;
  int * values;
} ra_filter_t;
// Untuk kirim JPEG secara chunked
typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

// ----------------------- SERVER & FACE ENGINE VARIABLES -----------------------
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
static ra_filter_t ra_filter;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;
static mtmn_config_t mtmn_config = {0};
static int8_t detection_enabled = 1;
static int8_t recognition_enabled = 1;
static int8_t is_enrolling = 0;
static bool face_db_full = false;
static face_id_list id_list = {0};                      // Database wajah di RAM (embedding)
static int face_index_to_user_id[FACE_ID_SAVE_NUMBER];  // Mapping: index RAM → ID user di SD

// ----------------------- FACE DATABASE -----------------------
// Menghapus seluruh face embedding di RAM
void clear_faces_in_ram() {
  for (int i = 0; i < id_list.size; i++) {
    if (id_list.id_list[i]) {
      dl_matrix3d_free(id_list.id_list[i]); // Clear free RAM
      id_list.id_list[i] = NULL;
    }
  }
  // Reset pointer database wajah di RAM
  id_list.count = 0;
  id_list.head  = 0;
  id_list.tail  = 0;
  Serial.println("🧹 RAM face database cleared");
}

// Load ulang seluruh wajah dari SD → RAM
void load_faces_from_sd() {
  // Kosongkan RAM
  clear_faces_in_ram();

  // Reset mapping RAM → user ID
  for (int i = 0; i < FACE_ID_SAVE_NUMBER; i++) {
    face_index_to_user_id[i] = -1;
  }
  int ram_index = 0;

  // Scan file /face_xxx.bin di SD
  for (int id = 1; id <= FACE_ID_SAVE_NUMBER; id++) {
    char path[32];
    sprintf(path, "/face_%d.bin", id);

    // Skip jika file tidak ada
    if (!SD_MMC.exists(path)) continue;

    // Alokasikan buffer face di RAM
    id_list.id_list[ram_index] = dl_matrix3d_alloc(1,1,1,FACE_ID_SIZE);
    File file = SD_MMC.open(path, FILE_READ);
    if (!file) continue;

    // Copy embedding dari SD ke RAM
    file.read(
      (uint8_t*)id_list.id_list[ram_index]->item,
      FACE_ID_SIZE * sizeof(float)
    );
    file.close();

    // Mapping RAM index → user ID
    face_index_to_user_id[ram_index] = id;
    Serial.printf("📂 RAM[%d] ← USER ID %d\n", ram_index, id);
    ram_index++;
  }
  // Update jumlah wajah aktif di RAM
  id_list.count = ram_index;
  id_list.tail  = ram_index;
  Serial.printf("📂 Loaded %d faces (RAM compacted)\n", id_list.count);
}

// Menyimpan face embedding ke file SD
void save_face_to_sd(int index) {
  if (!id_list.id_list[index]) {
    Serial.println("❌ Face ID kosong");
    return;
  }
  char path[32];
  // Start ID dari 1
  sprintf(path, "/face_%d.bin", index+1);
  File file = SD_MMC.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Gagal buka file SD");
    return;
  }
  file.write(
    (uint8_t*)id_list.id_list[index]->item,
    FACE_ID_SIZE * sizeof(float)
  );
  file.close();
  Serial.printf("✅ Face %d disimpan ke SD\n", index+1);
}


// Halaman menu utama
static esp_err_t index_handler(httpd_req_t *req){
  const char* html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <title>ESP32-CAM</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body {
        margin: 0;
        height: 100vh;
        font-family: Arial;
        background: #111;
        color: white;
        display: flex;
        justify-content: center;
        align-items: center;
        text-align: center;
      }
      .container {
        width: 100%;
        max-width: 600px;
        padding: 20px;
      }
      h2 {
        margin-bottom: 30px;
      }
      .menu {
        display: flex;
        gap: 20px;
      }
      button {
        flex: 1;
        padding: 18px 10px;
        font-size: 16px;
        border: none;
        border-radius: 10px;
        background: #2196F3;
        color: white;
        cursor: pointer;
        transition: background 0.2s ease;
      }
      button:hover {
        background: #1976D2;
      }
      button:active {
        background: #1565C0;
      }
      button:active {
        background: #1976D2;
      }
      @media (max-width: 500px) {
        .menu {
          flex-direction: column;
        }
      }
    </style>
  </head>
  <body>
    <div class="container">
      <h2>Menu ESP32-CAM</h2>
      <div class="menu">
        <button onclick="location.href='/stream'">
          Verifikasi Wajah
        </button>
        <button onclick="location.href='/enroll'">
          Daftarkan Wajah
        </button>
        <button onclick="location.href='/users'">
          List ID Wajah
        </button>  
      </div>
    </div>
  </body>
  </html>
  )rawliteral";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, strlen(html));
}

// Halaman daftarkan wajah (HTML gzip dari camera_index.h)
static esp_err_t enroll_handler(httpd_req_t *req){
  // BLOKIR ENROLL jika MQTT masih false (perlu refresh web untuk kondisi terkini)
  if (!enrollTrigger) {
    const char* msg = "ENROLL DISABLED - MQTT trigger is false";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, msg, strlen(msg));
  }

  // Berikan Akses HALAMAN ENROLL jika MQTT sudah true (perlu refresh web untuk kondisi terkini)
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  sensor_t * s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    return httpd_resp_send(req, (const char *)index_ov3660_html_gz, index_ov3660_html_gz_len);
  }
  return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
}

// Halaman manajemen pengguna (List face_xxx.bin dari SD dan tampilkan tabel)
static esp_err_t users_handler(httpd_req_t *req) {
  String rows = "";
  bool found = false;
  File root = SD_MMC.open("/");
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    file.close();
    if (name.startsWith("/face_") && name.endsWith(".bin")) {
      int id = name.substring(6, name.length() - 4).toInt();
      found = true;
      rows += "<tr>";
      rows += "<td>" + String(id) + "</td>";
      rows += "</tr>";
    }
    file = root.openNextFile();
  }
  root.close();
  if (!found) {
    rows = "<tr><td colspan='2'>Belum ada pengguna</td></tr>";
  }
  String html = R"rawliteral(
  <!DOCTYPE html>
  <html>
  <head>
    <title>List Wajah Pengguna</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
      body {
        margin: 0;
        height: 100vh;
        font-family: Arial;
        background: #111;
        color: white;
        display: flex;
        justify-content: center;
        align-items: center;
      }
      .container {
        width: 100%;
        max-width: 600px;
        padding: 24px;
        text-align: center;
      }
      table {
        width: 100%;
        border-collapse: collapse;
        margin-top: 24px;
        margin-bottom: 40px; /* JARAK KE TOMBOL */
      }
      th, td {
        border: 1px solid #444;
        padding: 12px;
        text-align: center;
      }
      th {
        background: #1e1e1e;
      }
      .action-bar {
        display: flex;
        justify-content: center;
        gap: 18px;          /* GAP ANTAR TOMBOL */
        margin-top: 10px;
      }
      button {
        padding: 10px 18px;
        min-width: 140px;
        border: none;
        border-radius: 8px;
        color: white;
        cursor: pointer;
        font-size: 14px;
        transition: background 0.2s ease;
      }
      .btn-danger {
        background: #e53935;
      }
      .btn-danger:hover {
        background: #c62828;
      }
      .btn-blue {
        background: #2196F3;
      }
      .btn-blue:hover {
        background: #1976D2;
      }
      .btn-green {
        background: #4CAF50;
      }
      .btn-green:hover {
        background: #388E3C;
      }
      a {
        text-decoration: none;
      }
    </style>
  </head>
  <body>
    <div class="container">
      <h2>List Wajah Pengguna</h2>
      <table>
        <tr>
          <th>ID Pengguna</th>
  )rawliteral";
    html += rows;
    html += R"rawliteral(
      </table>
      <div class="action-bar">
        <button class="btn-green" onclick="location.href='/'">
          Kembali ke Menu
        </button>
        <button class="btn-blue" onclick="location.href='/enroll'">
          Daftarkan Wajah
        </button>
        <button class="btn-blue" onclick="location.href='/stream'">
          Verifikasi Wajah
        </button>
      </div>
    </div>
  </body>
  </html>
  )rawliteral";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html.c_str(), html.length());
}

static ra_filter_t * ra_filter_init(ra_filter_t * filter, size_t sample_size){
  memset(filter, 0, sizeof(ra_filter_t));
  filter->values = (int *)malloc(sample_size * sizeof(int));
  if(!filter->values){
    return NULL;
  }
  memset(filter->values, 0, sample_size * sizeof(int));
  filter->size = sample_size;
  return filter;
}

static int ra_filter_run(ra_filter_t * filter, int value){
  if(!filter->values){
    return value;
  }
  filter->sum -= filter->values[filter->index];
  filter->values[filter->index] = value;
  filter->sum += filter->values[filter->index];
  filter->index++;
  filter->index = filter->index % filter->size;
  if (filter->count < filter->size) {
    filter->count++;
  }
  return filter->sum / filter->count;
}

static void rgb_print(dl_matrix3du_t *image_matrix, uint32_t color, const char * str){
  fb_data_t fb;
  fb.width = image_matrix->w;
  fb.height = image_matrix->h;
  fb.data = image_matrix->item;
  fb.bytes_per_pixel = 3;
  fb.format = FB_BGR888;
  fb_gfx_print(&fb, (fb.width - (strlen(str) * 14)) / 2, 10, color, str);
}

static int rgb_printf(dl_matrix3du_t *image_matrix, uint32_t color, const char *format, ...){
  char loc_buf[64];
  char * temp = loc_buf;
  int len;
  va_list arg;
  va_list copy;
  va_start(arg, format);
  va_copy(copy, arg);
  len = vsnprintf(loc_buf, sizeof(loc_buf), format, arg);
  va_end(copy);
  if(len >= sizeof(loc_buf)){
    temp = (char*)malloc(len+1);
    if(temp == NULL) {
      return 0;
    }
  }
  vsnprintf(temp, len+1, format, arg);
  va_end(arg);
  rgb_print(image_matrix, color, temp);
  if(len > 64){
    free(temp);
  }
  return len;
}

static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes, int face_id){
  int x, y, w, h, i;
  uint32_t color = FACE_COLOR_YELLOW;
  if (face_db_full) {
    color = FACE_COLOR_PURPLE;
  } else if (face_id < 0) {
      color = FACE_COLOR_RED;
  } else if (face_id > 0) {
      color = FACE_COLOR_GREEN;
  }
  fb_data_t fb;
  fb.width = image_matrix->w;
  fb.height = image_matrix->h;
  fb.data = image_matrix->item;
  fb.bytes_per_pixel = 3;
  fb.format = FB_BGR888;
  for (i = 0; i < boxes->len; i++){
    if (face_id == -2) {
      rgb_print(image_matrix, FACE_COLOR_PURPLE, "MAX FACE REACHED");
    }
    x = (int)boxes->box[i].box_p[0];
    y = (int)boxes->box[i].box_p[1];
    w = (int)boxes->box[i].box_p[2] - x + 1;
    h = (int)boxes->box[i].box_p[3] - y + 1;
    fb_gfx_drawFastHLine(&fb, x, y, w, color);
    fb_gfx_drawFastHLine(&fb, x, y+h-1, w, color);
    fb_gfx_drawFastVLine(&fb, x, y, h, color);
    fb_gfx_drawFastVLine(&fb, x+w-1, y, h, color);
    #if 0
      int x0, y0, j;
      for (j = 0; j < 10; j+=2) {
          x0 = (int)boxes->landmark[i].landmark_p[j];
          y0 = (int)boxes->landmark[i].landmark_p[j+1];
          fb_gfx_fillRect(&fb, x0, y0, 3, 3, color);
      }
    #endif
  }
}

// Detect → Align → Enroll / Recognize
static int run_face_recognition(dl_matrix3du_t *image_matrix, box_array_t *net_boxes){
    dl_matrix3du_t *aligned_face = NULL;
    int matched_id = 0;

    aligned_face = dl_matrix3du_alloc(1, FACE_WIDTH, FACE_HEIGHT, 3);
    if(!aligned_face){
        Serial.println("❌ Could not allocate face buffer");
        return matched_id;
    }

    if (align_face(net_boxes, image_matrix, aligned_face) == ESP_OK){

        /* ------------ ENROLL ------------ */
        if (is_enrolling){

            // STOP TOTAL kalau DB penuh
            if (id_list.count >= FACE_ID_SAVE_NUMBER){
                face_db_full = true;
                is_enrolling = 0;
                rgb_print(image_matrix, FACE_COLOR_RED, "FACE DB FULL");
                goto cleanup;
            }

            // Hanya boleh 1 wajah saat enroll
            if (!net_boxes || net_boxes->len != 1){
                Serial.println("⚠️ Enroll but face count != 1 (skip)");
                goto cleanup;
            }
            int8_t left_sample_face = enroll_face(&id_list, aligned_face);
            Serial.printf(
              "Enrolling Face ID: %d sample %d\n",
              id_list.tail + 1,
              ENROLL_CONFIRM_TIMES - left_sample_face
            );
            rgb_printf(
              image_matrix,
              FACE_COLOR_CYAN,
              "ID[%u] Sample[%u]",
              id_list.tail + 1,
              ENROLL_CONFIRM_TIMES - left_sample_face
            );

            // ENROLL SELESAI
            if (left_sample_face == 0){
                is_enrolling = 0;

                mqttClient.publish(topic_enroll_confirm, "Enroll_Face_Finished");
                enrollTrigger = false; 
                streamTrigger = false;

                // index terakhir di RAM
                int face_index = id_list.count - 1;
                if (face_index < 0 || face_index >= id_list.size){
                    Serial.println("❌ Invalid face index, abort save");
                    goto cleanup;
                }

                // Gunakan ID file sesuai tail + 1
                int user_id = 0;
                for (int i = 1; i <= FACE_ID_SAVE_NUMBER; i++) {
                    char path[32];
                    sprintf(path, "/face_%d.bin", i);
                    if (!SD_MMC.exists(path)) {
                        user_id = i;
                        break;
                    }
                }
                if (user_id == 0) {
                    face_db_full = true;
                    Serial.println("❌ FACE DB FULL, cannot assign ID");
                    goto cleanup;
                }

                // Simpan ke SD
                char path[32];
                sprintf(path, "/face_%d.bin", user_id);
                File f = SD_MMC.open(path, FILE_WRITE);
                if(f){
                    f.write((uint8_t*)id_list.id_list[face_index]->item, FACE_ID_SIZE * sizeof(float));
                    f.close();
                    face_index_to_user_id[face_index] = user_id;  // mapping RAM → SD
                    Serial.printf("✅ Face ID %d saved to SD\n", user_id);
                } else {
                    Serial.println("❌ Failed to open SD file for saving");
                }
            }
        }

        /* ------------ RECOGNIZE ------------ */
        else {
          matched_id = recognize_face(&id_list, aligned_face);
          if (matched_id >= 0 && matched_id < id_list.count){
              int userId = face_index_to_user_id[matched_id];
              rgb_printf(image_matrix, FACE_COLOR_GREEN, "Hello %u", userId);

              // SET HASIL (dibaca .ino → publish MQTT)
              matchFaceStatus = true;
              matchFaceId = userId;
          } else {
              rgb_print(image_matrix, FACE_COLOR_RED, "Intruder!");

              // Wajah tidak dikenal
              matched_id = -1;

              // SET HASIL (dibaca .ino → publish MQTT)
              matchFaceStatus = false;
              matchFaceId = 0;
          }
        }
    }
    else {
        if (is_enrolling){
            Serial.println("⚠️ Align failed, sample skipped");
        }
    }
cleanup:
    dl_matrix3du_free(aligned_face);
    return matched_id;
}


static size_t jpg_encode_stream(void * arg, size_t index, const void* data, size_t len){
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if(!index){
    j->len = 0;
  }
  if(httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  int64_t fr_start = esp_timer_get_time();
  fb = esp_camera_fb_get();
  if (!fb) {
      Serial.println("Camera capture failed");
      httpd_resp_send_500(req);
      return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  size_t out_len, out_width, out_height;
  uint8_t * out_buf;
  bool s;
  bool detected = false;
  int face_id = 0;
  if(!detection_enabled || fb->width > 400){
      size_t fb_len = 0;
      if(fb->format == PIXFORMAT_JPEG){
          fb_len = fb->len;
          res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
      } else {
          jpg_chunking_t jchunk = {req, 0};
          res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk)?ESP_OK:ESP_FAIL;
          httpd_resp_send_chunk(req, NULL, 0);
          fb_len = jchunk.len;
      }
      esp_camera_fb_return(fb);
      int64_t fr_end = esp_timer_get_time();
      Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start)/1000));
      return res;
  }
  dl_matrix3du_t *image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
  if (!image_matrix) {
      esp_camera_fb_return(fb);
      Serial.println("dl_matrix3du_alloc failed");
      httpd_resp_send_500(req);
      return ESP_FAIL;
  }
  out_buf = image_matrix->item;
  out_len = fb->width * fb->height * 3;
  out_width = fb->width;
  out_height = fb->height;
  s = fmt2rgb888(fb->buf, fb->len, fb->format, out_buf);
  esp_camera_fb_return(fb);
  if(!s){
      dl_matrix3du_free(image_matrix);
      Serial.println("to rgb888 failed");
      httpd_resp_send_500(req);
      return ESP_FAIL;
  }
  box_array_t *net_boxes = face_detect(image_matrix, &mtmn_config);
  if (net_boxes){
      detected = true;
      if(recognition_enabled){
          face_id = run_face_recognition(image_matrix, net_boxes);
      }
      draw_face_boxes(image_matrix, net_boxes, face_id);
      free(net_boxes->score);
      free(net_boxes->box);
      free(net_boxes->landmark);
      free(net_boxes);
  }
  jpg_chunking_t jchunk = {req, 0};
  s = fmt2jpg_cb(out_buf, out_len, out_width, out_height, PIXFORMAT_RGB888, 90, jpg_encode_stream, &jchunk);
  dl_matrix3du_free(image_matrix);
  if(!s){
      Serial.println("JPEG compression failed");
      return ESP_FAIL;
  }
  int64_t fr_end = esp_timer_get_time();
  Serial.printf("FACE: %uB %ums %s%d\n", (uint32_t)(jchunk.len), (uint32_t)((fr_end - fr_start)/1000), detected?"DETECTED ":"", face_id);
  return res;
}

// Halaman cek wajah (Trigger dengan MQTT)
static esp_err_t stream_handler(httpd_req_t *req){
  // BLOKIR CEK WAJAH jika MQTT masih false (perlu refresh web untuk kondisi terkini)
  if (!streamTrigger) {
    const char* msg = "STREAM DISABLED - MQTT trigger is false";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, msg, strlen(msg));
  }

  // Berikan Akses HALAMAN CEK WAJAH jika MQTT sudah true (perlu refresh web untuk kondisi terkini)
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];
  dl_matrix3du_t *image_matrix = NULL;
  bool detected = false;
  int face_id = 0;
  int64_t fr_start = 0;
  int64_t fr_ready = 0;
  int64_t fr_face = 0;
  int64_t fr_recognize = 0;
  int64_t fr_encode = 0;
  static int64_t last_frame = 0;
  if(!last_frame) {
      last_frame = esp_timer_get_time();
  }
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
      return res;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  while(true){
    if (!streamTrigger) {
      Serial.println("[STREAM] Trigger Off -> stop streaming");
      break;
    }
      detected = false;
      face_id = 0;
      fb = esp_camera_fb_get();
      if (!fb) {
          Serial.println("Camera capture failed");
          res = ESP_FAIL;
      } else {
          fr_start = esp_timer_get_time();
          fr_ready = fr_start;
          fr_face = fr_start;
          fr_encode = fr_start;
          fr_recognize = fr_start;
          if(!detection_enabled || fb->width > 400){
              if(fb->format != PIXFORMAT_JPEG){
                  bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
                  esp_camera_fb_return(fb);
                  fb = NULL;
                  if(!jpeg_converted){
                      Serial.println("JPEG compression failed");
                      res = ESP_FAIL;
                  }
              } else {
                  _jpg_buf_len = fb->len;
                  _jpg_buf = fb->buf;
              }
          } else {
              image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
              if (!image_matrix) {
                  Serial.println("dl_matrix3du_alloc failed");
                  res = ESP_FAIL;
              } else {
                  if(!fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item)){
                      Serial.println("fmt2rgb888 failed");
                      res = ESP_FAIL;
                  } else {
                      fr_ready = esp_timer_get_time();
                      box_array_t *net_boxes = NULL;
                      if(detection_enabled){
                          net_boxes = face_detect(image_matrix, &mtmn_config);
                      }
                      fr_face = esp_timer_get_time();
                      fr_recognize = fr_face;
                      if (net_boxes || fb->format != PIXFORMAT_JPEG){
                          if(net_boxes){
                              detected = true;
                              if(recognition_enabled){
                                  face_id = run_face_recognition(image_matrix, net_boxes);
                              }
                              fr_recognize = esp_timer_get_time();
                              draw_face_boxes(image_matrix, net_boxes, face_id);
                              free(net_boxes->score);
                              free(net_boxes->box);
                              free(net_boxes->landmark);
                              free(net_boxes);
                          }
                          if(!fmt2jpg(image_matrix->item, fb->width*fb->height*3, fb->width, fb->height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len)){
                              Serial.println("fmt2jpg failed");
                              res = ESP_FAIL;
                          }
                          esp_camera_fb_return(fb);
                          fb = NULL;
                      } else {
                          _jpg_buf = fb->buf;
                          _jpg_buf_len = fb->len;
                      }
                      fr_encode = esp_timer_get_time();
                  }
                  dl_matrix3du_free(image_matrix);
              }
          }
      }
      if(res == ESP_OK){
          size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
          res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
      }
      if(res == ESP_OK){
          res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
      }
      if(res == ESP_OK){
          res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      }
      if(fb){
          esp_camera_fb_return(fb);
          fb = NULL;
          _jpg_buf = NULL;
      } else if(_jpg_buf){
          free(_jpg_buf);
          _jpg_buf = NULL;
      }
      if(res != ESP_OK){
          break;
      }
      int64_t fr_end = esp_timer_get_time();
      int64_t ready_time = (fr_ready - fr_start)/1000;
      int64_t face_time = (fr_face - fr_ready)/1000;
      int64_t recognize_time = (fr_recognize - fr_face)/1000;
      int64_t encode_time = (fr_encode - fr_recognize)/1000;
      int64_t process_time = (fr_encode - fr_start)/1000;
      int64_t frame_time = fr_end - last_frame;
      last_frame = fr_end;
      frame_time /= 1000;
      uint32_t avg_frame_time = ra_filter_run(&ra_filter, frame_time);
      Serial.printf("MJPG: %uB %ums (%.1ffps), AVG: %ums (%.1ffps), %u+%u+%u+%u=%u %s%d\n",
          (uint32_t)(_jpg_buf_len),
          (uint32_t)frame_time, 1000.0 / (uint32_t)frame_time,
          avg_frame_time, 1000.0 / avg_frame_time,
          (uint32_t)ready_time, (uint32_t)face_time, (uint32_t)recognize_time, (uint32_t)encode_time, (uint32_t)process_time,
          (detected)?"DETECTED ":"", face_id
      );
  }
  last_frame = 0;
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req){
  char*  buf;
  size_t buf_len;
  char variable[32] = {0,};
  char value[32] = {0,};
  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
      buf = (char*)malloc(buf_len);
      if(!buf){
          httpd_resp_send_500(req);
          return ESP_FAIL;
      }
      if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
          if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
              httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
          } else {
              free(buf);
              httpd_resp_send_404(req);
              return ESP_FAIL;
          }
      } else {
          free(buf);
          httpd_resp_send_404(req);
          return ESP_FAIL;
      }
      free(buf);
  } else {
      httpd_resp_send_404(req);
      return ESP_FAIL;
  }
  int val = atoi(value);
  sensor_t * s = esp_camera_sensor_get();
  int res = 0;
  if(!strcmp(variable, "framesize")) {
      if(s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
  }
  else if(!strcmp(variable, "quality")) res = s->set_quality(s, val);
  else if(!strcmp(variable, "contrast")) res = s->set_contrast(s, val);
  else if(!strcmp(variable, "brightness")) res = s->set_brightness(s, val);
  else if(!strcmp(variable, "saturation")) res = s->set_saturation(s, val);
  else if(!strcmp(variable, "gainceiling")) res = s->set_gainceiling(s, (gainceiling_t)val);
  else if(!strcmp(variable, "colorbar")) res = s->set_colorbar(s, val);
  else if(!strcmp(variable, "awb")) res = s->set_whitebal(s, val);
  else if(!strcmp(variable, "agc")) res = s->set_gain_ctrl(s, val);
  else if(!strcmp(variable, "aec")) res = s->set_exposure_ctrl(s, val);
  else if(!strcmp(variable, "hmirror")) res = s->set_hmirror(s, val);
  else if(!strcmp(variable, "vflip")) res = s->set_vflip(s, val);
  else if(!strcmp(variable, "awb_gain")) res = s->set_awb_gain(s, val);
  else if(!strcmp(variable, "agc_gain")) res = s->set_agc_gain(s, val);
  else if(!strcmp(variable, "aec_value")) res = s->set_aec_value(s, val);
  else if(!strcmp(variable, "aec2")) res = s->set_aec2(s, val);
  else if(!strcmp(variable, "dcw")) res = s->set_dcw(s, val);
  else if(!strcmp(variable, "bpc")) res = s->set_bpc(s, val);
  else if(!strcmp(variable, "wpc")) res = s->set_wpc(s, val);
  else if(!strcmp(variable, "raw_gma")) res = s->set_raw_gma(s, val);
  else if(!strcmp(variable, "lenc")) res = s->set_lenc(s, val);
  else if(!strcmp(variable, "special_effect")) res = s->set_special_effect(s, val);
  else if(!strcmp(variable, "wb_mode")) res = s->set_wb_mode(s, val);
  else if(!strcmp(variable, "ae_level")) res = s->set_ae_level(s, val);
  else if(!strcmp(variable, "face_detect")) {
      detection_enabled = 1;
      if(!detection_enabled) {
          recognition_enabled = 1;
      }
  }
  else if(!strcmp(variable, "face_enroll")) {
      if (id_list.count >= FACE_ID_SAVE_NUMBER) {
          Serial.println("❌ Cannot enroll face database full");
          is_enrolling = 0;
      } else {
          is_enrolling = val;
      }
  }
  else if(!strcmp(variable, "face_recognize")) {
      recognition_enabled = 1;
      if(recognition_enabled){
          detection_enabled = 1;
      }
  }
  else {
      res = -1;
  }
  if(res){
      return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req){
  static char json_response[1024];
  sensor_t * s = esp_camera_sensor_get();
  char * p = json_response;
  *p++ = '{';
  p+=sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p+=sprintf(p, "\"quality\":%u,", s->status.quality);
  p+=sprintf(p, "\"brightness\":%d,", s->status.brightness);
  p+=sprintf(p, "\"contrast\":%d,", s->status.contrast);
  p+=sprintf(p, "\"saturation\":%d,", s->status.saturation);
  p+=sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
  p+=sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
  p+=sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
  p+=sprintf(p, "\"awb\":%u,", s->status.awb);
  p+=sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
  p+=sprintf(p, "\"aec\":%u,", s->status.aec);
  p+=sprintf(p, "\"aec2\":%u,", s->status.aec2);
  p+=sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
  p+=sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
  p+=sprintf(p, "\"agc\":%u,", s->status.agc);
  p+=sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
  p+=sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
  p+=sprintf(p, "\"bpc\":%u,", s->status.bpc);
  p+=sprintf(p, "\"wpc\":%u,", s->status.wpc);
  p+=sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
  p+=sprintf(p, "\"lenc\":%u,", s->status.lenc);
  p+=sprintf(p, "\"vflip\":%u,", s->status.vflip);
  p+=sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
  p+=sprintf(p, "\"dcw\":%u,", s->status.dcw);
  p+=sprintf(p, "\"colorbar\":%u,", s->status.colorbar);
  p+=sprintf(p, "\"face_detect\":%u,", detection_enabled);
  p+=sprintf(p, "\"face_enroll\":%u,", is_enrolling);
  p+=sprintf(p, "\"face_recognize\":%u", recognition_enabled);
  *p++ = '}';
  *p++ = 0;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

// Inisialisasi Web Server
void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  httpd_uri_t index_uri = {
    // Halaman Menu Utama
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t stream_uri = {
    // Halaman Cek Wajah
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t enroll_uri = {
    // Halaman Daftarkan Wajah
    .uri       = "/enroll",
    .method    = HTTP_GET,
    .handler   = enroll_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t users_uri = {
    // Halaman Manajemen Pengguna
    .uri       = "/users",
    .method    = HTTP_GET,
    .handler   = users_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t cmd_uri = {
    .uri       = "/control",
    .method    = HTTP_GET,
    .handler   = cmd_handler,
    .user_ctx  = NULL
  };
  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };
  ra_filter_init(&ra_filter, 20);
  mtmn_config.type = FAST;
  mtmn_config.min_face = 80;
  mtmn_config.pyramid = 0.707;
  mtmn_config.pyramid_times = 4;
  mtmn_config.p_threshold.score = 0.6;
  mtmn_config.p_threshold.nms = 0.7;
  mtmn_config.p_threshold.candidate_number = 20;
  mtmn_config.r_threshold.score = 0.7;
  mtmn_config.r_threshold.nms = 0.7;
  mtmn_config.r_threshold.candidate_number = 10;
  mtmn_config.o_threshold.score = 0.7;
  mtmn_config.o_threshold.nms = 0.7;
  mtmn_config.o_threshold.candidate_number = 1;
  face_id_init(&id_list, FACE_ID_SAVE_NUMBER, ENROLL_CONFIRM_TIMES);
  load_faces_from_sd();
  sensor_t * s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, 0);
    s->set_vflip(s, 0);
  }
  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &enroll_uri);
    httpd_register_uri_handler(camera_httpd, &users_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
  }
  config.server_port += 1;
  config.ctrl_port += 1;
  Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}
