#include "face_detection.h"

#if defined(USE_ESP_IDF) && defined(USE_ESP32_VARIANT_ESP32P4)

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"

#include "human_face_detect.hpp"
#ifdef USE_FACE_DETECTION_RECOGNITION
#include "human_face_recognition.hpp"
#endif

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <list>

namespace esphome::face_detection {

static const char *const TAG = "face_detection";

// The detector runs here, not in loop(): an inference takes tens to hundreds
// of milliseconds, and the main loop also draws the display. Same priority as
// the main loop and no core of its own, so the two share whatever the radio
// leaves. esp-dl's own examples run inference from app_main's default stack;
// this is several times that.
static constexpr uint32_t TASK_STACK_SIZE = 16384;
static constexpr UBaseType_t TASK_PRIORITY = 1;
// With nothing to do the task still wakes this often, so an enroll or a delete
// asked for while the camera is off is not left waiting for a frame.
static constexpr uint32_t TASK_IDLE_WAKE_MS = 500;
// The detector never sees more than this along a frame's long side. Every
// model resizes its input far below it anyway (MSR to 160x120, ESPDet to 224
// or 416 square); what the margin keeps is enough of the face for the
// landmark stage and for recognition, which crop from this copy.
static constexpr uint32_t JOB_LONG_SIDE = 800;
// Boxes stay on the picture this long without a detection to confirm them, so
// a stalled task cannot leave a rectangle frozen in front of an empty room.
static constexpr int64_t BOXES_VALID_US = 1500000;
// Detections in a row that find nobody before a recognized face counts as
// gone. One missed detection is common -- a turned head, motion blur -- and
// would otherwise recognize the same person, and fire the trigger, again.
static constexpr int GONE_AFTER_EMPTY_RUNS = 3;
// How long the task waits for the SD card (or wherever the models and the face
// database live) to be mounted.
static constexpr uint32_t STORAGE_WAIT_MS = 30000;
static constexpr uint32_t REPORT_INTERVAL_MS = 30000;

static constexpr uint16_t COLOR_GREEN = 0x07E0;
static constexpr uint16_t COLOR_BLUE = 0x001F;
static constexpr uint16_t COLOR_RED = 0xF800;
static constexpr uint16_t COLOR_WHITE = 0xFFFF;
static constexpr uint16_t COLOR_BLACK = 0x0000;

static const char *const MODEL_NAMES[] = {"msrmnp_s8_v1", "espdet_pico_224_224_face", "espdet_pico_416_416_face"};

namespace {

/// The lock the task and the main loop share, taken for a few copies at most.
class Guard {
 public:
  explicit Guard(SemaphoreHandle_t lock, TickType_t wait = portMAX_DELAY) : lock_(lock) {
    this->held_ = lock != nullptr && xSemaphoreTake(lock, wait) == pdTRUE;
  }
  ~Guard() {
    if (this->held_)
      xSemaphoreGive(this->lock_);
  }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
  bool held() const { return this->held_; }

 private:
  SemaphoreHandle_t lock_;
  bool held_{false};
};

int64_t now_us() { return esp_timer_get_time(); }

// opendir() rather than stat(): FatFs cannot stat the root of a volume, so
// stat("/sdcard") fails on a card that is mounted and perfectly usable.
bool dir_exists(const std::string &dir) {
  DIR *d = opendir(dir.c_str());
  if (d == nullptr)
    return false;
  closedir(d);
  return true;
}

bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/// "/sdcard/faces/db.bin" -> "/sdcard": the mount point a path lives on.
std::string mount_point(const std::string &path) {
  if (path.size() < 2 || path[0] != '/')
    return "";
  const size_t slash = path.find('/', 1);
  return slash == std::string::npos ? path : path.substr(0, slash);
}

bool wait_for_dir(const std::string &dir, uint32_t timeout_ms) {
  const int64_t until = now_us() + (int64_t) timeout_ms * 1000;
  while (!dir_exists(dir)) {
    if (now_us() >= until)
      return false;
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return true;
}

/// Makes every folder above `path` that is missing, below its mount point.
bool make_parent_dirs(const std::string &path) {
  const size_t last = path.rfind('/');
  if (last == std::string::npos || last == 0)
    return true;
  const std::string parent = path.substr(0, last);
  size_t pos = mount_point(path).size();
  while (pos < parent.size()) {
    pos = parent.find('/', pos + 1);
    const std::string level = parent.substr(0, pos);
    if (!dir_exists(level) && mkdir(level.c_str(), 0755) != 0) {
      ESP_LOGE(TAG, "Cannot create folder %s", level.c_str());
      return false;
    }
  }
  return true;
}

#ifdef USE_FACE_DETECTION_RECOGNITION
/// The feature length a face database on disk was written with, or 0 when the
/// file is not one esp-dl would read back correctly.
///
/// esp-dl's DataBase is three uint16 (faces ever enrolled, faces still valid,
/// floats per face), then per face a uint16 id and the floats. When reading
/// one fails it logs and carries on with whatever it had read, and every
/// enroll after that is refused -- so a damaged file is set aside here, before
/// the recognizer opens it.
uint16_t sound_database_feat_len(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return 0;
  uint16_t meta[3] = {0, 0, 0};
  const size_t got = fread(meta, sizeof(uint16_t), 3, f);
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fclose(f);
  if (got != 3 || meta[2] == 0 || meta[1] > meta[0])
    return 0;
  const long expected = 6L + (long) meta[0] * (2L + 4L * meta[2]);
  return size == expected ? meta[2] : 0;
}

void set_database_aside(const std::string &path) {
  const std::string aside = path + ".bad";
  std::remove(aside.c_str());
  if (std::rename(path.c_str(), aside.c_str()) == 0) {
    ESP_LOGW(TAG, "Set the unreadable face database aside as %s; starting an empty one", aside.c_str());
  } else {
    std::remove(path.c_str());
    ESP_LOGW(TAG, "Removed the unreadable face database %s; starting an empty one", path.c_str());
  }
}
#endif  // USE_FACE_DETECTION_RECOGNITION

// 5x7 capitals and digits, for the name drawn over a recognized face.
const uint8_t FONT_5X7[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},  // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},  // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E},  // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},  // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
    {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},  // -
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},  // .
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},  // '
};

