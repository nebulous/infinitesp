import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@nebulous"]
DEPENDENCIES = ["uart"]
MULTI_CONF = True

CONF_INFINITESP_ID = "infinitesp_id"

sam_ascii_ns = cg.esphome_ns.namespace("sam_ascii")
SamAsciiComponent = sam_ascii_ns.class_("SamAsciiComponent", cg.Component, uart.UARTDevice)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(SamAsciiComponent),
    cv.Required(CONF_INFINITESP_ID): cv.use_id("infinitesp::InfinitESPComponent"),
}).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    parent = await cg.get_variable(config[CONF_INFINITESP_ID])
    cg.add(var.set_infinitesp(parent))
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
