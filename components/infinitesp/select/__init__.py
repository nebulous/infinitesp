import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_TYPE
from .. import (
    InfinitESPEntity,
    CONF_INFINITESP_ID,
    infinitesp_ns,
    register_infinitesp_entity,
    system_mode_options,
)

CONF_ZONE = "zone"

InfinitESPSelect = infinitesp_ns.class_("InfinitESPSelect", select.Select, InfinitESPEntity)

SELECT_TYPES = {
    "system_mode": {
        "key": "system_mode",
        # Base options only; emergency_heat/heat_pump are appended by
        # system_mode_options() when a hub sets experimental_heat_source_modes
        # (they do not select the heat source and misbehave on some tstats).
        "options": ["heat", "cool", "auto", "off"],
    },
    "fan_mode": {
        "key": "fan_mode",
        "options": ["auto", "low", "med", "high"],
    },
    # Which zone the wall control displays (SAM 3B02 byte 28). Manual-only,
    # system-wide, zone key ignored (same as system_mode). Reads track the
    # display; writes are ACKed by every tstat tested and adopted only by the
    # older UI family (issue #37). Options are plain strings so the state maps
    # straight to the register byte.
    "displayed_zone": {
        "key": "displayed_zone",
        "options": ["1", "2", "3", "4", "5", "6", "7", "8"],
    },
}

CONFIG_SCHEMA = select.select_schema(InfinitESPSelect).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_TYPE): cv.one_of(*SELECT_TYPES, lower=True),
        cv.Optional(CONF_ZONE, default=1): cv.int_range(min=1, max=8),
    }
)

async def to_code(config):
    stype = config[CONF_TYPE]
    info = SELECT_TYPES[stype]
    var = cg.new_Pvariable(config[CONF_ID])
    options = system_mode_options() if stype == "system_mode" else info["options"]
    await select.register_select(var, config, options=options)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_select_type(info["key"]))
    await register_infinitesp_entity(var, config)
