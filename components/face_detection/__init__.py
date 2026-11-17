import esphome.codegen as cg
from esphome import automation
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
import esphome.final_validate as fv

CODEOWNERS = ["@youkorr"]
# No dependency on a camera component: either kind is accepted (see
# _check_camera), and naming one here would make the other impossible.
DEPENDENCIES = ["esp32"]

# Espressif's face models, from the ESP Component Registry. human_face_detect
# 0.5.x brings esp-dl 3.3.x with it, and human_face_recognition 0.3.x asks for
# human_face_detect ~0.5, so these two pins decide the whole stack.
HUMAN_FACE_DETECT_VERSION = "0.5.2"
HUMAN_FACE_RECOGNITION_VERSION = "0.3.3"

CONF_CAMERA_ID = "camera_id"
CONF_CANVAS_ID = "canvas_id"
CONF_MODEL = "model"
CONF_MODEL_TYPE = "model_type"
CONF_SCORE_THRESHOLD = "score_threshold"
CONF_NMS_THRESHOLD = "nms_threshold"
CONF_RECOGNITION_ENABLED = "recognition_enabled"
CONF_FACE_DB_PATH = "face_db_path"
CONF_RECOGNITION_THRESHOLD = "recognition_threshold"
CONF_ON_FACE_DETECTED = "on_face_detected"
CONF_ON_FACE_RECOGNIZED = "on_face_recognized"
CONF_DETECTION_INTERVAL = "detection_interval"
CONF_DRAW_ENABLED = "draw_enabled"
CONF_DRAW_LABELS = "draw_labels"
CONF_UNKNOWN_LABEL = "unknown_label"
CONF_MODEL_LOCATION = "model_location"
CONF_MODEL_PATH = "model_path"
CONF_FACE_ID = "face_id"

MODEL_LOCATION_FLASH = "flash_rodata"
MODEL_LOCATION_SDCARD = "sdcard"

# The SD card the models are read from is wherever esp-dl looks for it:
# CONFIG_BSP_SD_MOUNT_POINT, which only an Espressif BSP defines, and "/sdcard"
# otherwise. A board config cannot move it, so model_path must be under it.
SD_MOUNT_POINT = "/sdcard"

# Espressif's three face detectors (HumanFaceDetect::model_type_t), each with
# the Kconfig option that puts it in flash and the one that makes it the
# default. Only MSRMNP finds the five landmarks recognition aligns a face on.
MODELS = {
    "msrmnp_s8_v1": (
        0,
        "CONFIG_FLASH_HUMAN_FACE_DETECT_MSRMNP_S8_V1",
        "CONFIG_HUMAN_FACE_DETECT_MSRMNP_S8_V1",
    ),
    "espdet_pico_224_224_face": (
        1,
        "CONFIG_FLASH_ESPDET_PICO_224_224_FACE",
        "CONFIG_ESPDET_PICO_224_224_FACE",
    ),
    "espdet_pico_416_416_face": (
        2,
        "CONFIG_FLASH_ESPDET_PICO_416_416_FACE",
        "CONFIG_ESPDET_PICO_416_416_FACE",
    ),
}
MODEL_WITH_LANDMARKS = "msrmnp_s8_v1"

face_detection_ns = cg.esphome_ns.namespace("face_detection")
FaceDetectionComponent = face_detection_ns.class_(
    "FaceDetectionComponent", cg.Component
)

FaceDetectedTrigger = face_detection_ns.class_(
    "FaceDetectedTrigger", automation.Trigger.template(cg.int_)
)
FaceRecognizedTrigger = face_detection_ns.class_(
    "FaceRecognizedTrigger", automation.Trigger.template(cg.int_, cg.float_)
)

EnrollFaceAction = face_detection_ns.class_("EnrollFaceAction", automation.Action)
EnrollFaceWithNameAction = face_detection_ns.class_(
    "EnrollFaceWithNameAction", automation.Action
)
SetFaceNameAction = face_detection_ns.class_("SetFaceNameAction", automation.Action)
DeleteFaceAction = face_detection_ns.class_("DeleteFaceAction", automation.Action)
ClearAllFacesAction = face_detection_ns.class_(
    "ClearAllFacesAction", automation.Action
)

