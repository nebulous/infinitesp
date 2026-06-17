import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import cover
from esphome.const import CONF_ID
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

# Zone damper cover. Reports damper position from the bus (register 0308,
# mirrored to 0319). device_class is set via YAML (use device_class: damper).
#
# Optional on_change fires whenever the cover's reported position doesn't match
# the commanded position — whether the command came from the thermostat (bus)
# or from Home Assistant — passing the new 0.0-1.0 position as `pos`. The cover
# never writes the ABCD bus; what the action does (log, drive relays, toggle
# GPIO) is up to the user. An un-attached trigger is a no-op, so omitting
# on_change keeps the cover as a pure position reporter.

InfinitESPCover = infinitesp_ns.class_("InfinitESPCover", cover.Cover, InfinitESPEntity)

CONF_ZONE = "zone"
CONF_ON_CHANGE = "on_change"

CONFIG_SCHEMA = cover.cover_schema(InfinitESPCover).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=4),
        cv.Optional(CONF_ON_CHANGE): automation.validate_automation(single=True),
    }
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cover.register_cover(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_bus_class(6))  # 0x60 >> 4
    if CONF_ON_CHANGE in config:
        await automation.build_automation(
            var.get_change_trigger(), [(float, "pos")], config[CONF_ON_CHANGE]
        )
    await register_infinitesp_entity(var, config)
