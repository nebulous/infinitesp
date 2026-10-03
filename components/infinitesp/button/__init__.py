import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

InfinitESPButton = infinitesp_ns.class_("InfinitESPButton", button.Button, InfinitESPEntity)

# Single flavor: the manual clock-sync trigger ("Sync Thermostat Clock",
# Diagnostic). Fires InfinitESPComponent::sync_clock_from_source() — the
# same core as sam_ascii TIME!NOW and the infinitesp.sync_clock action.
# Fire-and-forget by design; the outcome lands in the clock_sync text
# sensor. Manual-only (Q11): nothing may call this on a schedule.
CONFIG_SCHEMA = button.button_schema(InfinitESPButton).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
    }
)

async def to_code(config):
    var = await button.new_button(config)
    await register_infinitesp_entity(var, config)
