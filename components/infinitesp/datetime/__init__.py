import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import datetime
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"

InfinitESPDateTime = infinitesp_ns.class_("InfinitESPDateTime", datetime.TimeEntity, InfinitESPEntity)

# One flavor ("hold_until"), so unlike typed platforms there is no `type` key:
# the entity kind (TIME) comes from datetime.time_schema's default and the
# flavor is implicit. Zone-scoped like the cover platform.
CONFIG_SCHEMA = datetime.time_schema(InfinitESPDateTime).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=8),
    }
)

async def to_code(config):
    var = await datetime.new_datetime(config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    await register_infinitesp_entity(var, config)
