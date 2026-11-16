# ESPHome Face Detection & Recognition (ESP32-P4)

On-device **face detection** and **face recognition** for ESPHome, running
entirely on the **ESP32-P4** with no cloud dependency.

It runs Espressif's [esp-dl](https://github.com/espressif/esp-dl) and its
`human_face_detect` / `human_face_recognition` models, taken as published from
the ESP Component Registry (human_face_detect 0.5.2, human_face_recognition
0.3.3, esp-dl 3.3). Detection and feature extraction run on the P4's vector
instructions, on a task of their own so the main loop and the display never
wait for an inference; recognized faces are stored locally on the SD card.

---

## Features

- **Face detection** — locates faces in the live camera stream and returns
  bounding boxes, and with the default detector 5 facial landmarks. Three of
  Espressif's detectors to choose from (`model:`).
- **Every camera sensor** — reads `esp_video_camera` (every MIPI-CSI sensor
  esp_video drives) or `esp_cam_sensor`.
- **Face recognition** — extracts a feature embedding per face and matches it
  against an enrolled database (cosine similarity).
- **Enrollment at runtime** — enroll the currently visible face, with or
  without a name, from any ESPHome automation.
- **Local & private** — models and the face database live on flash / SD card.
  Nothing leaves the device.
- **Overlay** — draws the boxes and names into the camera frames themselves,
  so they show on the display and in the picture Home Assistant receives.
- **Event triggers** — `on_face_detected` and `on_face_recognized` automations.

