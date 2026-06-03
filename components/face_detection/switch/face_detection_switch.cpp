#include "face_detection_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace face_detection {

static const char *const TAG = "face_detection.switch";

void FaceDetectionSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) {
    this->parent_->set_enabled(state);
  }
  this->publish_state(state);
}

void FaceDetectionSwitch::dump_config() { LOG_SWITCH("", "Face Detection Switch", this); }

}  // namespace face_detection
}  // namespace esphome
