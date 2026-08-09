import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_ID, CONF_TYPE, CONF_DISABLED_BY_DEFAULT, STATE_CLASS_MEASUREMENT, STATE_CLASS_TOTAL_INCREASING, DEVICE_CLASS_TEMPERATURE, DEVICE_CLASS_VOLTAGE, DEVICE_CLASS_DURATION, CONF_ACCURACY_DECIMALS, CONF_STATE_CLASS
from .. import InfinitESPEntity, CONF_INFINITESP_ID, infinitesp_ns, register_infinitesp_entity

CONF_ZONE = "zone"

# raw_register (generic bus-field sensor) config keys and datatypes.
CONF_DEVICE_ADDRESS = "device_address"
CONF_REGISTER = "register"
CONF_OFFSET = "offset"
CONF_DATATYPE = "datatype"
CONF_SCALE = "scale"
CONF_VALUE_MIN = "value_min"
CONF_VALUE_MAX = "value_max"
CONF_LAMBDA = "lambda"
RAW_DATATYPES = ["uint8", "int8", "uint16_be", "int16_be", "uint32_be", "int32_be", "f32_be"]

InfinitESPSensor = infinitesp_ns.class_("InfinitESPSensor", sensor.Sensor, InfinitESPEntity)

# Monotonic counters (register 0310 cycles / 0311 hours): state_class=total_increasing
# so HA treats them as utility-meter-able totals (not stair-step measurement graphs),
# with integer precision.
_HOURS = {"unit": "h", "device_class": DEVICE_CLASS_DURATION,
          "state_class": STATE_CLASS_TOTAL_INCREASING, "accuracy_decimals": 0}
_CYCLES = {"unit": "cycles", "state_class": STATE_CLASS_TOTAL_INCREASING, "accuracy_decimals": 0}

