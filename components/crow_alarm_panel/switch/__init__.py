import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE, CONF_OUTPUT
from .. import (
    crow_alarm_panel_ns,
    CrowAlarmPanel,
    CrowAlarmPanelZoneBypassSwitch,
    CONF_CROW_ALARM_PANEL_ID,
    CONF_ZONE,
)

DEPENDENCIES = ["crow_alarm_panel"]

CONF_BYPASS = "bypass"

CrowAlarmPanelSwitch = crow_alarm_panel_ns.class_(
    "CrowAlarmPanelSwitch", switch.Switch, cg.Component
)
CrowAlarmPanelOutputSwitch = crow_alarm_panel_ns.class_(
    "CrowAlarmPanelOutputSwitch", CrowAlarmPanelSwitch
)


CROW_SWITCH_SCHEMA = switch.switch_schema(CrowAlarmPanelSwitch).extend(
    {
        cv.GenerateID(CONF_CROW_ALARM_PANEL_ID): cv.use_id(CrowAlarmPanel),
    }
).extend(cv.COMPONENT_SCHEMA)


CONFIG_SCHEMA = cv.typed_schema(
    {
        CONF_OUTPUT: CROW_SWITCH_SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(CrowAlarmPanelOutputSwitch),
                cv.Required(CONF_OUTPUT): cv.int_range(min=1, max=8),
            }
        ),
        CONF_BYPASS: CROW_SWITCH_SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(CrowAlarmPanelZoneBypassSwitch),
                cv.Required(CONF_ZONE): cv.int_range(min=1, max=16),
            }
        ),
    }
)


async def to_code(config):
    paren = await cg.get_variable(config[CONF_CROW_ALARM_PANEL_ID])
    var = cg.new_Pvariable(config[CONF_ID])
    type = config[CONF_TYPE]
    if type == CONF_OUTPUT:
        cg.add(var.set_crow_alarm_panel_parent(paren))
        cg.add(var.set_output_number(config[CONF_OUTPUT]))
        cg.add(paren.register_output_switch(var, config[CONF_OUTPUT]))
    elif type == CONF_BYPASS:
        cg.add(var.set_parent(paren))
        cg.add(var.set_zone_number(config[CONF_ZONE]))
        cg.add(paren.register_zone_bypass_switch(var, config[CONF_ZONE]))

    await switch.register_switch(var, config)
    await cg.register_component(var, config)