int glyph_index(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a';
  if (c >= '0' && c <= '9')
    return 26 + (c - '0');
  if (c == '-')
    return 36;
  if (c == '.')
    return 37;
  if (c == '\'')
    return 38;
  return -1;  // a space, or anything the font does not have: left blank
}

/// A name as the 5x7 font can draw it. The font has no accents, and a name
/// typed in French is full of them: each UTF-8 letter of Latin-1 is drawn as
/// its plain letter, so "Hélène" reads HELENE rather than H L NE. Anything
/// else outside ASCII is dropped.
std::string fold_for_font(const std::string &text) {
  // U+00C0 to U+00FF, as the letter each one is drawn as ('\0': dropped).
  static const char LATIN1[] = "AAAAAAACEEEEIIII"   // C0-CF
                               "DNOOOOO\0OUUUUY\0\0"  // D0-DF
                               "AAAAAAACEEEEIIII"   // E0-EF
                               "DNOOOOO\0OUUUUY\0Y";   // F0-FF
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); i++) {
    const auto c = (uint8_t) text[i];
    if (c < 0x80) {
      out += (char) c;
    } else if ((c == 0xC3) && i + 1 < text.size() && ((uint8_t) text[i + 1] & 0xC0) == 0x80) {
      const char plain = LATIN1[(uint8_t) text[i + 1] - 0x80];
      if (plain != '\0')
        out += plain;
      i++;
    } else if ((c == 0xC5) && i + 1 < text.size() && ((uint8_t) text[i + 1] == 0x92 || (uint8_t) text[i + 1] == 0x93)) {
      out += "OE";  // Œ, œ
      i++;
    }
    // Any other byte above 0x7F is part of a character the font cannot draw.
  }
  return out;
}

/// The width in pixels of `text` drawn at `scale`, the last column of
/// spacing left out.
int text_width(const std::string &text, int scale) { return text.empty() ? 0 : (int) text.size() * 6 * scale - scale; }

/// A filled rectangle, clipped to the frame. Rows are `stride` bytes apart.
void fill_rect(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, int x1, int y1, int x2, int y2,
               uint16_t color) {
  x1 = std::max(x1, 0);
  y1 = std::max(y1, 0);
  x2 = std::min(x2, (int) width);
  y2 = std::min(y2, (int) height);
  if (x2 <= x1)
    return;
  for (int y = y1; y < y2; y++) {
    auto *row = reinterpret_cast<uint16_t *>(pixels + (size_t) y * stride);
    std::fill(row + x1, row + x2, color);
  }
}

void frame_rect(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, int x1, int y1, int x2, int y2,
                int thickness, uint16_t color) {
  fill_rect(pixels, width, height, stride, x1, y1, x2, y1 + thickness, color);
  fill_rect(pixels, width, height, stride, x1, y2 - thickness, x2, y2, color);
  fill_rect(pixels, width, height, stride, x1, y1, x1 + thickness, y2, color);
  fill_rect(pixels, width, height, stride, x2 - thickness, y1, x2, y2, color);
}

}  // namespace

// ===========================================================================
// Setup
// ===========================================================================
void FaceDetectionComponent::set_unknown_label(const std::string &label) {
  this->unknown_label_ = fold_for_font(label);
}

#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
void FaceDetectionComponent::set_camera(esp_video_camera::ESPVideoCamera *camera) {
  this->camera_ = camera;
  camera->add_raw_frame_listener(this);
}
#endif