SENSOR_TYPES = {
    # SAM/thermostat sensors — device_class 0 (any), they gate on register_key
    "temperature": {"key": "temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "humidity": {"key": "humidity", "unit": "%", "bus_class": 0},
    "outdoor_temperature": {"key": "outdoor_temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "vacation_min_temp": {"key": "vacation_min_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    "vacation_max_temp": {"key": "vacation_max_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 0},
    # IDU sensors — device class 4
    "blower_rpm": {"key": "blower_rpm", "unit": "RPM", "bus_class": 4},
    "airflow_cfm": {"key": "airflow_cfm", "unit": "ft³/min", "bus_class": 4},
    # ODU sensors — device class 5
    # bare = actual (measured) RPM [2..3] (the original `compressor_rpm` read
    # [0..1] = target; re-pointed to actual). target_compressor_rpm [0..1] is
    # additive. Infinitude OutdoorUnit.pm 0604: target_rpm / current_rpm.
    "compressor_rpm": {"key": "compressor_rpm", "unit": "RPM", "bus_class": 5},
    "target_compressor_rpm": {"key": "target_compressor_rpm", "unit": "RPM", "bus_class": 5},
    "compressor_frequency": {"key": "compressor_frequency", "unit": "Hz", "bus_class": 5},
    # ODU expansion valve position from register 0608 byte [2] (0-100 percent).
    # Ramps over 10-15s on cycle transitions; reads 0 (off) or 100 (running) otherwise.
    "odu_expansion_valve": {"key": "odu_expansion_valve", "unit": "%", "bus_class": 5},
    "odu_commanded_stage": {"key": "odu_commanded_stage", "unit": "", "bus_class": 5},
    "odu_stage": {"key": "odu_stage", "unit": "", "bus_class": 5},
    "odu_mode": {"key": "odu_operating_mode", "unit": "", "bus_class": 5},
    # ODU line voltage from register 0304 byte 7 (whole volts, state-independent)
    "odu_line_voltage": {"key": "odu_line_voltage", "unit": "V", "device_class": DEVICE_CLASS_VOLTAGE, "bus_class": 5},
    # ODU IEEE754 float32 values from register 061f. idx 1..5 are DELTAS
    # (superheat/subcooling/control \u0394T) published in NATIVE \u00b0F with NO
    # device_class: HA's temperature conversion applies a +32 offset that corrupts
    # deltas (3\u00b0F -> 1.67\u00b0C -> displayed as 35\u00b0F). idx 6 is dimensionless.
    "odu_float_1": {"key": "odu_float_1", "unit": "\u00b0F", "bus_class": 5},
    "odu_float_2": {"key": "odu_float_2", "unit": "\u00b0F", "bus_class": 5},
    "odu_float_3": {"key": "odu_float_3", "unit": "\u00b0F", "bus_class": 5},
    "odu_float_4": {"key": "odu_float_4", "unit": "\u00b0F", "bus_class": 5},
    "odu_float_5": {"key": "odu_float_5", "unit": "\u00b0F", "bus_class": 5},
    "odu_float_6": {"key": "odu_float_6", "unit": "", "bus_class": 5},
    # ODU register 0302 temperature measurements
    "odu_outdoor_temp": {"key": "odu_outdoor_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_coil_temp": {"key": "odu_coil_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_suction_temp": {"key": "odu_suction_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    # \u0394T (delta): published native \u00b0F, no device_class (HA's temp conversion
    # adds +32, corrupting deltas). Matches the thermostat's \u00b0F superheat display.
    "odu_suction_superheat": {"key": "odu_suction_superheat", "unit": "\u00b0F", "bus_class": 5},
    "odu_indoor_ambient": {"key": "odu_indoor_ambient", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    "odu_discharge_temp": {"key": "odu_discharge_temp", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 5},
    # ZC register 0302 (device class 6 = 0x60>>4). 24-byte TLV [tag,id,hi,lo],
    # °F = uint16_BE / 16. zone N -> id N; id 0x14 = LAT, id 0x1C = HPT.
    # LAT/HPT exist only on zone boards with those thermistor ports wired, so
    # they default to disabled (enable in HA if your board reports them).
    "zc_zone_temperature": {"key": "zc_zone_temperature", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 6},
    "zc_lat": {"key": "zc_lat", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 6, "disabled_by_default": True},
    "zc_hpt": {"key": "zc_hpt", "unit": "\u00b0C", "device_class": DEVICE_CLASS_TEMPERATURE, "bus_class": 6, "disabled_by_default": True},
    # IDU cycle counters (register 0310, 4-byte key-value entries) — device class 4
    "idu_low_heat_cycles": {"key": "idu_low_heat_cycles", "bus_class": 4, **_CYCLES},
    "idu_high_heat_cycles": {"key": "idu_high_heat_cycles", "bus_class": 4, **_CYCLES},
    "idu_med_heat_cycles": {"key": "idu_med_heat_cycles", "bus_class": 4, **_CYCLES},
    "idu_blower_cycles": {"key": "idu_blower_cycles", "bus_class": 4, **_CYCLES},
    "idu_poweron_cycles": {"key": "idu_poweron_cycles", "bus_class": 4, **_CYCLES},
    # IDU runtime hours (register 0311, 4-byte key-value entries) — device class 4
    "idu_low_heat_hours": {"key": "idu_low_heat_hours", "bus_class": 4, **_HOURS},
    "idu_high_heat_hours": {"key": "idu_high_heat_hours", "bus_class": 4, **_HOURS},
    "idu_med_heat_hours": {"key": "idu_med_heat_hours", "bus_class": 4, **_HOURS},
    "idu_blower_hours": {"key": "idu_blower_hours", "bus_class": 4, **_HOURS},
    "idu_poweron_hours": {"key": "idu_poweron_hours", "bus_class": 4, **_HOURS},
    # ODU cycle counters (register 0310) — device class 5
    "odu_heat_cycles": {"key": "odu_heat_cycles", "bus_class": 5, **_CYCLES},
    "odu_cool_cycles": {"key": "odu_cool_cycles", "bus_class": 5, **_CYCLES},
    "odu_defrost_cycles": {"key": "odu_defrost_cycles", "bus_class": 5, **_CYCLES},
    "odu_poweron_cycles": {"key": "odu_poweron_cycles", "bus_class": 5, **_CYCLES},
    # ODU runtime hours (register 0311) — device class 5
    "odu_heat_hours": {"key": "odu_heat_hours", "bus_class": 5, **_HOURS},
    "odu_cool_hours": {"key": "odu_cool_hours", "bus_class": 5, **_HOURS},
    "odu_defrost_hours": {"key": "odu_defrost_hours", "bus_class": 5, **_HOURS},
    "odu_poweron_hours": {"key": "odu_poweron_hours", "bus_class": 5, **_HOURS},
    # Generic bus-field sensor: user specifies device/register/offset/datatype/scale
    # (decode mode) or a full-frame lambda. bus_class is derived from device_address.
    "raw_register": {"key": "raw_register", "bus_class": 0},
}

def _apply_sensor_type(config):
    """Inject unit/device_class/accuracy_decimals/state_class from SENSOR_TYPES,
    and force disabled_by_default for sensor types that opt into it (e.g.
    zc_lat/zc_hpt). raw_register supplies its own from yaml, so it is skipped."""
    if config[CONF_TYPE] == "raw_register":
        return config
    info = SENSOR_TYPES[config[CONF_TYPE]]
    config[sensor.CONF_UNIT_OF_MEASUREMENT] = info["unit"]
    config[sensor.CONF_DEVICE_CLASS] = info.get("device_class", "")
    config[CONF_ACCURACY_DECIMALS] = info.get("accuracy_decimals", 1)
    config[CONF_STATE_CLASS] = sensor.validate_state_class(info.get("state_class", STATE_CLASS_MEASUREMENT))
    if info.get("disabled_by_default"):
        config[CONF_DISABLED_BY_DEFAULT] = True
    return config


def _validate_raw_register(config):
    """raw_register needs device_address + register, and either datatype (decode
    mode) or lambda (full-frame decode), mutually exclusive."""
    if config[CONF_TYPE] != "raw_register":
        return config
    if CONF_DEVICE_ADDRESS not in config:
        raise cv.Invalid("raw_register requires 'device_address'")
    if CONF_REGISTER not in config:
        raise cv.Invalid("raw_register requires 'register'")
    has_lambda = CONF_LAMBDA in config
    has_datatype = CONF_DATATYPE in config
    if has_lambda and has_datatype:
        raise cv.Invalid("raw_register: 'lambda' and 'datatype' are mutually exclusive")
    if not has_lambda and not has_datatype:
        raise cv.Invalid("raw_register: requires either 'datatype' or 'lambda'")
    if (CONF_VALUE_MIN in config) != (CONF_VALUE_MAX in config):
        raise cv.Invalid("raw_register: 'value_min' and 'value_max' must be set together")
    return config


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
                # raw_register extras (ignored by other sensor types):
                cv.Optional(CONF_DEVICE_ADDRESS): cv.hex_uint8_t,
                cv.Optional(CONF_REGISTER): cv.hex_uint16_t,
                cv.Optional(CONF_OFFSET, default=0): cv.int_range(min=0, max=250),
                cv.Optional(CONF_DATATYPE): cv.one_of(*RAW_DATATYPES, lower=True),
                cv.Optional(CONF_SCALE, default=1.0): cv.float_,
                cv.Optional(CONF_VALUE_MIN): cv.float_,
                cv.Optional(CONF_VALUE_MAX): cv.float_,
                cv.Optional(CONF_LAMBDA): cv.lambda_,
            }
        )
    ),
    _validate_raw_register,
    _apply_sensor_type,
)


async def to_code(config):
    stype = config[CONF_TYPE]
    info = SENSOR_TYPES[stype]
    var = cg.new_Pvariable(config[CONF_ID])
    await sensor.register_sensor(var, config)
    cg.add(var.set_zone(config[CONF_ZONE]))
    cg.add(var.set_sensor_type(info["key"]))
    if stype == "raw_register":
        dev = config[CONF_DEVICE_ADDRESS]
        cg.add(var.set_bus_class(dev >> 4))
        cg.add(var.set_raw_target(dev, config[CONF_REGISTER]))
        cg.add(var.set_raw_offset(config[CONF_OFFSET]))
        cg.add(var.set_raw_scale(config[CONF_SCALE]))
        if CONF_DATATYPE in config:
            cg.add(var.set_raw_datatype(config[CONF_DATATYPE]))
        if CONF_VALUE_MIN in config:
            cg.add(var.set_raw_value_range(config[CONF_VALUE_MIN], config[CONF_VALUE_MAX]))
        if CONF_LAMBDA in config:
            lam = await cg.process_lambda(
                config[CONF_LAMBDA],
                [(cg.std_vector.template(cg.uint8).operator("const").operator("ref"), "data")],
                return_type=cg.float_,
            )
            cg.add(var.set_raw_lambda(lam))
    else:
        cg.add(var.set_bus_class(info.get("bus_class", 0)))
    await register_infinitesp_entity(var, config)
