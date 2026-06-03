#include "face_detection_switch.h"
#include "esphome/core/log.h"

namespace esphome {
namespace face_detection {

static const char *const TAG = "face_detection.switch";

void FaceDetectionSwitch::setup() {
  // The base switch_::Switch class does NOT apply the restore_mode automatically;
  // each switch must resolve and apply its initial state in setup(). Without this,
  // the component's enabled_ flag stays at its default and never matches the
  // switch's restored/default state until the user toggles it manually.
  auto initial_state = this->get_initial_state_with_restore_mode();
  if (initial_state.has_value()) {
    if (initial_state.value()) {
      this->turn_on();
    } else {
      this->turn_off();
    }
  }
}

void FaceDetectionSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) {
    this->parent_->set_enabled(state);
  }
  this->publish_state(state);
}

void FaceDetectionSwitch::dump_config() { LOG_SWITCH("", "Face Detection Switch", this); }

}  // namespace face_detection
}  // namespace esphome
