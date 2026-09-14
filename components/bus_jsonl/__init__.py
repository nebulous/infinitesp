import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@nebulous"]
DEPENDENCIES = ["uart", "infinitesp"]

CONF_INFINITESP_ID = "infinitesp_id"

bus_jsonl_ns = cg.esphome_ns.namespace("bus_jsonl")
BusJsonlComponent = bus_jsonl_ns.class_("BusJsonlComponent", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(BusJsonlComponent),
    cv.Required(CONF_INFINITESP_ID): cv.use_id("infinitesp::InfinitESPComponent"),
}).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_INFINITESP_ID])
    cg.add(parent.set_bus_jsonl(var))
    # Timestamps come from whatever time platform the config declares (there
    # is never a choice to make; first one wins if several are listed).
    # ESPHome has no "global clock" lookup, so read the validated `time:`
    # section for its generated id. The epoch provider stays a lambda in
    # main.cpp, which has the time header whenever a time platform is
    # configured (issue #26 staging rule); with no time platform no lambda
    # is generated and lines fall back to boot-relative ms.
    time_platforms = CORE.config.get("time", [])
    if time_platforms:
        time_id = time_platforms[0][CONF_ID]
        await cg.get_variable(time_id)  # config-time existence check
        cg.add(
            var.set_epoch_provider(
                cg.RawExpression(
                    f"[]() -> time_t {{ auto t = id({time_id.id}).now(); "
                    f"return t.is_valid() ? t.timestamp : (time_t) 0; }}"
                )
            )
        )
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
