import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_ID, CONF_TYPE
from .. import InfinitESPDevice, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_device

InfinitESPBinarySensor = infinitesp_ns.class_("InfinitESPBinarySensor", binary_sensor.BinarySensor, InfinitESPDevice)

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(InfinitESPBinarySensor).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_TYPE): cv.one_of("bus_status", "electric_heat", "compressor_running", lower=True),
    }
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_sensor_type(config[CONF_TYPE]))
    await binary_sensor.register_binary_sensor(var, config)
    await register_infinitesp_device(var, config)