void FaceDetectionComponent::setup() {
  if (this->camera_ == nullptr) {
    ESP_LOGE(TAG, "No camera");
    this->mark_failed();
    return;
  }
#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
  if (!this->camera_->has_raw_frames()) {
    ESP_LOGE(TAG, "The camera delivers JPEG (a USB camera, or a device already encoding), so there are no pixels to "
                  "look for faces in. Face detection needs the MIPI-CSI sensor: device: jpeg.");
    this->mark_failed();
    return;
  }
#endif
  this->lock_ = xSemaphoreCreateMutex();
  this->boxes_.reserve(8);
  if (this->lock_ == nullptr ||
      xTaskCreatePinnedToCore(FaceDetectionComponent::task_entry_, "face_detect", TASK_STACK_SIZE, this, TASK_PRIORITY,
                              &this->task_, tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE(TAG, "Cannot start the detection task");
    this->mark_failed();
    return;
  }
  this->report_at_ms_ = millis() + REPORT_INTERVAL_MS;
}

void FaceDetectionComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Face Detection:");
#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
  ESP_LOGCONFIG(TAG, "  Camera: esp_video_camera (frames seen before the display and the encoder)");
#endif
#ifdef USE_FACE_DETECTION_ESP_CAM_SENSOR
  ESP_LOGCONFIG(TAG, "  Camera: esp_cam_sensor");
#endif
  ESP_LOGCONFIG(TAG, "  Model: %s (Espressif human_face_detect)", MODEL_NAMES[std::clamp(this->model_, 0, 2)]);
  if (!this->model_dir_.empty()) {
    ESP_LOGCONFIG(TAG, "  Models read from: %s", this->model_dir_.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Models in flash");
  }
  ESP_LOGCONFIG(TAG,
                "  Score threshold: %.2f\n"
                "  NMS threshold: %.2f\n"
                "  Detection: one frame in %u, at most %u px on the long side\n"
                "  Draw boxes: %s, labels: %s (unknown face: '%s')",
                this->score_threshold_, this->nms_threshold_, (unsigned) this->detection_interval_,
                (unsigned) JOB_LONG_SIDE, YESNO(this->draw_enabled_), YESNO(this->draw_labels_),
                this->unknown_label_.c_str());
#ifdef USE_FACE_DETECTION_RECOGNITION
  ESP_LOGCONFIG(TAG,
                "  Recognition: YES (Espressif human_face_recognition)\n"
                "  Face database: %s\n"
                "  Recognition threshold: %.2f",
                this->face_db_path_.c_str(), this->recognition_threshold_);
#else
  ESP_LOGCONFIG(TAG, "  Recognition: NO");
#endif
}

// ===========================================================================
// The detection task
// ===========================================================================
void FaceDetectionComponent::task_entry_(void *arg) { static_cast<FaceDetectionComponent *>(arg)->task_main_(); }

void FaceDetectionComponent::task_main_() {
  if (!this->load_models_()) {
    this->failed_.store(true, std::memory_order_release);
    this->task_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  this->ready_.store(true, std::memory_order_release);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TASK_IDLE_WAKE_MS));
    this->serve_requests_();
    if (this->busy_.load(std::memory_order_acquire)) {
      this->run_job_();
      this->busy_.store(false, std::memory_order_release);
    }
  }
}

bool FaceDetectionComponent::load_models_() {
  if (!this->model_dir_.empty()) {
    if (!wait_for_dir(this->model_dir_, STORAGE_WAIT_MS)) {
      ESP_LOGE(TAG, "The model folder %s did not appear within %us; is the SD card mounted?", this->model_dir_.c_str(),
               (unsigned) (STORAGE_WAIT_MS / 1000));
      return false;
    }
    // esp-dl opens these by name and does not survive a missing one, so look
    // for them first and say which is missing.
    std::vector<const char *> needed;
    if (this->model_ == 0) {
      needed = {"human_face_detect_msr_s8_v1.espdl", "human_face_detect_mnp_s8_v1.espdl"};
    } else if (this->model_ == 1) {
      needed = {"espdet_pico_224_224_face.espdl"};
    } else {
      needed = {"espdet_pico_416_416_face.espdl"};
    }
#ifdef USE_FACE_DETECTION_RECOGNITION
    needed.push_back("human_face_feat_mfn_s8_v1.espdl");
#endif
    bool all_there = true;
    for (const char *name : needed) {
      const std::string path = this->model_dir_ + "/" + name;
      if (!file_exists(path)) {
        ESP_LOGE(TAG, "Model file %s is missing. The files are in esp-dl's models/*/models/p4 folders.", path.c_str());
        all_there = false;
      }
    }
    if (!all_there)
      return false;
  }

  const size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const int64_t started = now_us();
  this->detector_ = new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(this->model_), false);
  this->detector_->set_score_thr(this->score_threshold_);
  this->detector_->set_nms_thr(this->nms_threshold_);
  const size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ESP_LOGI(TAG, "Loaded %s in %u ms (%u KB of PSRAM)", MODEL_NAMES[std::clamp(this->model_, 0, 2)],
           (unsigned) ((now_us() - started) / 1000),
           (unsigned) ((psram_before > psram_after ? psram_before - psram_after : 0) / 1024));

