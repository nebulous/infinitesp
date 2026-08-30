import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_ID, CONF_TYPE
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"
CONF_DEVICE_ADDRESS = "device_address"
# Named bus_class, not device_class: the stock text_sensor schema owns
# device_class (string registry) and setup_text_sensor_core_ feeds it to
# register_device_class unconditionally.
CONF_BUS_CLASS = "bus_class"

InfinitESPTextSensor = infinitesp_ns.class_("InfinitESPTextSensor", text_sensor.TextSensor, InfinitESPEntity)

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
    "manufacture_date": "manufacture_date",
    "version": "version",
}

CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(InfinitESPTextSensor).extend(
        {
            cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
            cv.Required(CONF_TYPE): cv.one_of(*TEXT_SENSOR_TYPES, lower=True),
            cv.Optional(CONF_ZONE, default=1): cv.int_range(min=1, max=8),
            # device_address: exact bus node (manufacture_date only reads it).
            # bus_class: the dispatch class gate (base InfinitESPEntity
            # concept); what generated entities use since the low nibble
            # varies across installs. Mutually exclusive.
            cv.Optional(CONF_DEVICE_ADDRESS): cv.hex_uint8_t,
            cv.Optional(CONF_BUS_CLASS): cv.int_range(min=1, max=15),
        }
    ),
    cv.has_at_most_one_key(CONF_DEVICE_ADDRESS, CONF_BUS_CLASS),
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await text_sensor.register_text_sensor(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_sensor_type(TEXT_SENSOR_TYPES[config[CONF_TYPE]]))
    if CONF_DEVICE_ADDRESS in config:
        cg.add(var.set_device_address(config[CONF_DEVICE_ADDRESS]))
    if CONF_BUS_CLASS in config:
        cg.add(var.set_bus_class(config[CONF_BUS_CLASS]))
    await register_infinitesp_entity(var, config)
    if TEXT_SENSOR_TYPES[config[CONF_TYPE]] == "version":
        parent = await cg.get_variable(config[CONF_INFINITESP_ID])
        cg.add(parent.set_version_text_sensor(var))