# The two cameras the frames can come from, named here rather than imported so
# that a board with only one of them installed still loads this file. A class
# is matched by its C++ name, so these compare equal to the components' own.
ESPVideoCamera = cg.esphome_ns.namespace("esp_video_camera").class_(
    "ESPVideoCamera", cg.Component
)
MipiDsiCam = cg.esphome_ns.namespace("esp_cam_sensor").class_(
    "MipiDSICamComponent", cg.Component
)
# esp_video_camera's MIPI-CSI source, the only one with pixels to look at. A
# USB camera (device: uvc) and any /dev/videoN delivering JPEG have none.
ESP_VIDEO_MIPI_DEVICES = ("jpeg", "/dev/video10")


def _model_type(value):
    # The option once offered yolo11 and pose_detection, which never ran: setup
    # failed on anything but face_recognition. Say so instead of building them.
    value = cv.string_strict(value).lower()
    if value != "face_recognition":
        raise cv.Invalid(
            f"model_type '{value}' does not exist in this component; only "
            "face_recognition does, and it is the default. To choose Espressif's "
            "face detector, use 'model:'."
        )
    return value


def _model_path(value):
    value = cv.string_strict(value).rstrip("/")
    if value != SD_MOUNT_POINT and not value.startswith(SD_MOUNT_POINT + "/"):
        raise cv.Invalid(
            f"model_path must be a folder on the SD card mounted at {SD_MOUNT_POINT}: "
            "esp-dl reads the models from there and nowhere else"
        )
    return value


def _recognition_needs_landmarks(config):
    if config[CONF_RECOGNITION_ENABLED] and config[CONF_MODEL] != MODEL_WITH_LANDMARKS:
        raise cv.Invalid(
            f"recognition_enabled needs model: {MODEL_WITH_LANDMARKS}. Recognition "
            "aligns each face on five landmarks, and only that detector finds them.",
            path=[CONF_MODEL],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FaceDetectionComponent),
            # Either camera; _check_camera says which ones can work.
            cv.Required(CONF_CAMERA_ID): cv.use_id(cg.Component),
            cv.Optional(CONF_CANVAS_ID): cv.string,
            cv.Optional(CONF_MODEL, default=MODEL_WITH_LANDMARKS): cv.one_of(
                *MODELS, lower=True
            ),
            cv.Optional(CONF_MODEL_TYPE): _model_type,
            cv.Optional(CONF_SCORE_THRESHOLD, default=0.3): cv.float_range(
                min=0.0, max=1.0
            ),
            cv.Optional(CONF_NMS_THRESHOLD, default=0.5): cv.float_range(
                min=0.0, max=1.0
            ),
            # Offer one frame in N to the detector, and only when it has finished
            # with the last one: a slow detection skips frames, it never queues.
            cv.Optional(CONF_DETECTION_INTERVAL, default=8): cv.int_range(
                min=1, max=600
            ),
            cv.Optional(CONF_DRAW_ENABLED, default=True): cv.boolean,
            # A label in each box: the name of a recognized face, this for the
            # others. The font has capitals, digits and - . ' only; accented
            # letters are drawn plain.
            cv.Optional(CONF_DRAW_LABELS, default=True): cv.boolean,
            cv.Optional(CONF_UNKNOWN_LABEL, default="Personne"): cv.string,
            cv.Optional(CONF_RECOGNITION_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_FACE_DB_PATH, default="/sdcard/faces.db"): cv.string,
            cv.Optional(CONF_RECOGNITION_THRESHOLD, default=0.7): cv.float_range(
                min=0.0, max=1.0
            ),
            cv.Optional(CONF_MODEL_LOCATION, default=MODEL_LOCATION_FLASH): cv.one_of(
                MODEL_LOCATION_FLASH, MODEL_LOCATION_SDCARD, lower=True
            ),
            cv.Optional(CONF_MODEL_PATH, default=SD_MOUNT_POINT): _model_path,
            cv.Optional(CONF_ON_FACE_DETECTED): automation.validate_automation(
                {
                    cv.GenerateID(): cv.declare_id(FaceDetectedTrigger),
                }
            ),
            cv.Optional(CONF_ON_FACE_RECOGNIZED): automation.validate_automation(
                {
                    cv.GenerateID(): cv.declare_id(FaceRecognizedTrigger),
                }
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32P4]),
    _recognition_needs_landmarks,
)