#ifdef USE_FACE_DETECTION_RECOGNITION
  if (!this->open_recognizer_()) {
    // Detection still works without it; only the recognition half is lost.
    ESP_LOGE(TAG, "Face recognition is off: the face database cannot be opened");
  }
#endif
  return true;
}

#ifdef USE_FACE_DETECTION_RECOGNITION
bool FaceDetectionComponent::open_recognizer_() {
  const std::string &path = this->face_db_path_;
  const std::string root = mount_point(path);
  if (!root.empty() && !wait_for_dir(root, STORAGE_WAIT_MS)) {
    ESP_LOGE(TAG, "%s did not appear within %us; is the card mounted?", root.c_str(),
             (unsigned) (STORAGE_WAIT_MS / 1000));
    return false;
  }
  if (!make_parent_dirs(path))
    return false;

  uint16_t stored_len = 0;
  if (file_exists(path)) {
    stored_len = sound_database_feat_len(path);
    if (stored_len == 0)
      set_database_aside(path);
  }

  const auto feat_model = static_cast<HumanFaceFeat::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_FEAT_MODEL);
  const int64_t started = now_us();
  this->recognizer_ = new HumanFaceRecognizer(path, feat_model, false);
  // A database written by another feature model has the wrong length per face,
  // which esp-dl reads without noticing. Start that one afresh too.
  const int feat_len = this->recognizer_->get_feat_model()->get_feat_len();
  if (stored_len != 0 && stored_len != feat_len) {
    ESP_LOGW(TAG, "The face database holds faces of %u values and this model makes them of %d",
             (unsigned) stored_len, feat_len);
    delete this->recognizer_;
    set_database_aside(path);
    this->recognizer_ = new HumanFaceRecognizer(path, feat_model, false);
  }
  this->enrolled_count_.store(this->recognizer_->get_num_feats());
  ESP_LOGI(TAG, "Face recognition ready in %u ms: %d face(s) enrolled in %s", (unsigned) ((now_us() - started) / 1000),
           this->enrolled_count_.load(), path.c_str());
  return true;
}
#endif  // USE_FACE_DETECTION_RECOGNITION

void FaceDetectionComponent::serve_requests_() {
#ifdef USE_FACE_DETECTION_RECOGNITION
  if (this->recognizer_ == nullptr)
    return;
  bool clear;
  bool reset;
  std::vector<int> deletes;
  {
    Guard guard(this->lock_);
    clear = this->clear_requested_;
    this->clear_requested_ = false;
    reset = this->reset_requested_;
    this->reset_requested_ = false;
    deletes.swap(this->delete_requests_);
  }
  if (reset)
    this->recognized_ = false;
  if (clear) {
    const esp_err_t err = this->recognizer_->clear_all_feats();
    this->recognized_ = false;
    this->enrolled_count_.store(this->recognizer_->get_num_feats());
    if (err != ESP_OK)
      ESP_LOGE(TAG, "Clearing the face database failed (%s)", esp_err_to_name(err));
    Guard guard(this->lock_);
    this->last_recognition_ = {-1, 0.0f, false};
    this->cleared_event_ = err == ESP_OK;
  }
  for (int id : deletes) {
    const esp_err_t err =
        (id > 0 && id <= 0xFFFF) ? this->recognizer_->delete_feat((uint16_t) id) : ESP_ERR_INVALID_ARG;
    this->enrolled_count_.store(this->recognizer_->get_num_feats());
    Guard guard(this->lock_);
    this->deleted_events_.emplace_back(id, err == ESP_OK);
    if (err == ESP_OK && this->last_recognition_.id == id) {
      this->last_recognition_ = {-1, 0.0f, false};
      this->recognized_ = false;
    }
  }
#endif
}

