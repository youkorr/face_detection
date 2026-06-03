#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "../face_detection.h"

namespace esphome {
namespace face_detection {

// Switch that enables/disables the face detection pipeline at runtime.
class FaceDetectionSwitch : public switch_::Switch,
                            public Component,
                            public Parented<FaceDetectionComponent> {
 public:
  void dump_config() override;

 protected:
  void write_state(bool state) override;
};

}  // namespace face_detection
}  // namespace esphome
