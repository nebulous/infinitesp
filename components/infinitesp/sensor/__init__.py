import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE, STATE_CLASS_MEASUREMENT, DEVICE_CLASS_TEMPERATURE
from .. import InfinitESPDevice, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_device

CONF_ZONE = "zone"

InfinitESPSensor = infinitesp_ns.class_("InfinitESPSensor", sensor.Sensor, InfinitESPDevice)

SENSOR_TYPES = {
    "temperature": {"key": "temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "humidity": {"key": "humidity", "unit": "%"},
    "outdoor_temperature": {"key": "outdoor_temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "vacation_min_temp": {"key": "vacation_min_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "vacation_max_temp": {"key": "vacation_max_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "blower_rpm": {"key": "blower_rpm", "unit": "RPM"},
    "airflow_cfm": {"key": "airflow_cfm", "unit": "CFM"},
    "compressor_rpm": {"key": "compressor_rpm", "unit": "RPM"},
    "odu_demand": {"key": "odu_demand", "unit": "%"},
    "odu_stage": {"key": "odu_stage", "unit": ""},
    "odu_modulation": {"key": "odu_modulation", "unit": ""},
    "odu_setpoint": {"key": "odu_setpoint", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_mode": {"key": "odu_operating_mode", "unit": ""},
    # ODU IEEE754 float32 values from register 061f (hypothesized meanings)
    "odu_float_1": {"key": "odu_float_1", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},  # Superheat target (~7.5)
    "odu_float_2": {"key": "odu_float_2", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},  # Actual superheat (~10)
    "odu_float_3": {"key": "odu_float_3", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},  # Subcooling target? (~14)
    "odu_float_4": {"key": "odu_float_4", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},  # Actual subcooling? (~12)
    "odu_float_5": {"key": "odu_float_5", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},  # Discharge superheat? (-68 to +17)
    "odu_float_6": {"key": "odu_float_6", "unit": ""},         # Unknown (~0.039)
    # ODU register 0302 temperature measurements (int16 BE / 16)
    "odu_outdoor_temp": {"key": "odu_outdoor_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_coil_temp": {"key": "odu_coil_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_suction_temp": {"key": "odu_suction_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_liquid_temp": {"key": "odu_liquid_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_indoor_coil_temp": {"key": "odu_indoor_coil_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
    "odu_discharge_temp": {"key": "odu_discharge_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE},
}

CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.Required(CONF_TYPE): cv.one_of(*SENSOR_TYPES, lower=True)}).extend(
        sensor.sensor_schema(
            InfinitESPSensor,
            accuracy_decimals=1,
            state_class=STATE_CLASS_MEASUREMENT,
        ).extend(
            {
                cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(CONF_INFINITESP_ID),
                cv.Optional(CONF_ZONE, default=1): cv.int_range(min=1, max=8),
            }
        )
    ),
    lambda config: {**config, sensor.CONF_UNIT_OF_MEASUREMENT: SENSOR_TYPES[config[CONF_TYPE]]["unit"],
                    sensor.CONF_DEVICE_CLASS: SENSOR_TYPES[config[CONF_TYPE]].get("device_class", "")},
)


async def to_code(config):
    stype = config[CONF_TYPE]
    info = SENSOR_TYPES[stype]
    var = cg.new_Pvariable(config[CONF_ID])
    await sensor.register_sensor(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_sensor_type(info["key"]))
    await register_infinitesp_device(var, config)