void FaceDetectionComponent::run_job_() {
  dl::image::img_t img{};
  img.data = this->job_pixels_;
  img.width = (uint16_t) this->job_width_;
  img.height = (uint16_t) this->job_height_;
  // The ISP writes RGB565 little-endian, which is what LVGL draws and what the
  // encoder reads. Told big-endian, the detector would see every colour wrong
  // and miss most faces.
  img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;

  const int64_t started = now_us();
  std::list<dl::detect::result_t> &found = this->detector_->run(img);
  const int64_t took = now_us() - started;

  std::vector<FaceBox> boxes;
  boxes.reserve(found.size());
  const int scale = (int) this->job_scale_;
  for (const auto &r : found) {
    if (r.box.size() < 4)
      continue;
    FaceBox box{};
    box.x1 = r.box[0] * scale;
    box.y1 = r.box[1] * scale;
    box.x2 = r.box[2] * scale;
    box.y2 = r.box[3] * scale;
    box.score = r.score;
    box.has_keypoints = r.keypoint.size() >= 10;
    if (box.has_keypoints) {
      for (int i = 0; i < 10; i++)
        box.keypoints[i] = r.keypoint[i] * scale;
    }
    boxes.push_back(box);
  }

  bool recognized_now = false;
  bool gone = false;
  RecognitionResult recognition{-1, 0.0f, false};
  bool enrolled = false;
  int enrolled_id = -1;
  std::string enrolled_name;
#ifdef USE_FACE_DETECTION_RECOGNITION
  if (this->recognizer_ != nullptr) {
    // Enroll and recognize both take the largest face and align it on its
    // landmarks; without them there is nothing to do.
    const bool usable = !found.empty() && found.front().keypoint.size() >= 10;
    if (found.empty()) {
      if (this->recognized_ && ++this->empty_runs_ >= GONE_AFTER_EMPTY_RUNS) {
        this->recognized_ = false;
        gone = true;
      }
    } else if (usable) {
      this->empty_runs_ = 0;
      bool enroll;
      {
        Guard guard(this->lock_);
        enroll = this->enroll_requested_;
        if (enroll) {
          this->enroll_requested_ = false;
          enrolled_name.swap(this->enroll_name_);
        }
      }
      if (enroll) {
        enrolled = true;
        if (this->recognizer_->enroll(img, found) == ESP_OK) {
          // esp-dl numbers a face when it stores it and does not say which
          // number it gave. The face just stored is its own best match, at a
          // similarity of one, so asking straight away names it.
          auto match = this->recognizer_->recognize(img, found);
          if (!match.empty())
            enrolled_id = match.front().id;
        }
        this->enrolled_count_.store(this->recognizer_->get_num_feats());
      } else if (!this->recognized_) {
        // Once a face is recognized it is not asked about again until it has
        // gone: the trigger fires once per visit, not once per detection.
        auto match = this->recognizer_->recognize(img, found);
        if (!match.empty() && match.front().similarity >= this->recognition_threshold_) {
          this->recognized_ = true;
          recognized_now = true;
          recognition = {match.front().id, match.front().similarity, true};
        }
      }
    }
  }
#endif

  Guard guard(this->lock_);
  this->boxes_.swap(boxes);
  this->boxes_frame_width_ = this->job_frame_width_;
  this->boxes_frame_height_ = this->job_frame_height_;
  this->boxes_at_us_ = now_us();
  this->infer_us_ = took;
  this->detections_++;
  if (!this->boxes_.empty())
    this->detected_event_ = (int) this->boxes_.size();
  if (gone)
    this->last_recognition_ = {-1, 0.0f, false};
  if (recognized_now) {
    this->last_recognition_ = recognition;
    this->recognized_event_ = true;
    this->recognized_value_ = recognition;
  }
  if (enrolled) {
    this->enrolled_event_ = true;
    this->enrolled_id_ = enrolled_id;
    this->enrolled_name_ = std::move(enrolled_name);
  }
}

// ===========================================================================
// Main loop side
// ===========================================================================
bool FaceDetectionComponent::job_due_() {
  if (this->frame_counter_ < this->detection_interval_)
    this->frame_counter_++;
  if (this->frame_counter_ < this->detection_interval_ || !this->ready_.load(std::memory_order_acquire) ||
      this->busy_.load(std::memory_order_acquire))
    return false;
  this->frame_counter_ = 0;
  return true;
}

