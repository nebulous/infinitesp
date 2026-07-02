import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome import pins
from esphome.const import (
    CONF_ID,
    CONF_BAUD_RATE,
    CONF_TX_PIN,
    CONF_RX_PIN,
    CONF_RX_BUFFER_SIZE,
)

CODEOWNERS = ["@nebulous"]
DEPENDENCIES = ["esp32"]
MULTI_CONF = True
AUTO_LOAD = ["uart"]

# String constants (stop/data/parity live in esphome.components.const; define
# locally to avoid the cross-module import dance).
CONF_STOP_BITS = "stop_bits"
CONF_DATA_BITS = "data_bits"
CONF_PARITY = "parity"
CONF_RX_TIMEOUT = "rx_timeout"
CONF_RX_FULL_THRESHOLD = "rx_full_threshold"
CONF_FLUSH_TIMEOUT = "flush_timeout"
CONF_SUB_BITS = "sub_bits"
CONF_RMT_MEM_SYMBOLS = "rmt_mem_symbols"

uart_rmtx_ns = cg.esphome_ns.namespace("uart_rmtx")
RmtTxUARTComponent = uart_rmtx_ns.class_(
    "RmtTxUARTComponent", uart.IDFUARTComponent
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(RmtTxUARTComponent),
            cv.Required(CONF_BAUD_RATE): cv.int_range(min=1),
            # tx_pin is the RMT TX GPIO; it is NOT forwarded to the base UART,
            # which opens RX-only so the RMT channel owns the TX line.
            cv.Required(CONF_TX_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_RX_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_RX_BUFFER_SIZE, default=4096): cv.All(
                cv.validate_bytes, cv.uint16_t
            ),
            cv.Optional(CONF_STOP_BITS, default=1): cv.one_of(1, 2, int=True),
            cv.Optional(CONF_DATA_BITS, default=8): cv.int_range(min=5, max=8),
            cv.Optional(CONF_PARITY, default="NONE"): cv.enum(
                uart.UART_PARITY_OPTIONS, upper=True
            ),
            cv.Optional(CONF_RX_FULL_THRESHOLD): cv.All(
                cv.validate_bytes, cv.int_range(min=1, max=120)
            ),
            cv.Optional(CONF_RX_TIMEOUT, default=2): cv.All(
                cv.validate_bytes, cv.int_range(min=0, max=92)
            ),
            cv.Optional(CONF_FLUSH_TIMEOUT): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SUB_BITS, default=3): cv.int_range(min=1, max=16),
            cv.Optional(CONF_RMT_MEM_SYMBOLS, default=64): cv.int_range(min=48, max=512),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_baud_rate(config[CONF_BAUD_RATE]))
    cg.add(var.set_data_bits(config[CONF_DATA_BITS]))
    cg.add(var.set_stop_bits(config[CONF_STOP_BITS]))
    cg.add(var.set_parity(config[CONF_PARITY]))
    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))

    # RX pin goes to the base hardware UART.
    rx_pin = await cg.gpio_pin_expression(config[CONF_RX_PIN])
    cg.add(var.set_rx_pin(rx_pin))

    # TX pin goes to the RMT channel (NOT the base UART).
    tx_pin = await cg.gpio_pin_expression(config[CONF_TX_PIN])
    cg.add(var.set_rmt_tx_pin(tx_pin))

    if CONF_RX_FULL_THRESHOLD in config:
        cg.add(var.set_rx_full_threshold(config[CONF_RX_FULL_THRESHOLD]))
    else:
        # Replicate ESPHome's stock uart default: ~10ms of bytes, clamped to
        # [1,120]. The base IDFUARTComponent::load_settings() calls
        # uart_set_rx_full_threshold unconditionally; a 0 default (the base class
        # initial value) returns ESP_ERR_INVALID_ARG and marks the component
        # FAILED, silently disabling TX. 38400 8N1 -> 37.
        bytelength = config[CONF_DATA_BITS] + config[CONF_STOP_BITS] + 1
        if config[CONF_PARITY] != "NONE":
            bytelength += 1
        import math
        th = max(1, min(120, math.floor(config[CONF_BAUD_RATE] / (bytelength * 1000 / 10)) - 1))
        cg.add(var.set_rx_full_threshold(th))
    if CONF_RX_TIMEOUT in config:
        cg.add(var.set_rx_timeout(config[CONF_RX_TIMEOUT]))
    if CONF_FLUSH_TIMEOUT in config:
        cg.add(var.set_flush_timeout(config[CONF_FLUSH_TIMEOUT]))

    cg.add(var.set_sub_bits(config[CONF_SUB_BITS]))
    cg.add(var.set_rmt_mem_symbols(config[CONF_RMT_MEM_SYMBOLS]))
