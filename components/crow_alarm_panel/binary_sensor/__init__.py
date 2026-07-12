import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, CONF_TYPE
from .. import CrowAlarmPanel, CONF_CROW_ALARM_PANEL_ID, CONF_ZONE

DEPENDENCIES = ["crow_alarm_panel"]

binary_sensor_ns = cg.esphome_ns.namespace("binary_sensor")
BinarySensor = binary_sensor_ns.class_("BinarySensor", cg.EntityBase)

ZONE_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(): cv.declare_id(BinarySensor),
        cv.GenerateID(CONF_CROW_ALARM_PANEL_ID): cv.use_id(CrowAlarmPanel),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=16),
    }
).extend(cv.COMPONENT_SCHEMA)

# Bypass state is exposed via the switch platform (`type: bypass`) or the parent
# `zones:` config — the switch is both the control and the state indicator.
CONFIG_SCHEMA = cv.typed_schema(
    {
        CONF_ZONE: ZONE_SCHEMA,
    }
)


async def to_code(config):
    paren = await cg.get_variable(config[CONF_CROW_ALARM_PANEL_ID])
    var = cg.new_Pvariable(config[CONF_ID])

    await binary_sensor.register_binary_sensor(var, config)

    cg.add(paren.register_zone(var, config[CONF_ZONE]))
