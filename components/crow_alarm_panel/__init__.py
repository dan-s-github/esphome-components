from esphome import pins, automation
import esphome.codegen as cg
import esphome.config_validation as cv
# NOTE: aliases are load-bearing. This package has child platforms named binary_sensor,
# switch, etc.; importing one of them sets it as an attribute on this package, silently
# shadowing an identically named module-level global. Import-time uses would survive, but
# these modules are needed later (schema validators, to_code), so alias them.
from esphome.components import alarm_control_panel as acp
from esphome.components import binary_sensor as binary_sensor_component
from esphome.components import switch as switch_component
from esphome.const import (
    CONF_ADDRESS,
    CONF_CODE,
    CONF_ENTITY_CATEGORY,
    CONF_ID,
    CONF_CLOCK_PIN,
    CONF_DATA_PIN,
    CONF_DEVICE_CLASS,
    CONF_ICON,
    CONF_NAME,
    CONF_OUTPUTS,
    ENTITY_CATEGORY_CONFIG,
)

AUTO_LOAD = ["binary_sensor", "text_sensor", "switch", "button", "alarm_control_panel"]
MULTI_CONF = True

CONF_ARMED_STATE = "armed_state"
CONF_CROW_ALARM_PANEL_ID = "crow_alarm_panel_id"
CONF_NUM_ZONES = "number_of_zones"
CONF_KEYPADS = "keypads"
CONF_ON_MESSAGE = "on_message"
CONF_ZONES = "zones"
CONF_ZONE = "zone"
# Internal keys populated by _zone_entry_defaults, not user-facing.
CONF_ZONE_SENSOR = "zone_sensor"
CONF_BYPASS_SWITCH = "bypass_switch"

crow_alarm_panel_ns = cg.esphome_ns.namespace("crow_alarm_panel")

CrowAlarmPanel = crow_alarm_panel_ns.class_("CrowAlarmPanel", cg.Component)
CrowAlarmControlPanel = crow_alarm_panel_ns.class_(
    "CrowAlarmControlPanel", acp.AlarmControlPanel, cg.Component
)
CrowAlarmPanelZoneBypassSwitch = crow_alarm_panel_ns.class_(
    "CrowAlarmPanelZoneBypassSwitch", switch_component.Switch, cg.Component
)


def _zone_entry_defaults(config):
    """Expand a terse zone entry into full entity configs.

    Each `zones:` entry creates a zone binary sensor and a bypass toggle switch. The
    switch doubles as the bypass state indicator — it is only ever published from the
    panel's ZONE_STATE bypass bitmap. Default names are "Zone {n}" / "Bypass {n}";
    a custom `name` yields "{name}" / "Bypass {name}".
    """
    config = config.copy()
    zone = config[CONF_ZONE]
    if CONF_NAME in config:
        zone_name = config[CONF_NAME]
        bypass_name = f"Bypass {config[CONF_NAME]}"
    else:
        zone_name = f"Zone {zone}"
        bypass_name = f"Bypass {zone}"

    zone_sensor = {CONF_NAME: zone_name}
    if CONF_DEVICE_CLASS in config:
        zone_sensor[CONF_DEVICE_CLASS] = config[CONF_DEVICE_CLASS]
    if CONF_ICON in config:
        zone_sensor[CONF_ICON] = config[CONF_ICON]
    config[CONF_ZONE_SENSOR] = binary_sensor_component.binary_sensor_schema()(zone_sensor)
    bypass_entity_category = config.get(CONF_ENTITY_CATEGORY, ENTITY_CATEGORY_CONFIG)
    config[CONF_BYPASS_SWITCH] = switch_component.switch_schema(CrowAlarmPanelZoneBypassSwitch)(
        {CONF_NAME: bypass_name, CONF_ENTITY_CATEGORY: bypass_entity_category}
    )
    return config


ZONE_ENTRY_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ZONE): cv.int_range(min=1, max=16),
            cv.Optional(CONF_NAME): cv.string,
            # Applied to the zone binary sensor; validated by the nested schema.
            cv.Optional(CONF_DEVICE_CLASS): cv.string,
            cv.Optional(CONF_ICON): cv.icon,
            # Applied to the bypass switch; defaults to "config" so bypass switches
            # appear in the HA device Configuration section rather than the main entity list.
            cv.Optional(CONF_ENTITY_CATEGORY): cv.entity_category,
        }
    ),
    _zone_entry_defaults,
)


def _no_duplicate_zones(value):
    seen = set()
    for entry in value:
        if entry[CONF_ZONE] in seen:
            raise cv.Invalid(f"Duplicate zone {entry[CONF_ZONE]} in zones")
        seen.add(entry[CONF_ZONE])
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CrowAlarmPanel),
        cv.Required(CONF_CLOCK_PIN): pins.internal_gpio_input_pin_schema,
        cv.Required(CONF_DATA_PIN): pins.internal_gpio_input_pin_schema,
        cv.Optional(CONF_ADDRESS): cv.int_range(min=0, max=7),
        # Panel-level alarm code; entities (disarm button, alarm control panel)
        # fall back to it when they don't set their own.
        cv.Optional(CONF_CODE): cv.string,
        cv.Optional(CONF_KEYPADS, default=[]): cv.ensure_list(
            cv.Schema(
                {
                    cv.Required(CONF_NAME): cv.string,
                    cv.Required(CONF_ADDRESS): cv.int_range(min=0, max=7),
                }
            )
        ),
        cv.Optional(CONF_ZONES, default=[]): cv.All(
            cv.ensure_list(ZONE_ENTRY_SCHEMA), _no_duplicate_zones
        ),
        cv.Optional(CONF_ON_MESSAGE): automation.validate_automation(single=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    clock_pin = await cg.gpio_pin_expression(config[CONF_CLOCK_PIN])
    cg.add(var.set_clock_pin(clock_pin))

    data_pin = await cg.gpio_pin_expression(config[CONF_DATA_PIN])
    cg.add(var.set_data_pin(data_pin))

    if CONF_ADDRESS in config:
        cg.add(var.set_keypad_address(config[CONF_ADDRESS]))

    if CONF_CODE in config:
        cg.add(var.set_code(config[CONF_CODE]))

    for keypad in config[CONF_KEYPADS]:
        cg.add(var.add_keypad(keypad[CONF_NAME], keypad[CONF_ADDRESS]))

    for zone_conf in config[CONF_ZONES]:
        zone = zone_conf[CONF_ZONE]
        zone_sens = await binary_sensor_component.new_binary_sensor(zone_conf[CONF_ZONE_SENSOR])
        cg.add(var.register_zone(zone_sens, zone))
        bypass_switch = await switch_component.new_switch(zone_conf[CONF_BYPASS_SWITCH])
        await cg.register_component(bypass_switch, zone_conf[CONF_BYPASS_SWITCH])
        cg.add(bypass_switch.set_parent(var))
        cg.add(bypass_switch.set_zone_number(zone))
        cg.add(var.register_zone_bypass_switch(bypass_switch, zone))

    if CONF_ON_MESSAGE in config:
        await automation.build_automation(
            var.get_on_message_trigger(),
            [(cg.uint8, "type"), (cg.std_vector.template(cg.uint8), "data")],
            config[CONF_ON_MESSAGE],
        )
