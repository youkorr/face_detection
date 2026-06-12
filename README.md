# ESPHome Face Detection & Recognition (ESP32-P4)

On-device **face detection** and **face recognition** for ESPHome, running
entirely on the **ESP32-P4** with no cloud dependency.

This is an ESPHome port of Espressif's
[esp-dl](https://github.com/espressif/esp-dl) deep-learning stack and its
`human_face_detect` / `human_face_recognition` models. Detection and feature
extraction run on the P4's vector instructions; recognized faces are stored
locally on the SD card.

---

## Features

- **Face detection** — locates faces in the live camera stream and returns
  bounding boxes + 5 facial landmarks.
- **Face recognition** — extracts a feature embedding per face and matches it
  against an enrolled database (cosine similarity).
- **Enrollment at runtime** — enroll the currently visible face, with or
  without a name, from any ESPHome automation.
- **Local & private** — models and the face database live on flash / SD card.
  Nothing leaves the device.
- **LVGL overlay** — optionally draws bounding boxes and names directly onto an
  LVGL canvas.
- **Event triggers** — `on_face_detected` and `on_face_recognized` automations.

> ⚠️ **Hardware:** ESP32-P4 only. The models and the optimized kernels are built
> for the P4 target (`esp32p4`). A camera wired through
> [`esp_cam_sensor`](https://github.com/youkorr/test2_esp_video_esphome)
> is required; an SD card is required for recognition (face database).

---

## Installation

Add the component through `external_components`:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/face_detection
      ref: main
    components: [face_detection]
    refresh: always
```

It depends on a camera component (`esp_cam_sensor` / `esp_video`). See the
[full example](#full-example) below for a working ESP32-P4 setup.

---

## Configuration

```yaml
face_detection:
  id: face_detect
  camera_id: tab5_cam          # required: an esp_cam_sensor camera
  canvas_id: camera_canvas     # optional: LVGL canvas to draw overlays on
  model_type: face_recognition
  score_threshold: 0.3
  nms_threshold: 0.5
  detection_interval: 5
  draw_enabled: true
  recognition_enabled: true
  face_db_path: "/sdcard/reconnaisance_faciale/faces.db"
  recognition_threshold: 0.85
```

### Options

| Option | Type | Default | Description |
|---|---|---|---|
| `camera_id` | ID | **required** | The `esp_cam_sensor` camera to read frames from. |
| `canvas_id` | string | — | ID of an LVGL canvas. When set (and `draw_enabled`), boxes/names are drawn onto it. |
| `model_type` | enum | `face_recognition` | One of `face_recognition`, `yolo11`, `pose_detection`. |
| `score_threshold` | float (0–1) | `0.3` | Minimum detection confidence to keep a face. |
| `nms_threshold` | float (0–1) | `0.5` | Non-max-suppression IoU threshold (overlapping-box merge). |
| `detection_interval` | int (1–600) | `8` | Run inference every N camera frames (higher = lighter CPU). |
| `draw_enabled` | bool | `true` | Draw bounding boxes / names on the image buffer. |
| `recognition_enabled` | bool | `false` | Enable the recognition stage (embeddings + database). |
| `face_db_path` | string | `/sdcard/faces.db` | Path to the persisted face database (SD card). |
| `recognition_threshold` | float (0–1) | `0.7` | Minimum similarity to consider a face a match. |
| `model_location` | enum | `flash_rodata` | Where the model weights live: `flash_rodata` (embedded) or `sdcard`. |
| `model_path` | string | `/sdcard` | Path to the model when `model_location: sdcard`. |

---

## Automations (triggers)

### `on_face_detected`
Fired each detection cycle when at least one face is present.
Variable: `face_count` (`int`).

```yaml
face_detection:
  id: face_detect
  camera_id: tab5_cam
  on_face_detected:
    - logger.log:
        format: "Faces in frame: %d"
        args: [face_count]
```

### `on_face_recognized`
Fired when an enrolled face is matched.
Variables: `face_id` (`int`), `similarity` (`float`).

```yaml
  on_face_recognized:
    - lambda: |-
        ESP_LOGI("face", "Recognized id=%d (%.2f)", face_id, similarity);
    - lvgl.page.show: home_page    # e.g. unlock the screen
```

---

## Actions

| Action | Parameters | Description |
|---|---|---|
| `face_detection.enroll` | — | Enroll the currently visible face (auto-assigned ID). |
| `face_detection.enroll_with_name` | `name` (templatable string) | Enroll the visible face under a name. |
| `face_detection.set_name` | `face_id`, `name` | Rename an existing enrolled face. |
| `face_detection.delete` | `face_id` | Remove one enrolled face. |
| `face_detection.clear_all` | — | Wipe the whole face database. |

```yaml
button:
  - platform: template
    name: "Enroll my face"
    on_press:
      - face_detection.enroll_with_name:
          id: face_detect
          name: "Sapphire"

  - platform: template
    name: "Clear face database"
    on_press:
      - face_detection.clear_all: face_detect
```

---

## Lambda API

The component exposes helpers you can call from `lambda:`:

```cpp
id(face_detect).get_detected_face_count();   // int  — faces in last frame
id(face_detect).get_enrolled_count();        // int  — faces in the database
id(face_detect).get_last_recognized_name();  // std::string
id(face_detect).get_face_name(id);           // std::string
id(face_detect).enroll_face();               // int  — enroll, returns new id
id(face_detect).delete_face(id);             // bool
id(face_detect).clear_all_faces();           // void
```

Example sensor exposing the number of enrolled faces to Home Assistant:

```yaml
sensor:
  - platform: template
    name: "Enrolled Faces"
    icon: "mdi:face-man"
    lambda: return id(face_detect).get_enrolled_count();
    update_interval: 60s
```

---

## How it works

1. **Capture** — frames are pulled from the `esp_cam_sensor` camera at
   `detection_interval` spacing.
2. **Detect** — Espressif's two-stage detector (`MSR_S8_V1` → `MNP_S8_V1`)
   proposes candidates then refines them, yielding boxes + 5 landmarks,
   filtered by `score_threshold` / `nms_threshold`.
3. **Recognize** *(optional)* — each detected face is aligned and passed through
   the feature model (`MFN_S8_V1`) to produce an embedding, compared against the
   database with cosine similarity against `recognition_threshold`.
4. **Persist** — enrolled embeddings and names are saved to `face_db_path` on
   the SD card and reloaded on boot.
5. **Draw** *(optional)* — boxes and recognized names are rendered onto the
   LVGL `canvas_id`.

Model weights are embedded in flash by default (`model_location: flash_rodata`)
so the device works without reading models from the SD card at runtime.

---

## Full example

A trimmed, working ESP32-P4 configuration (camera + display + face detection):

```yaml
esp32:
  variant: esp32p4
  board: esp32-p4-evboard
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/youkorr/face_detection
      ref: main
    components: [face_detection]
    refresh: always

# Camera (provides esp_cam_sensor)
esp_video:
  i2c_id: bsp_bus
  xclk_pin: GPIO36
  xclk_freq: 24000000
  enable_jpeg: true
  enable_isp: true

esp_cam_sensor:
  id: tab5_cam
  i2c_id: bsp_bus
  sensor_type: ov5647
  resolution: "800x640"
  framerate: 30

# Optional: show the stream on an LVGL canvas
lvgl_camera_display:
  id: camera_display
  camera_id: tab5_cam
  canvas_id: camera_canvas
  update_interval: 16ms

# SD card for the face database
sd_mmc_card:
  id: sd_card
  clk_pin: GPIO43
  cmd_pin: GPIO44
  data0_pin: GPIO39
  data1_pin: GPIO40
  data2_pin: GPIO41
  data3_pin: GPIO42
  mode_1bit: false

# Face detection + recognition
face_detection:
  id: face_detect
  camera_id: tab5_cam
  canvas_id: camera_canvas
  model_type: face_recognition
  score_threshold: 0.3
  nms_threshold: 0.5
  detection_interval: 5
  draw_enabled: true
  recognition_enabled: true
  face_db_path: "/sdcard/reconnaisance_faciale/faces.db"
  recognition_threshold: 0.85
  on_face_recognized:
    - lambda: |-
        ESP_LOGI("face", "Recognized id=%d (%.2f)", face_id, similarity);

sensor:
  - platform: template
    name: "Enrolled Faces"
    icon: "mdi:face-man"
    lambda: return id(face_detect).get_enrolled_count();
    update_interval: 60s
```

> The complete real-world configuration this component was developed against
> (Waveshare ESP32-P4 7" with audio, LVGL UI, video calling, etc.) is available
> on request — the snippet above isolates the face-detection parts.

---

## Tips & troubleshooting

- **No detections:** lower `score_threshold` (e.g. `0.25`), make sure the face
  is well lit and large enough in frame.
- **CPU too busy / dropped frames:** raise `detection_interval`, or disable
  recognition (`recognition_enabled: false`) when you only need detection.
- **False matches:** raise `recognition_threshold` (e.g. `0.9`).
- **Recognition not persisting:** confirm the SD card is mounted before the
  component sets up and that `face_db_path` is writable.

---

## License

This project is licensed under the **MIT License** — see [`LICENSE`](LICENSE).

It is an **external component for ESPHome**. ESPHome is dual-licensed: its
Python codebase is MIT, while its C++/runtime code (`.c`, `.cpp`, `.h`, `.hpp`,
`.tcc`, `.ino`) is licensed under the **GPLv3**. The MIT terms of this component
are GPLv3-compatible, but firmware compiled together with the ESPHome runtime
is, as a combined work, subject to the GPLv3.

### Third-party components

This project bundles and ports source from **Espressif Systems (Shanghai)
Co., Ltd.**, distributed under the MIT License, Copyright © 2021 Espressif:

| Component | Upstream | License |
|---|---|---|
| `esp-dl` | [espressif/esp-dl](https://github.com/espressif/esp-dl) | MIT |
| `human_face_detect` (+ models) | [esp-dl/models/human_face_detect](https://github.com/espressif/esp-dl/tree/master/models/human_face_detect) | MIT |
| `human_face_recognition` (+ models) | [esp-dl/models/human_face_recognition](https://github.com/espressif/esp-dl/tree/master/models/human_face_recognition) | MIT |

Their original license texts are retained in the respective directories under
[`components/`](components/).

## Credits

- [Espressif esp-dl](https://github.com/espressif/esp-dl) — detection /
  recognition models and inference kernels.
- [ESPHome](https://esphome.io) — the firmware framework this component plugs
  into.