void FaceDetectionComponent::post_job_(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
  const uint32_t long_side = std::max(width, height);
  const uint32_t scale = std::max<uint32_t>(1, (long_side + JOB_LONG_SIDE - 1) / JOB_LONG_SIDE);
  const uint32_t w = width / scale;
  const uint32_t h = height / scale;
  if (w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF)
    return;
  const size_t need = (size_t) w * h;
  if (need > this->job_capacity_) {
    heap_caps_free(this->job_pixels_);
    this->job_pixels_ =
        static_cast<uint16_t *>(heap_caps_aligned_alloc(64, need * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    this->job_capacity_ = this->job_pixels_ != nullptr ? need : 0;
    if (this->job_pixels_ == nullptr) {
      ESP_LOGE(TAG, "No PSRAM for a %ux%u copy of the frame", (unsigned) w, (unsigned) h);
      return;
    }
  }
  // A plain decimation: one pixel in `scale` along each axis. The detector
  // resizes again to its own input with interpolation, so smoothing here would
  // be paid for twice.
  for (uint32_t y = 0; y < h; y++) {
    const auto *src = reinterpret_cast<const uint16_t *>(pixels + (size_t) y * scale * stride);
    uint16_t *dst = this->job_pixels_ + (size_t) y * w;
    if (scale == 1) {
      std::memcpy(dst, src, (size_t) w * 2);
    } else {
      for (uint32_t x = 0; x < w; x++)
        dst[x] = src[x * scale];
    }
  }
  this->job_width_ = w;
  this->job_height_ = h;
  this->job_scale_ = scale;
  this->job_frame_width_ = width;
  this->job_frame_height_ = height;
  this->busy_.store(true, std::memory_order_release);
  xTaskNotifyGive(this->task_);
}

#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
bool FaceDetectionComponent::wants_raw_frames() const { return this->ready_.load(std::memory_order_acquire); }

bool FaceDetectionComponent::on_raw_frame_seen(uint8_t *pixels, const esp_video_camera::RawFrame &frame) {
  if (frame.format != camera::PIXEL_FORMAT_RGB565) {
    if (!this->warned_no_pixels_) {
      this->warned_no_pixels_ = true;
      ESP_LOGW(TAG, "The camera's frames are not RGB565; face detection reads nothing else");
    }
    return false;
  }
  // The copy for the detector is taken before anything is drawn, or it would
  // be looking at its own rectangles.
  if (this->job_due_())
    this->post_job_(pixels, frame.width, frame.height, frame.stride);
  return this->draw_results_(pixels, frame.width, frame.height, frame.stride);
}
#endif

void FaceDetectionComponent::loop() {
  if (this->failed_.load(std::memory_order_acquire)) {
    this->mark_failed();
    return;
  }

#ifdef USE_FACE_DETECTION_ESP_CAM_SENSOR
  // esp_cam_sensor lends its buffers out from loop(), and its display draws the
  // boxes through draw_on_frame(). Here the interval counts loop turns.
  if (this->camera_->is_streaming() && this->job_due_()) {
    esp_cam_sensor::SimpleBufferElement *buffer = this->camera_->acquire_buffer();
    if (buffer != nullptr) {
      uint8_t *data = this->camera_->get_buffer_data(buffer);
      const uint32_t width = this->camera_->get_image_width();
      const uint32_t height = this->camera_->get_image_height();
      if (data != nullptr && width > 0 && height > 0) {
        // The sensor wrote these by DMA; what the cache holds may be older.
        if (esp_ptr_external_ram(data)) {
          esp_cache_msync(data, (size_t) width * height * 2,
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        }
        this->post_job_(data, width, height, width * 2);
      }
      this->camera_->release_buffer(buffer);
    }
  }
#endif

#ifdef USE_FACE_DETECTION_RECOGNITION
  if (!this->names_loaded_ && this->ready_.load(std::memory_order_acquire)) {
    // Read only once the task has seen the storage mounted.
    this->names_loaded_ = true;
    this->load_names_();
  }
#endif

  this->deliver_events_();

  const uint32_t now = millis();
  if ((int32_t) (now - this->report_at_ms_) >= 0) {
    this->report_at_ms_ = now + REPORT_INTERVAL_MS;
    uint32_t detections = 0;
    int64_t infer_us = 0;
    {
      Guard guard(this->lock_);
      detections = this->detections_;
      this->detections_ = 0;
      infer_us = this->infer_us_;
    }
    if (detections > 0) {
      ESP_LOGD(TAG, "%u detections in %us, the last in %u ms", (unsigned) detections,
               (unsigned) (REPORT_INTERVAL_MS / 1000), (unsigned) (infer_us / 1000));
    }
  }
}

void FaceDetectionComponent::deliver_events_() {
  int detected;
  bool recognized;
  RecognitionResult recognition;
  bool enrolled;
  int enrolled_id;
  std::string enrolled_name;
  bool cleared;
  std::vector<std::pair<int, bool>> deleted;
  {
    Guard guard(this->lock_, 0);
    if (!guard.held())
      return;  // the task has it for a moment; the events keep until next turn
    detected = this->detected_event_;
    this->detected_event_ = 0;
    recognized = this->recognized_event_;
    this->recognized_event_ = false;
    recognition = this->recognized_value_;
    enrolled = this->enrolled_event_;
    this->enrolled_event_ = false;
    enrolled_id = this->enrolled_id_;
    enrolled_name.swap(this->enrolled_name_);
    cleared = this->cleared_event_;
    this->cleared_event_ = false;
    deleted.swap(this->deleted_events_);
  }

  if (cleared) {
    this->face_names_.clear();
    this->drawn_name_id_ = -1;
    this->save_names_();
    ESP_LOGI(TAG, "All faces and their names cleared");
  }
  for (const auto &d : deleted) {
    if (!d.second) {
      ESP_LOGW(TAG, "Face ID %d is not in the database", d.first);
      continue;
    }
    ESP_LOGI(TAG, "Face ID %d deleted", d.first);
    if (this->face_names_.erase(d.first) > 0) {
      this->drawn_name_id_ = -1;
      this->save_names_();
    }
  }
  if (enrolled) {
    if (enrolled_id > 0) {
      ESP_LOGI(TAG, "Face enrolled as ID %d (%d enrolled)", enrolled_id, this->enrolled_count_.load());
      if (!enrolled_name.empty()) {
        this->face_names_[enrolled_id] = enrolled_name;
        this->drawn_name_id_ = -1;
        this->save_names_();
        ESP_LOGI(TAG, "  named '%s'", enrolled_name.c_str());
      }
    } else {
      ESP_LOGE(TAG, "Enrolling the face failed; the log above says why");
    }
  }
  if (detected > 0) {
    for (auto &callback : this->on_face_detected_callbacks_)
      callback(detected);
  }
  if (recognized) {
    ESP_LOGI(TAG, "Face recognized: ID %d, similarity %.2f", recognition.id, recognition.similarity);
    for (auto &callback : this->on_face_recognized_callbacks_)
      callback(recognition.id, recognition.similarity);
  }
}

void FaceDetectionComponent::draw_on_frame(uint8_t *img_data, uint16_t width, uint16_t height) {
  if (img_data != nullptr)
    this->draw_results_(img_data, width, height, (uint32_t) width * 2);
}

bool FaceDetectionComponent::draw_results_(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
  if (!this->draw_enabled_ || this->lock_ == nullptr)
    return false;
  RecognitionResult recognition;
  {
    // A short wait at most: this is the frame path, and the task only ever
    // holds the lock for a copy.
    Guard guard(this->lock_, pdMS_TO_TICKS(2));
    if (!guard.held() || this->boxes_.empty())
      return false;
    // Boxes found in a frame of another size, or too long ago, would be drawn
    // in the wrong place.
    if (this->boxes_frame_width_ != width || this->boxes_frame_height_ != height ||
        now_us() - this->boxes_at_us_ > BOXES_VALID_US)
      return false;
    this->drawn_boxes_.assign(this->boxes_.begin(), this->boxes_.end());
    recognition = this->last_recognition_;
  }

  // Lines that stay visible at any resolution: 2 px at 640, 4 at 1280.
  const int thickness = std::max<int>(2, (int) width / 320);
  const int text_scale = std::max<int>(2, (int) width / 480);
  // The recognizer looks at the largest face only, so that is the one named.
  size_t largest = 0;
  for (size_t i = 1; i < this->drawn_boxes_.size(); i++) {
    const auto &a = this->drawn_boxes_[i];
    const auto &b = this->drawn_boxes_[largest];
    if ((a.x2 - a.x1) * (a.y2 - a.y1) > (b.x2 - b.x1) * (b.y2 - b.y1))
      largest = i;
  }

  for (size_t i = 0; i < this->drawn_boxes_.size(); i++) {
    const FaceBox &box = this->drawn_boxes_[i];
    const bool named = recognition.recognized && i == largest;
    const uint16_t color = named ? COLOR_BLUE : COLOR_GREEN;
    frame_rect(pixels, width, height, stride, box.x1, box.y1, box.x2, box.y2, named ? thickness + 1 : thickness, color);
    if (box.has_keypoints) {
      for (int k = 0; k < 5; k++) {
        const int kx = box.keypoints[k * 2];
        const int ky = box.keypoints[k * 2 + 1];
        fill_rect(pixels, width, height, stride, kx - thickness, ky - thickness, kx + thickness, ky + thickness,
                  COLOR_RED);
      }
    }
    if (!this->draw_labels_)
      continue;
    const std::string *label = &this->unknown_label_;
    if (named) {
      if (this->drawn_name_id_ != recognition.id) {
        std::string name = this->get_face_name(recognition.id);
        if (name.empty())
          name = "ID " + std::to_string(recognition.id);
        this->drawn_name_ = fold_for_font(name);
        this->drawn_name_id_ = recognition.id;
      }
      label = &this->drawn_name_;
    }
    if (label->empty())
      continue;
    // The label sits in a band of the box's own colour across the top of the
    // box, inside it, so it reads against any background: white on the
    // recognized blue, black on the detected green. A face at the top of the
    // picture keeps its label, which above the box would be off the frame.
    const int pad = text_scale;
    const int band_w = text_width(*label, text_scale) + 2 * pad;
    const int band_h = 7 * text_scale + 2 * pad;
    fill_rect(pixels, width, height, stride, box.x1, box.y1, box.x1 + band_w, box.y1 + band_h, color);
    this->draw_text_(pixels, width, height, stride, box.x1 + pad, box.y1 + pad, *label,
                     named ? COLOR_WHITE : COLOR_BLACK, text_scale);
  }
  return true;
}

void FaceDetectionComponent::draw_text_(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, int x,
                                        int y, const std::string &text, uint16_t color, int scale) {
  for (char c : text) {
    if (x >= (int) width)
      break;
    const int glyph = glyph_index(c);
    if (glyph >= 0) {
      for (int row = 0; row < 7; row++) {
        const uint8_t bits = FONT_5X7[glyph][row];
        for (int col = 0; col < 5; col++) {
          if (bits & (0x10 >> col)) {
            fill_rect(pixels, width, height, stride, x + col * scale, y + row * scale, x + (col + 1) * scale,
                      y + (row + 1) * scale, color);
          }
        }
      }
    }
    x += 6 * scale;
  }
}

// ===========================================================================
// Detection and recognition API
// ===========================================================================
int FaceDetectionComponent::get_detected_face_count() {
  Guard guard(this->lock_, pdMS_TO_TICKS(5));
  if (!guard.held() || now_us() - this->boxes_at_us_ > BOXES_VALID_US)
    return 0;
  return (int) this->boxes_.size();
}

std::vector<FaceBox> FaceDetectionComponent::get_detected_faces() {
  Guard guard(this->lock_, pdMS_TO_TICKS(5));
  if (!guard.held() || now_us() - this->boxes_at_us_ > BOXES_VALID_US)
    return {};
  return this->boxes_;
}

int FaceDetectionComponent::enroll_face() { return this->enroll_face_with_name(""); }

int FaceDetectionComponent::enroll_face_with_name(const std::string &name) {
#ifdef USE_FACE_DETECTION_RECOGNITION
  Guard guard(this->lock_);
  this->enroll_requested_ = true;
  this->enroll_name_ = name;
  if (name.empty()) {
    ESP_LOGI(TAG, "The next face seen will be enrolled");
  } else {
    ESP_LOGI(TAG, "The next face seen will be enrolled as '%s'", name.c_str());
  }
  return 0;
#else
  ESP_LOGE(TAG, "Enrolling needs recognition_enabled: true");
  return -1;
#endif
}

bool FaceDetectionComponent::delete_face(int id) {
#ifdef USE_FACE_DETECTION_RECOGNITION
  {
    Guard guard(this->lock_);
    this->delete_requests_.push_back(id);
  }
  if (this->task_ != nullptr)
    xTaskNotifyGive(this->task_);
  return true;
#else
  ESP_LOGE(TAG, "Deleting a face needs recognition_enabled: true");
  return false;
#endif
}

void FaceDetectionComponent::clear_all_faces() {
#ifdef USE_FACE_DETECTION_RECOGNITION
  {
    Guard guard(this->lock_);
    this->clear_requested_ = true;
  }
  if (this->task_ != nullptr)
    xTaskNotifyGive(this->task_);
#else
  ESP_LOGE(TAG, "Clearing faces needs recognition_enabled: true");
#endif
}

RecognitionResult FaceDetectionComponent::get_last_recognition() {
  Guard guard(this->lock_);
  return this->last_recognition_;
}

void FaceDetectionComponent::reset_last_recognition() {
  {
    Guard guard(this->lock_);
    this->last_recognition_ = {-1, 0.0f, false};
    this->reset_requested_ = true;
  }
  if (this->task_ != nullptr)
    xTaskNotifyGive(this->task_);
  ESP_LOGI(TAG, "Recognition reset: the next face seen will be recognized again");
}

void FaceDetectionComponent::set_face_name(int id, const std::string &name) {
  this->face_names_[id] = name;
  this->drawn_name_id_ = -1;
  ESP_LOGI(TAG, "Face ID %d is named '%s'", id, name.c_str());
  this->save_names_();
}

std::string FaceDetectionComponent::get_face_name(int id) {
  auto it = this->face_names_.find(id);
  return it != this->face_names_.end() ? it->second : "";
}

std::string FaceDetectionComponent::get_last_recognized_name() {
  const RecognitionResult recognition = this->get_last_recognition();
  return recognition.recognized ? this->get_face_name(recognition.id) : "";
}

// ===========================================================================
// Names, next to the face database: one "ID:NAME" per line
// ===========================================================================
std::string FaceDetectionComponent::get_names_file_path_() {
  std::string path = this->face_db_path_;
  const size_t dot = path.rfind('.');
  const size_t slash = path.rfind('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    path.resize(dot);
  return path + "_names.txt";
}

void FaceDetectionComponent::load_names_() {
  const std::string path = this->get_names_file_path_();
  std::ifstream file(path);
  if (!file.is_open()) {
    ESP_LOGI(TAG, "No names yet (%s is written with the first one)", path.c_str());
    return;
  }
  this->face_names_.clear();
  std::string line;
  while (std::getline(file, line)) {
    const size_t colon = line.find(':');
    if (colon == 0 || colon == std::string::npos || colon > 5)
      continue;
    const std::string id = line.substr(0, colon);
    if (!std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; }))
      continue;
    this->face_names_[std::stoi(id)] = line.substr(colon + 1);
  }
  this->drawn_name_id_ = -1;
  ESP_LOGI(TAG, "%u face name(s) read from %s", (unsigned) this->face_names_.size(), path.c_str());
}

void FaceDetectionComponent::save_names_() {
  const std::string path = this->get_names_file_path_();
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    ESP_LOGE(TAG, "Cannot write %s", path.c_str());
    return;
  }
  for (const auto &entry : this->face_names_)
    file << entry.first << ":" << entry.second << "\n";
}

}  // namespace esphome::face_detection

#endif  // USE_ESP_IDF && USE_ESP32_VARIANT_ESP32P4
