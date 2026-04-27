import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE, STATE_CLASS_MEASUREMENT
from .. import InfinitESPDevice, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_device

CONF_ZONE = "zone"

InfinitESPSensor = infinitesp_ns.class_("InfinitESPSensor", sensor.Sensor, InfinitESPDevice)

SENSOR_TYPES = {
    "temperature": {"key": "temperature", "unit": "\u00b0F"},
    "humidity": {"key": "humidity", "unit": "%"},
    "outdoor_temperature": {"key": "outdoor_temperature", "unit": "\u00b0F"},
    "vacation_min_temp": {"key": "vacation_min_temp", "unit": "\u00b0F"},
    "vacation_max_temp": {"key": "vacation_max_temp", "unit": "\u00b0F"},
    "blower_rpm": {"key": "blower_rpm", "unit": "RPM"},
    "airflow_cfm": {"key": "airflow_cfm", "unit": "CFM"},
    "compressor_rpm": {"key": "compressor_rpm", "unit": "RPM"},
    "odu_demand": {"key": "odu_demand", "unit": "%"},
    "odu_stage": {"key": "odu_stage", "unit": ""},
    "odu_modulation": {"key": "odu_modulation", "unit": ""},
    "odu_setpoint": {"key": "odu_setpoint", "unit": "\u00b0F"},
    "odu_mode": {"key": "odu_operating_mode", "unit": ""},
    # ODU IEEE754 float32 values from register 061f (hypothesized meanings)
    "odu_float_1": {"key": "odu_float_1", "unit": "\u00b0F"},  # Superheat target (~7.5)
    "odu_float_2": {"key": "odu_float_2", "unit": "\u00b0F"},  # Actual superheat (~10)
    "odu_float_3": {"key": "odu_float_3", "unit": "\u00b0F"},  # Subcooling target? (~14)
    "odu_float_4": {"key": "odu_float_4", "unit": "\u00b0F"},  # Actual subcooling? (~12)
    "odu_float_5": {"key": "odu_float_5", "unit": "\u00b0F"},  # Discharge superheat? (-68 to +17)
    "odu_float_6": {"key": "odu_float_6", "unit": ""},         # Unknown (~0.039)
    # ODU register 0302 temperature measurements (int16 BE / 16)
    "odu_outdoor_temp": {"key": "odu_outdoor_temp", "unit": "\u00b0F"},       # Outdoor air temperature
    "odu_coil_temp": {"key": "odu_coil_temp", "unit": "\u00b0F"},             # Outdoor coil temperature
    "odu_suction_temp": {"key": "odu_suction_temp", "unit": "\u00b0F"},       # Suction line temperature
    "odu_liquid_temp": {"key": "odu_liquid_temp", "unit": "\u00b0F"},         # Liquid line / expansion temperature
    "odu_indoor_coil_temp": {"key": "odu_indoor_coil_temp", "unit": "\u00b0F"}, # Indoor coil temperature
    "odu_discharge_temp": {"key": "odu_discharge_temp", "unit": "\u00b0F"},   # Discharge / hot gas temperature
}

CONFIG_SCHEMA = sensor.sensor_schema(
    InfinitESPSensor,
    accuracy_decimals=0,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
        cv.Required(CONF_TYPE): cv.one_of(*SENSOR_TYPES, lower=True),
        cv.Optional(CONF_ZONE, default=1): cv.int_range(min=1, max=8),
    }
)


async def to_code(config):
    stype = config[CONF_TYPE]
    info = SENSOR_TYPES[stype]
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_unit_of_measurement(info["unit"]))
    await sensor.register_sensor(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_sensor_type(info["key"]))
    await register_infinitesp_device(var, config)
