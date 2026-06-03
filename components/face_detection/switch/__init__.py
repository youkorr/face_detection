import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import ENTITY_CATEGORY_CONFIG

from .. import CONF_FACE_DETECTION_ID, FaceDetectionComponent, face_detection_ns

DEPENDENCIES = ["face_detection"]

FaceDetectionSwitch = face_detection_ns.class_(
    "FaceDetectionSwitch", switch.Switch, cg.Component
)

CONFIG_SCHEMA = (
    switch.switch_schema(
        FaceDetectionSwitch,
        entity_category=ENTITY_CATEGORY_CONFIG,
        default_restore_mode="RESTORE_DEFAULT_ON",
        icon="mdi:face-recognition",
    )
    .extend(
        {
            cv.GenerateID(CONF_FACE_DETECTION_ID): cv.use_id(FaceDetectionComponent),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_FACE_DETECTION_ID])
    cg.add(var.set_parent(parent))
