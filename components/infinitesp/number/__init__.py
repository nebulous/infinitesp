import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"

InfinitESPNumber = infinitesp_ns.class_("InfinitESPNumber", number.Number, InfinitESPEntity)

# One flavor ("hold_minutes" — remaining timed-hold minutes, settable), so no
# `type` key. Zone-scoped like the cover and datetime platforms. NOT a
# Component: see infinitesp_number.h for why the hub owns the debounce.
CONFIG_SCHEMA = number.number_schema(
    InfinitESPNumber, unit_of_measurement="min", icon="mdi:timer-outline"
).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=8),
    }
)

async def to_code(config):
    var = await number.new_number(
        config,
        min_value=0,
        max_value=1425,
        step=15,  # the thermostat's timed-hold grid
    )
    cg.add(var.set_zone(config[CONF_ZONE]))
    await register_infinitesp_entity(var, config)
