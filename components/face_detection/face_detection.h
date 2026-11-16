#pragma once

#include "esphome/core/defines.h"

#if defined(USE_ESP_IDF) && defined(USE_ESP32_VARIANT_ESP32P4)

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
#include "esphome/components/esp_video_camera/esp_video_camera.h"
#endif
#ifdef USE_FACE_DETECTION_ESP_CAM_SENSOR
#include "esphome/components/esp_cam_sensor/esp_cam_sensor_camera.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Espressif's classes, kept out of this header: esp-dl's headers are large and
// only the .cpp needs them.
class HumanFaceDetect;
class HumanFaceRecognizer;

namespace esphome::face_detection {

/// One face, in the coordinates of the camera frame it was found in.
struct FaceBox {
  int x1, y1, x2, y2;
  float score;
  /// Five landmarks, x then y: the eyes, the nose and the corners of the
  /// mouth. Only MSRMNP finds them; with ESPDet `has_keypoints` is false.
  int keypoints[10];
  bool has_keypoints;
};

struct RecognitionResult {
  int id;
  float similarity;
  bool recognized;
};

/// Runs Espressif's face detector (and optionally its recognizer) on the
/// camera's frames, on a task of its own so the main loop never waits for an
/// inference.
///
/// The frames come either from esp_video_camera, which shows each one to this
/// component before the display and the JPEG encoder see it -- every MIPI-CSI
/// sensor it supports, so every sensor esp_video drives -- or from
/// esp_cam_sensor, whose buffers are borrowed from loop(). One frame in
/// `detection_interval` is copied, shrunk to at most 800 pixels on its long
/// side, and handed to the task; the boxes found are drawn on every frame
/// after it, so they follow the picture at the camera's rate.
class FaceDetectionComponent : public Component
#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
    ,
                               public esp_video_camera::RawFrameListener
#endif
{
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the SD card is mounted, when the models or the face database live on
  // it. The task waits for the card on top of that, since mounting can lag.
  float get_setup_priority() const override { return -200.0f; }

  // Configuration ---------------------------------------------------------------
#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
  void set_camera(esp_video_camera::ESPVideoCamera *camera);
#endif
#ifdef USE_FACE_DETECTION_ESP_CAM_SENSOR
  void set_camera(esp_cam_sensor::MipiDSICamComponent *camera) { this->camera_ = camera; }
#endif
  /// HumanFaceDetect::model_type_t.
  void set_model(int model) { this->model_ = model; }
  /// Where the models are read from when they live on the SD card; only used to
  /// wait for that folder to appear.
  void set_model_dir(const std::string &dir) { this->model_dir_ = dir; }
  void set_canvas_id(const std::string &canvas_id) { this->canvas_id_ = canvas_id; }
  void set_score_threshold(float threshold) { this->score_threshold_ = threshold; }
  void set_nms_threshold(float threshold) { this->nms_threshold_ = threshold; }
  void set_detection_interval(int interval) { this->detection_interval_ = interval < 1 ? 1 : interval; }
  void set_draw_enabled(bool enabled) { this->draw_enabled_ = enabled; }
  void set_face_db_path(const std::string &path) { this->face_db_path_ = path; }
  void set_recognition_threshold(float threshold) { this->recognition_threshold_ = threshold; }

  const std::string &get_canvas_id() { return this->canvas_id_; }

  // Detection -------------------------------------------------------------------
  int get_detected_face_count();
  std::vector<FaceBox> get_detected_faces();

  // Recognition -----------------------------------------------------------------
  /// Asks for the next face seen to be enrolled. Returns 0 when the request is
  /// taken, -1 when recognition is not available.
  int enroll_face();
  int enroll_face_with_name(const std::string &name);
  /// Asks for one face to be forgotten. True when the request is taken; the log
  /// says whether the database had it.
  bool delete_face(int id);
  void clear_all_faces();
  int get_enrolled_count() { return this->enrolled_count_.load(); }
  RecognitionResult get_last_recognition();
  void reset_last_recognition();

  void set_face_name(int id, const std::string &name);
  std::string get_face_name(int id);
  std::string get_last_recognized_name();

  /// Draws the boxes into a frame a display is about to show. For esp_cam_sensor,
  /// whose display calls this; esp_video_camera's frames are drawn on before
  /// anyone sees them. Rows are `width` pixels of RGB565, back to back.
  void draw_on_frame(uint8_t *img_data, uint16_t width, uint16_t height);

  void add_on_face_detected_callback(std::function<void(int)> callback) {
    this->on_face_detected_callbacks_.push_back(std::move(callback));
  }
  void add_on_face_recognized_callback(std::function<void(int, float)> callback) {
    this->on_face_recognized_callbacks_.push_back(std::move(callback));
  }

#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
  // esp_video_camera::RawFrameListener ------------------------------------------
  bool wants_raw_frames() const override;
  bool on_raw_frame_seen(uint8_t *pixels, const esp_video_camera::RawFrame &frame) override;
#endif

 protected:
  static void task_entry_(void *arg);
  void task_main_();
  bool load_models_();
  void run_job_();
  void serve_requests_();
#ifdef USE_FACE_DETECTION_RECOGNITION
  bool open_recognizer_();
#endif
  /// Copies one frame into the task's buffer and wakes the task. Only called
  /// while the task is idle, which is what makes the buffer safe to write.
  void post_job_(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
  /// Whether a frame is due for the detector right now.
  bool job_due_();
  bool draw_results_(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride);
  void draw_text_(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride, int x, int y,
                  const std::string &text, uint16_t color, int scale);
  void deliver_events_();

  std::string get_names_file_path_();
  void load_names_();
  void save_names_();

#ifdef USE_FACE_DETECTION_ESP_VIDEO_CAMERA
  esp_video_camera::ESPVideoCamera *camera_{nullptr};
#endif
#ifdef USE_FACE_DETECTION_ESP_CAM_SENSOR
  esp_cam_sensor::MipiDSICamComponent *camera_{nullptr};
#endif

  // Configuration.
  int model_{0};
  std::string model_dir_{};
  std::string canvas_id_{};
  bool draw_enabled_{true};
  float score_threshold_{0.3f};
  float nms_threshold_{0.5f};
  uint32_t detection_interval_{8};
  std::string face_db_path_{"/sdcard/faces.db"};
  float recognition_threshold_{0.7f};

  // The task and what only it touches.
  TaskHandle_t task_{nullptr};
  HumanFaceDetect *detector_{nullptr};
  HumanFaceRecognizer *recognizer_{nullptr};
  bool recognized_{false};
  int empty_runs_{0};

  /// The frame handed to the task. Written by the main loop while `busy_` is
  /// false, read by the task while it is true, so neither ever sees the other
  /// half-way.
  uint16_t *job_pixels_{nullptr};
  size_t job_capacity_{0};  // in pixels
  uint32_t job_width_{0};
  uint32_t job_height_{0};
  uint32_t job_scale_{1};
  uint32_t job_frame_width_{0};
  uint32_t job_frame_height_{0};
  std::atomic<bool> busy_{false};
  /// Set once the models are loaded; nothing is copied for the task before.
  std::atomic<bool> ready_{false};
  std::atomic<bool> failed_{false};
  uint32_t frame_counter_{0};

  /// Everything below is shared between the task and the main loop, under
  /// this lock.
  SemaphoreHandle_t lock_{nullptr};
  std::vector<FaceBox> boxes_;
  uint32_t boxes_frame_width_{0};
  uint32_t boxes_frame_height_{0};
  int64_t boxes_at_us_{0};
  RecognitionResult last_recognition_{-1, 0.0f, false};
  // Requests from the main loop, served by the task.
  bool enroll_requested_{false};
  std::string enroll_name_{};
  bool clear_requested_{false};
  bool reset_requested_{false};
  std::vector<int> delete_requests_;
  // Events from the task, delivered by the main loop.
  int detected_event_{0};
  bool recognized_event_{false};
  RecognitionResult recognized_value_{-1, 0.0f, false};
  bool enrolled_event_{false};
  int enrolled_id_{-1};
  std::string enrolled_name_{};
  bool cleared_event_{false};
  std::vector<std::pair<int, bool>> deleted_events_;
  std::atomic<int> enrolled_count_{0};
  uint32_t detections_{0};
  int64_t infer_us_{0};

  // Main loop only.
  std::map<int, std::string> face_names_;
  bool names_loaded_{false};
  /// The boxes being drawn on this frame, copied out of the shared ones so the
  /// lock is not held while drawing. Kept to reuse its storage.
  std::vector<FaceBox> drawn_boxes_;
  std::string drawn_name_{};
  int drawn_name_id_{-1};
  bool warned_no_pixels_{false};
  uint32_t report_at_ms_{0};

  std::vector<std::function<void(int)>> on_face_detected_callbacks_;
  std::vector<std::function<void(int, float)>> on_face_recognized_callbacks_;
};

class FaceDetectedTrigger : public Trigger<int> {
 public:
  explicit FaceDetectedTrigger(FaceDetectionComponent *parent) {
    parent->add_on_face_detected_callback([this](int face_count) { this->trigger(face_count); });
  }
};

class FaceRecognizedTrigger : public Trigger<int, float> {
 public:
  explicit FaceRecognizedTrigger(FaceDetectionComponent *parent) {
    parent->add_on_face_recognized_callback(
        [this](int face_id, float similarity) { this->trigger(face_id, similarity); });
  }
};

// Action::play() takes its arguments as `const Ts &...`; a signature by value
// does not override it, and the action then silently does nothing.
template<typename... Ts> class EnrollFaceAction : public Action<Ts...>, public Parented<FaceDetectionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->enroll_face(); }
};

template<typename... Ts>
class EnrollFaceWithNameAction : public Action<Ts...>, public Parented<FaceDetectionComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, name)

  void play(const Ts &...x) override { this->parent_->enroll_face_with_name(this->name_.value(x...)); }
};

template<typename... Ts> class SetFaceNameAction : public Action<Ts...>, public Parented<FaceDetectionComponent> {
 public:
  TEMPLATABLE_VALUE(int, face_id)
  TEMPLATABLE_VALUE(std::string, name)

  void play(const Ts &...x) override {
    this->parent_->set_face_name(this->face_id_.value(x...), this->name_.value(x...));
  }
};

template<typename... Ts> class DeleteFaceAction : public Action<Ts...>, public Parented<FaceDetectionComponent> {
 public:
  TEMPLATABLE_VALUE(int, face_id)

  void play(const Ts &...x) override { this->parent_->delete_face(this->face_id_.value(x...)); }
};

template<typename... Ts> class ClearAllFacesAction : public Action<Ts...>, public Parented<FaceDetectionComponent> {
 public:
  void play(const Ts &...x) override { this->parent_->clear_all_faces(); }
};

}  // namespace esphome::face_detection

#endif  // USE_ESP_IDF && USE_ESP32_VARIANT_ESP32P4