> ⚠️ **Hardware:** ESP32-P4 only. The models and the optimized kernels are built
> for the P4 target (`esp32p4`). A MIPI-CSI camera is required, through
> [`esp_video_camera`](https://github.com/youkorr/esphome/tree/esp_video_camera-lvgl/esphome/components/esp_video_camera)
> or `esp_cam_sensor`. A USB (UVC) camera cannot be used: it delivers JPEG,
> with no pixels to look at. An SD card is required for recognition (face
> database).

---

## Screenshots

> Drop your images in a `docs/` folder at the repo root (e.g.
> `docs/detection.jpg`) and they will render here.

| Live detection | Recognition overlay |
|---|---|
| ![Face detection — bounding boxes and landmarks on the LVGL canvas](docs/detection.jpg) | ![Face recognition — matched name drawn over the detected face](docs/recognition.jpg) |

| Enrollment | Home Assistant sensor |
|---|---|
| ![Enrolling a new face from the UI](docs/enrollment.jpg) | ![Enrolled-faces count exposed to Home Assistant](docs/ha_sensor.jpg) |

<!--
To add a screenshot:
  1. mkdir docs
  2. copy your image there, e.g. docs/detection.jpg
  3. commit it — the links above will resolve automatically.
-->

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

It needs a camera component: `esp_video_camera` (recommended) or
`esp_cam_sensor`. See the [full example](#full-example) below for a working
ESP32-P4 setup.

---

## Configuration

```yaml
face_detection:
  id: face_detect
  camera_id: tab5_cam          # required: esp_video_camera or esp_cam_sensor
  model: msrmnp_s8_v1          # the detector (see below)
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
| `camera_id` | ID | **required** | The camera: an `esp_video_camera` with `device: jpeg` (its MIPI-CSI sensor), or an `esp_cam_sensor`. |
| `model` | enum | `msrmnp_s8_v1` | Espressif's detector: `msrmnp_s8_v1` (two stages, finds 5 landmarks, the only one recognition works with), `espdet_pico_224_224_face` or `espdet_pico_416_416_face` (one stage, boxes only; 416 finds smaller faces, more slowly). |
| `score_threshold` | float (0–1) | `0.3` | Minimum detection confidence to keep a face. |
| `nms_threshold` | float (0–1) | `0.5` | Non-max-suppression IoU threshold (overlapping-box merge). |
| `detection_interval` | int (1–600) | `8` | Offer one camera frame in N to the detector. A frame is only handed over when the previous detection has finished, so a slow one skips frames rather than queueing them. |
| `draw_enabled` | bool | `true` | Draw bounding boxes / names into the camera frames. |
| `recognition_enabled` | bool | `false` | Enable the recognition stage (embeddings + database). |
| `face_db_path` | string | `/sdcard/faces.db` | Path to the persisted face database (SD card). |
| `recognition_threshold` | float (0–1) | `0.7` | Minimum similarity to consider a face a match. esp-dl itself returns nothing below 0.5, so values under that act as 0.5. |
| `model_location` | enum | `flash_rodata` | Where the model weights live: `flash_rodata` (embedded) or `sdcard`. |
| `model_path` | string | `/sdcard` | The folder holding the `.espdl` files when `model_location: sdcard`. It must be on the card mounted at `/sdcard`, which is where esp-dl looks. |

`model_type` is still accepted with its only working value, `face_recognition`;
`yolo11` and `pose_detection` never ran and are now refused at validation.
`canvas_id` is accepted and no longer needed: the boxes are drawn into the
frames.

---

## Automations (triggers)

### `on_face_detected`
Fired after each detection that finds at least one face.
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
Fired when an enrolled face is matched -- once per visit: the face is not
asked about again until three detections in a row have found nobody.
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
| `face_detection.enroll` | — | Enroll the next face seen (auto-assigned ID; the log gives it). |
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
id(face_detect).get_detected_face_count();   // int  — faces in the last detection
id(face_detect).get_enrolled_count();        // int  — faces in the database
id(face_detect).get_last_recognized_name();  // std::string
id(face_detect).get_face_name(id);           // std::string
id(face_detect).enroll_face();               // int  — 0 when the request is taken
id(face_detect).delete_face(id);             // bool — the request is taken; the log says if it existed
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

1. **Capture** — `esp_video_camera` shows every frame to this component
   before the display and the JPEG encoder; `esp_cam_sensor` lends a buffer
   from the main loop. One frame in `detection_interval` is copied, shrunk to
   at most 800 px on its long side, and handed to the detection task when it
   is idle.
2. **Detect** — on that task, Espressif's detector: by default the two-stage
   `MSR_S8_V1` → `MNP_S8_V1`, which proposes candidates then refines them,
   yielding boxes + 5 landmarks, filtered by `score_threshold` /
   `nms_threshold`.
3. **Recognize** *(optional)* — each detected face is aligned and passed through
   the feature model (`MFN_S8_V1`) to produce an embedding, compared against the
   database with cosine similarity against `recognition_threshold`.
4. **Persist** — enrolled embeddings and names are saved to `face_db_path` on
   the SD card and reloaded on boot.
5. **Draw** *(optional)* — the last boxes and the recognized name are drawn
   into every frame until the next detection replaces them, so they follow
   the picture at the camera's rate.

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

  - source:
      type: git
      url: https://github.com/youkorr/esphome
      ref: esp_video_camera-lvgl
    components: [esp_video_camera, lvgl_camera_display]
    refresh: always

# Camera: any MIPI-CSI sensor esp_video drives (sc202cs, ov5647, sc2336,
# ov02c10...), auto-detected
esp_video_camera:
  id: tab5_cam
  name: Camera
  i2c_id: bsp_bus
  device: jpeg

# Optional: show the stream on an LVGL canvas; the boxes are already in it
lvgl_camera_display:
  camera_id: tab5_cam
  canvas_id: camera_canvas

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
- **CPU too busy:** raise `detection_interval`, or disable recognition
  (`recognition_enabled: false`) when you only need detection. The log prints
  how long a detection takes every 30 s at DEBUG level.
- **Small or distant faces:** `model: espdet_pico_416_416_face` (without
  recognition).
- **False matches:** raise `recognition_threshold` (e.g. `0.9`).
- **Recognition not persisting:** confirm the SD card is mounted and that
  `face_db_path` is writable. The component waits up to 30 s for the card at
  boot. A face database it cannot read back is renamed `*.bad` and a new one
  started.

---

## License

This project is licensed under the **MIT License** — see [`LICENSE`](LICENSE).

It is an **external component for ESPHome**. ESPHome is dual-licensed: its
Python codebase is MIT, while its C++/runtime code (`.c`, `.cpp`, `.h`, `.hpp`,
`.tcc`, `.ino`) is licensed under the **GPLv3**. The MIT terms of this component
are GPLv3-compatible, but firmware compiled together with the ESPHome runtime
is, as a combined work, subject to the GPLv3.

### Third-party components

The build fetches these from the ESP Component Registry; they are
**Espressif Systems (Shanghai) Co., Ltd.**'s, distributed under the MIT
License, Copyright © 2021 Espressif:

| Component | Upstream | License |
|---|---|---|
| `esp-dl` | [espressif/esp-dl](https://github.com/espressif/esp-dl) | MIT |
| `human_face_detect` (+ models) | [esp-dl/models/human_face_detect](https://github.com/espressif/esp-dl/tree/master/models/human_face_detect) | MIT |
| `human_face_recognition` (+ models) | [esp-dl/models/human_face_recognition](https://github.com/espressif/esp-dl/tree/master/models/human_face_recognition) | MIT |

Their license texts come with them, in the build's `managed_components/`.

## Credits

- [Espressif esp-dl](https://github.com/espressif/esp-dl) — detection /
  recognition models and inference kernels.
- [ESPHome](https://esphome.io) — the firmware framework this component plugs
  into.
