import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE, STATE_CLASS_MEASUREMENT, DEVICE_CLASS_TEMPERATURE
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"

InfinitESPSensor = infinitesp_ns.class_("InfinitESPSensor", sensor.Sensor, InfinitESPEntity)

SENSOR_TYPES = {
    # SAM/thermostat sensors — device_class 0 (any), they gate on register_key
    "temperature": {"key": "temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "humidity": {"key": "humidity", "unit": "%", "bus_class": 0},
    "outdoor_temperature": {"key": "outdoor_temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "vacation_min_temp": {"key": "vacation_min_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "vacation_max_temp": {"key": "vacation_max_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    # IDU sensors — device class 4
    "blower_rpm": {"key": "blower_rpm", "unit": "RPM", "bus_class": 4},
    "airflow_cfm": {"key": "airflow_cfm", "unit": "CFM", "bus_class": 4},
    # ODU sensors — device class 5
    "compressor_rpm": {"key": "compressor_rpm", "unit": "RPM", "bus_class": 5},
    "odu_demand": {"key": "odu_demand", "unit": "%", "bus_class": 5},
    "odu_stage": {"key": "odu_stage", "unit": "", "bus_class": 5},
    "odu_modulation": {"key": "odu_modulation", "unit": "", "bus_class": 5},
    "odu_setpoint": {"key": "odu_setpoint", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_mode": {"key": "odu_operating_mode", "unit": "", "bus_class": 5},
    # ODU IEEE754 float32 values from register 061f
    "odu_float_1": {"key": "odu_float_1", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_float_2": {"key": "odu_float_2", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_float_3": {"key": "odu_float_3", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_float_4": {"key": "odu_float_4", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_float_5": {"key": "odu_float_5", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_float_6": {"key": "odu_float_6", "unit": "", "bus_class": 5},
    # ODU register 0302 temperature measurements
    "odu_outdoor_temp": {"key": "odu_outdoor_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_coil_temp": {"key": "odu_coil_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_suction_temp": {"key": "odu_suction_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_subcooling_degf_int": {"key": "odu_subcooling_degf_int", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_indoor_coil_temp": {"key": "odu_indoor_coil_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_discharge_temp": {"key": "odu_discharge_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    # IDU cycle counters (register 0310, 4-byte key-value entries) — device class 4
    "idu_low_heat_cycles": {"key": "idu_low_heat_cycles", "unit": "cycles", "bus_class": 4},
    "idu_high_heat_cycles": {"key": "idu_high_heat_cycles", "unit": "cycles", "bus_class": 4},
    "idu_med_heat_cycles": {"key": "idu_med_heat_cycles", "unit": "cycles", "bus_class": 4},
    "idu_blower_cycles": {"key": "idu_blower_cycles", "unit": "cycles", "bus_class": 4},
    "idu_poweron_cycles": {"key": "idu_poweron_cycles", "unit": "cycles", "bus_class": 4},
    # IDU runtime hours (register 0311, 4-byte key-value entries) — device class 4
    "idu_low_heat_hours": {"key": "idu_low_heat_hours", "unit": "h", "bus_class": 4},
    "idu_high_heat_hours": {"key": "idu_high_heat_hours", "unit": "h", "bus_class": 4},
    "idu_med_heat_hours": {"key": "idu_med_heat_hours", "unit": "h", "bus_class": 4},
    "idu_blower_hours": {"key": "idu_blower_hours", "unit": "h", "bus_class": 4},
    "idu_poweron_hours": {"key": "idu_poweron_hours", "unit": "h", "bus_class": 4},
    # ODU cycle counters (register 0310) — device class 5
    "odu_heat_cycles": {"key": "odu_heat_cycles", "unit": "cycles", "bus_class": 5},
    "odu_cool_cycles": {"key": "odu_cool_cycles", "unit": "cycles", "bus_class": 5},
    "odu_defrost_cycles": {"key": "odu_defrost_cycles", "unit": "cycles", "bus_class": 5},
    "odu_poweron_cycles": {"key": "odu_poweron_cycles", "unit": "cycles", "bus_class": 5},
    # ODU runtime hours (register 0311) — device class 5
    "odu_heat_hours": {"key": "odu_heat_hours", "unit": "h", "bus_class": 5},
    "odu_cool_hours": {"key": "odu_cool_hours", "unit": "h", "bus_class": 5},
    "odu_defrost_hours": {"key": "odu_defrost_hours", "unit": "h", "bus_class": 5},
    "odu_poweron_hours": {"key": "odu_poweron_hours", "unit": "h", "bus_class": 5},
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
    cg.add(var.set_device_class(info.get("bus_class", 0)))
    await register_infinitesp_entity(var, config)
