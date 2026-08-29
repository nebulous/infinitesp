import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate
from esphome.const import CONF_ID
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity
from ..auto_entities import ZONE_ENTITY_FLAGS

CONF_ZONE = "zone"

InfinitESPClimate = infinitesp_ns.class_("InfinitESPClimate", climate.Climate, InfinitESPEntity)

CONFIG_SCHEMA = climate.climate_schema(InfinitESPClimate).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=8),
    }
).extend(
    # Per-zone entity opt-outs (default on). The entity matrix itself lives
    # in auto_entities.py; explicitly declared entities always win regardless.
    {cv.Optional(flag, default=True): validator for flag, validator in ZONE_ENTITY_FLAGS.items()}
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await climate.register_climate(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    await register_infinitesp_entity(var, config)

    from .. import auto_entities

    await auto_entities.spawn_zone_entities(config)
