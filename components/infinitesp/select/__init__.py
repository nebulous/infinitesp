import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_ID, CONF_TYPE
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"

InfinitESPSelect = infinitesp_ns.class_("InfinitESPSelect", select.Select, InfinitESPEntity)

SELECT_TYPES = {
    "system_mode": {
        "key": "system_mode",
        "options": ["heat", "cool", "auto", "emergency_heat", "off"],
    },
    "fan_mode": {
        "key": "fan_mode",
        "options": ["auto", "low", "med", "high"],
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
    await select.register_select(var, config, options=info["options"])
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_select_type(info["key"]))
    await register_infinitesp_entity(var, config)
