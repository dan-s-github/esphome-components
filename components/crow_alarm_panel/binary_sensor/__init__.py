import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, CONF_TYPE, DEVICE_CLASS_PROBLEM
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

# Panel fault indicators decoded from CONTROLLER_STATUS (docs/protocol_wire_format.md,
# "Fault bits"). `trouble` is on while a fault is current; `trouble_latched`
# mirrors the keypad's TROUBLE latch, which stays on until the fault is viewed on a
# keypad or the system is armed.
TYPE_TROUBLE = "trouble"
TYPE_TROUBLE_LATCHED = "trouble_latched"

TROUBLE_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_PROBLEM
).extend(
    {
        cv.GenerateID(): cv.declare_id(BinarySensor),
        cv.GenerateID(CONF_CROW_ALARM_PANEL_ID): cv.use_id(CrowAlarmPanel),
    }
)

# Bypass state is exposed via the switch platform (`type: bypass`) or the parent
# `zones:` config — the switch is both the control and the state indicator.
CONFIG_SCHEMA = cv.typed_schema(
    {
        CONF_ZONE: ZONE_SCHEMA,
        TYPE_TROUBLE: TROUBLE_SCHEMA,
        TYPE_TROUBLE_LATCHED: TROUBLE_SCHEMA,
    }
)


async def to_code(config):
    paren = await cg.get_variable(config[CONF_CROW_ALARM_PANEL_ID])
    var = cg.new_Pvariable(config[CONF_ID])

    await binary_sensor.register_binary_sensor(var, config)

    if config[CONF_TYPE] == TYPE_TROUBLE:
        cg.add(paren.register_trouble(var))
    elif config[CONF_TYPE] == TYPE_TROUBLE_LATCHED:
        cg.add(paren.register_trouble_latched(var))
    else:
        cg.add(paren.register_zone(var, config[CONF_ZONE]))
