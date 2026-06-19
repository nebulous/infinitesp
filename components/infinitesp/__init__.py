import esphome.codegen as cg
import esphome.config_validation as cv
import logging
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@nebulous"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["climate", "sensor", "select", "text_sensor", "binary_sensor", "cover"]
MULTI_CONF = True

CONF_INFINITESP_ID = "infinitesp_id"
CONF_STATUS_LIGHT_ID = "status_light_id"
CONF_STATUS_LED_PIN = "status_led_pin"

infinitesp_ns = cg.esphome_ns.namespace("infinitesp")
InfinitESPComponent = infinitesp_ns.class_("InfinitESPComponent", cg.Component, uart.UARTDevice)
InfinitESPEntity = infinitesp_ns.class_("InfinitESPEntity")

CONF_SAM_ADDRESS = "sam_address"
CONF_ADDRESS = "address"  # deprecated alias for sam_address
CONF_FLOW_CONTROL_PIN = "flow_control_pin"
CONF_ZONE_CONTROLLER_ADDRESS = "zone_controller_address"
CONF_TEMPERATURE_UNIT = "temperature_unit"

# ZC zone sensor reference configuration
CONF_ZC_ZONE_2 = "zc_zone_2"
CONF_ZC_ZONE_3 = "zc_zone_3"
CONF_ZC_ZONE_4 = "zc_zone_4"
CONF_TEMPERATURE_SENSOR = "temperature_sensor"
CONF_STALENESS_TIMEOUT = "staleness_timeout"
CONF_SENSOR_UNIT = "sensor_unit"

ZC_ZONE_SCHEMA = cv.Schema({
    cv.Optional(CONF_TEMPERATURE_SENSOR): cv.use_id("sensor"),
    cv.Optional(CONF_STALENESS_TIMEOUT, default=120): cv.positive_int,
    cv.Optional(CONF_SENSOR_UNIT): cv.one_of("C", "F"),
})

TEMP_UNIT_AUTO = "auto"
TEMP_UNIT_FAHRENHEIT = "F"
TEMP_UNIT_CELSIUS = "C"


def _validate_zc_config(config):
    """Warn on ZC zone sensor misconfiguration."""
    for zone_key in [CONF_ZC_ZONE_2, CONF_ZC_ZONE_3, CONF_ZC_ZONE_4]:
        if zone_key not in config:
            continue
        if config.get(CONF_ZONE_CONTROLLER_ADDRESS, 0) == 0:
            _LOGGER.warning("'%s' configured but zone_controller_address is 0; ZC emulation disabled", zone_key)
        # A sensor's native unit is independent of the bus; defaulting to the
        # system unit is a guess. It's caught at runtime by the 40-99°F band
        # check (rejected → falls back to zone-1 ambient), but warn so users
        # set sensor_unit explicitly and avoid mis-conversion in the first place.
        if CONF_TEMPERATURE_SENSOR in config[zone_key] and CONF_SENSOR_UNIT not in config[zone_key]:
            _LOGGER.warning(
                "'%s.temperature_sensor' has no 'sensor_unit'; defaulting to the system "
                "unit. A wrong guess is rejected at runtime (40-99°F band) and falls back "
                "to zone-1 ambient, but set 'sensor_unit: F' or 'sensor_unit: C' explicitly "
                "to avoid mis-conversion. Check the sensor's published value to pick correctly.",
                zone_key)
    return config


def _validate_addresses(config):
    """Handle address → sam_address deprecation and mutual exclusion."""
    if CONF_ADDRESS in config:
        if CONF_SAM_ADDRESS in config:
            raise cv.Invalid("Specify 'sam_address' or 'address', not both")
        _LOGGER.warning("'address' is deprecated, use 'sam_address' instead")
        config[CONF_SAM_ADDRESS] = config.pop(CONF_ADDRESS)
    config.setdefault(CONF_SAM_ADDRESS, 0x92)
    return config


