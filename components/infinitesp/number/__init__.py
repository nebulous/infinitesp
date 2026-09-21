import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ICON, CONF_TYPE, CONF_UNIT_OF_MEASUREMENT
from .. import (
    InfinitESPEntity,
    CONF_INFINITESP_ID,
    infinitesp_ns,
    register_infinitesp_entity,
)

NumberType = infinitesp_ns.enum("NumberType")

CONF_ZONE = "zone"

InfinitESPNumber = infinitesp_ns.class_("InfinitESPNumber", number.Number, InfinitESPEntity)

# Two flavors, keyed on `type`:
#   hold_minutes  — remaining timed-hold minutes, settable (zone-scoped).
#   vacation_hours — vacation duration in hours at the bus's native 1-h
#                   resolution, 0 clears (issue #33; system-wide, zone ignored
#                   like system_mode). The thermostat serves no countdown back
#                   (4012 is config-only), so the read side is the commanded
#                   value, not a ticking remaining.
# NOT a Component: see infinitesp_number.h for why the hub owns the timing.
NUMBER_TYPES = {"hold_minutes", "vacation_hours"}

# Vacation hours ceiling: the bus field accepts 32767, but the wall UI and
# SAM01 cap at 365 days, so enforce their 8760-hour limit.
VACATION_HOURS_MAX = 8760


def _require_zone(config):
    if config[CONF_TYPE] == "hold_minutes" and CONF_ZONE not in config:
        raise cv.Invalid("zone is required for type: hold_minutes")
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(InfinitESPNumber).extend(
        {
            cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
            # Default keeps pre-type-key hold_minutes configs valid unchanged.
            cv.Optional(CONF_TYPE, default="hold_minutes"): cv.one_of(
                *NUMBER_TYPES, lower=True
            ),
            # Optional so vacation_hours (system-wide) can omit it.
            cv.Optional(CONF_ZONE): cv.int_range(min=1, max=8),
        }
    ),
    _require_zone,
)


async def to_code(config):
    stype = config[CONF_TYPE]
    # unit/icon are consumed from the config dict by new_number's setup_entity
    # chain (no public setters on the entity), so inject per-flavor values
    # before registration — same net effect as the old schema-level kwargs.
    if stype == "vacation_hours":
        config[CONF_UNIT_OF_MEASUREMENT] = "h"
        config[CONF_ICON] = "mdi:beach"
        var = await number.new_number(config, min_value=0, max_value=VACATION_HOURS_MAX, step=1)
        cg.add(var.set_number_type(NumberType.NUMBER_VACATION_HOURS))
    else:
        config[CONF_UNIT_OF_MEASUREMENT] = "min"
        config[CONF_ICON] = "mdi:timer-outline"
        # step 15 = the thermostat's timed-hold grid
        var = await number.new_number(config, min_value=0, max_value=1425, step=15)
        cg.add(var.set_number_type(NumberType.NUMBER_HOLD_MINUTES))
        cg.add(var.set_zone(config[CONF_ZONE]))
    await register_infinitesp_entity(var, config)