def _check_camera(config):
    full_config = fv.full_config.get()
    camera_id = config[CONF_CAMERA_ID]
    declared = next(
        (d for d, _ in full_config.declare_ids if d.id == str(camera_id)), None
    )
    if declared is None:
        return config  # use_id has already said the id does not exist
    if declared.type.inherits_from(MipiDsiCam):
        return config
    if not declared.type.inherits_from(ESPVideoCamera):
        raise cv.Invalid(
            f"'{camera_id}' is not a camera face_detection can read: it takes "
            "esp_video_camera or esp_cam_sensor",
            path=[CONF_CAMERA_ID],
        )
    camera_config = full_config.get_config_for_path(
        full_config.get_path_for_id(camera_id)[:-1]
    )
    device = str(camera_config.get("device", "jpeg"))
    if device not in ESP_VIDEO_MIPI_DEVICES:
        raise cv.Invalid(
            f"'{camera_id}' reads device '{device}', which delivers JPEG: there are "
            "no pixels to look for faces in. Face detection needs the MIPI-CSI "
            "sensor (device: jpeg).",
            path=[CONF_CAMERA_ID],
        )
    return config


FINAL_VALIDATE_SCHEMA = _check_camera


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    camera_full_id, camera = await cg.get_variable_with_full_id(
        config[CONF_CAMERA_ID]
    )
    if camera_full_id.type.inherits_from(ESPVideoCamera):
        cg.add_define("USE_FACE_DETECTION_ESP_VIDEO_CAMERA")
    else:
        cg.add_define("USE_FACE_DETECTION_ESP_CAM_SENSOR")
    cg.add(var.set_camera(camera))

    model, flash_option, default_option = MODELS[config[CONF_MODEL]]
    cg.add(var.set_model(model))

    if CONF_CANVAS_ID in config:
        cg.add(var.set_canvas_id(config[CONF_CANVAS_ID]))
    cg.add(var.set_score_threshold(config[CONF_SCORE_THRESHOLD]))
    cg.add(var.set_nms_threshold(config[CONF_NMS_THRESHOLD]))
    cg.add(var.set_detection_interval(config[CONF_DETECTION_INTERVAL]))
    cg.add(var.set_draw_enabled(config[CONF_DRAW_ENABLED]))
    cg.add(var.set_draw_labels(config[CONF_DRAW_LABELS]))
    cg.add(var.set_unknown_label(config[CONF_UNKNOWN_LABEL]))

    recognition = config[CONF_RECOGNITION_ENABLED]
    if recognition:
        cg.add_define("USE_FACE_DETECTION_RECOGNITION")
        cg.add(var.set_face_db_path(config[CONF_FACE_DB_PATH]))
        cg.add(var.set_recognition_threshold(config[CONF_RECOGNITION_THRESHOLD]))

    for conf in config.get(CONF_ON_FACE_DETECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(trigger, [(cg.int_, "face_count")], conf)

    for conf in config.get(CONF_ON_FACE_RECOGNIZED, []):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(
            trigger, [(cg.int_, "face_id"), (cg.float_, "similarity")], conf
        )

    # Espressif's models, as published. The recognizer's feature model is a
    # megabyte of flash, so it is only brought in when recognition is on.
    add_idf_component(
        name="espressif/human_face_detect", ref=HUMAN_FACE_DETECT_VERSION
    )
    if recognition:
        add_idf_component(
            name="espressif/human_face_recognition",
            ref=HUMAN_FACE_RECOGNITION_VERSION,
        )

    on_sdcard = config[CONF_MODEL_LOCATION] == MODEL_LOCATION_SDCARD
    if on_sdcard:
        # esp-dl joins the mount point, this folder and the model's own file
        # name, so the folder is given relative to the mount point.
        folder = config[CONF_MODEL_PATH][len(SD_MOUNT_POINT) :].lstrip("/")
        cg.add(var.set_model_dir(config[CONF_MODEL_PATH]))
        add_idf_sdkconfig_option("CONFIG_HUMAN_FACE_DETECT_MODEL_IN_SDCARD", True)
        add_idf_sdkconfig_option("CONFIG_HUMAN_FACE_DETECT_MODEL_SDCARD_DIR", folder)
        if recognition:
            add_idf_sdkconfig_option("CONFIG_HUMAN_FACE_FEAT_MODEL_IN_SDCARD", True)
            add_idf_sdkconfig_option(
                "CONFIG_HUMAN_FACE_FEAT_MODEL_SDCARD_DIR", folder
            )
    else:
        add_idf_sdkconfig_option(
            "CONFIG_HUMAN_FACE_DETECT_MODEL_IN_FLASH_RODATA", True
        )
        # Only the chosen detector goes into flash; the component's own default
        # would add MSRMNP to an ESPDet build for nothing.
        for _, other_flash, _ in MODELS.values():
            add_idf_sdkconfig_option(other_flash, other_flash == flash_option)
        if recognition:
            add_idf_sdkconfig_option(
                "CONFIG_HUMAN_FACE_FEAT_MODEL_IN_FLASH_RODATA", True
            )
    add_idf_sdkconfig_option(default_option, True)


ENROLL_FACE_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FaceDetectionComponent),
    }
)

