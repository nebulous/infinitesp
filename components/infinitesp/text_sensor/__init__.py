import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID, CONF_TYPE
from .. import InfinitESPDevice, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_device

CONF_ZONE = "zone"

InfinitESPTextSensor = infinitesp_ns.class_("InfinitESPTextSensor", text_sensor.TextSensor, InfinitESPDevice)

TEXT_SENSOR_TYPES = {
    "zone_name": "zone_name",
    "hold_state": "hold_state",
    "tstat_ssid": "tstat_ssid",
    "tstat_hostname": "tstat_hostname",
    "tstat_wifi_mac": "tstat_wifi_mac",
    "tstat_cloud_host": "tstat_cloud_host",
    "tstat_proxy_server": "tstat_proxy_server",
    "tstat_dealer_name": "tstat_dealer_name",
    "tstat_dealer_brand": "tstat_dealer_brand",
    "tstat_dealer_url": "tstat_dealer_url",
    "comfort_profile": "comfort_profile",
    "fault_history": "fault_history",
}

CONFIG_SCHEMA = text_sensor.text_sensor_schema(InfinitESPTextSensor).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_TYPE): cv.one_of(*TEXT_SENSOR_TYPES, lower=True),
        cv.Optional(CONF_ZONE, default=1): cv.int_range(min=1, max=8),
    }
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await text_sensor.register_text_sensor(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_sensor_type(TEXT_SENSOR_TYPES[config[CONF_TYPE]]))
    await register_infinitesp_device(var, config)