def _validate_status_led(config):
    """Ensure status_light_id and status_led_pin are mutually exclusive."""
    if CONF_STATUS_LIGHT_ID in config and CONF_STATUS_LED_PIN in config:
        raise cv.Invalid("status_light_id and status_led_pin are mutually exclusive")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(InfinitESPComponent),
            cv.Optional(CONF_ADDRESS): cv.int_range(min=0, max=255),
            cv.Optional(CONF_SAM_ADDRESS): cv.int_range(min=0, max=255),
            # Status LED: reference an existing light entity (e.g. WS2812 RGB)
            cv.Optional(CONF_STATUS_LIGHT_ID): cv.use_id("light"),
            # Status LED: shorthand for a simple LED on a GPIO pin
            cv.Optional(CONF_STATUS_LED_PIN): pins.gpio_output_pin_schema,
            # RS485 transmit enable pin (DE/RE control)
            cv.Optional(CONF_FLOW_CONTROL_PIN): pins.gpio_output_pin_schema,
            # Zone controller emulation: set to 0x60 to emulate a SYSTXCC4ZC01
            cv.Optional(CONF_ZONE_CONTROLLER_ADDRESS, default=0): cv.int_range(min=0, max=255),
            # Temperature unit: auto (heuristic), F, or C
            cv.Optional(CONF_TEMPERATURE_UNIT, default=TEMP_UNIT_AUTO): cv.one_of(TEMP_UNIT_AUTO, TEMP_UNIT_FAHRENHEIT, TEMP_UNIT_CELSIUS, lower=True),
            # ZC zone temperature sensor references (requires zone_controller_address)
            cv.Optional(CONF_ZC_ZONE_2): ZC_ZONE_SCHEMA,
            cv.Optional(CONF_ZC_ZONE_3): ZC_ZONE_SCHEMA,
            cv.Optional(CONF_ZC_ZONE_4): ZC_ZONE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA).extend(uart.UART_DEVICE_SCHEMA),
    _validate_addresses,
    _validate_status_led,
    _validate_zc_config,
)

INFINITESP_DEVICE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_INFINITESP_ID): cv.use_id(InfinitESPComponent),
    }
)


async def register_infinitesp_entity(var, config):
    parent = await cg.get_variable(config[CONF_INFINITESP_ID])
    cg.add(parent.register_entity(var))
    cg.add(var.set_parent(parent))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_sam_address(config[CONF_SAM_ADDRESS]))

    if config[CONF_ZONE_CONTROLLER_ADDRESS] != 0:
        cg.add(var.set_zc_address(config[CONF_ZONE_CONTROLLER_ADDRESS]))

    # Wire up ZC zone temperature sensor references
    for zone_num, zone_key in [(2, CONF_ZC_ZONE_2), (3, CONF_ZC_ZONE_3), (4, CONF_ZC_ZONE_4)]:
        if zone_key in config:
            zone_cfg = config[zone_key]
            if CONF_TEMPERATURE_SENSOR in zone_cfg:
                sens = await cg.get_variable(zone_cfg[CONF_TEMPERATURE_SENSOR])
                cg.add(var.set_zc_temperature_sensor(zone_num, sens))
                # Explicit sensor_unit overrides the default (inherit from bus)
                if CONF_SENSOR_UNIT in zone_cfg:
                    is_f = zone_cfg[CONF_SENSOR_UNIT] == "F"
                    cg.add(var.set_zc_sensor_is_fahrenheit(zone_num, is_f))
            timeout = zone_cfg.get(CONF_STALENESS_TIMEOUT, 120)
            cg.add(var.set_zc_staleness_timeout(zone_num, timeout * 1000))

    temp_unit = config[CONF_TEMPERATURE_UNIT]
    if temp_unit == TEMP_UNIT_AUTO:
        cg.add(var.set_temperature_unit(cg.RawExpression("TemperatureUnit::AUTO")))
    elif temp_unit == TEMP_UNIT_FAHRENHEIT:
        cg.add(var.set_temperature_unit(cg.RawExpression("TemperatureUnit::FAHRENHEIT")))
    elif temp_unit == TEMP_UNIT_CELSIUS:
        cg.add(var.set_temperature_unit(cg.RawExpression("TemperatureUnit::CELSIUS")))

    if CONF_STATUS_LED_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_STATUS_LED_PIN])
        cg.add(var.set_status_led_pin(pin))
        cg.add_define("USE_INFINITESP_STATUS_LED_PIN")

    if CONF_FLOW_CONTROL_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_FLOW_CONTROL_PIN])
        cg.add(var.set_flow_control_pin(pin))
        cg.add_define("USE_INFINITESP_FLOW_CONTROL_PIN")

    if CONF_STATUS_LIGHT_ID in config:
        light_var = await cg.get_variable(config[CONF_STATUS_LIGHT_ID])
        cg.add(var.set_status_light(light_var))
        cg.add_define("USE_INFINITESP_STATUS_LIGHT")

    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