ENROLL_FACE_WITH_NAME_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FaceDetectionComponent),
        cv.Required(CONF_NAME): cv.templatable(cv.string),
    }
)

SET_FACE_NAME_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FaceDetectionComponent),
        cv.Required(CONF_FACE_ID): cv.templatable(cv.int_),
        cv.Required(CONF_NAME): cv.templatable(cv.string),
    }
)

DELETE_FACE_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FaceDetectionComponent),
        cv.Required(CONF_FACE_ID): cv.templatable(cv.int_),
    }
)

CLEAR_ALL_FACES_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(FaceDetectionComponent),
    }
)


@automation.register_action(
    "face_detection.enroll",
    EnrollFaceAction,
    ENROLL_FACE_ACTION_SCHEMA,
    synchronous=True,
)
async def enroll_face_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "face_detection.enroll_with_name",
    EnrollFaceWithNameAction,
    ENROLL_FACE_WITH_NAME_ACTION_SCHEMA,
    synchronous=True,
)
async def enroll_face_with_name_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(template_))
    return var


@automation.register_action(
    "face_detection.set_name",
    SetFaceNameAction,
    SET_FACE_NAME_ACTION_SCHEMA,
    synchronous=True,
)
async def set_face_name_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_id = await cg.templatable(config[CONF_FACE_ID], args, cg.int_)
    cg.add(var.set_face_id(template_id))
    template_name = await cg.templatable(config[CONF_NAME], args, cg.std_string)
    cg.add(var.set_name(template_name))
    return var


@automation.register_action(
    "face_detection.delete",
    DeleteFaceAction,
    DELETE_FACE_ACTION_SCHEMA,
    synchronous=True,
)
async def delete_face_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_FACE_ID], args, cg.int_)
    cg.add(var.set_face_id(template_))
    return var


@automation.register_action(
    "face_detection.clear_all",
    ClearAllFacesAction,
    CLEAR_ALL_FACES_ACTION_SCHEMA,
    synchronous=True,
)
async def clear_all_faces_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
