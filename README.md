# InfinitESP

ESPHome firmware for ESP32 that emulates a Carrier/Bryant/ICP System Access Module (SAM) on the ABCD RS485 bus, giving Home Assistant native control of Infinity/Evolution HVAC systems.

No cloud or Carrier API. Just a serial bus and a microcontroller.

> **Disclaimer:** This firmware was developed by reverse-engineering a proprietary protocol. Everything described below has been confirmed working in the author's system. Your mileage may vary with different equipment, firmware versions, or bus configurations.

## Design

InfinitESP speaks the Carrier ABCD bus protocol and registers as a SAM (address `0x92`). The thermostat sees a valid SAM on the bus and InfinitESP creates a climate device in Home Assistant to control each active zone:

| Entity Type | What You Get |
|---|---|
| **Climate** | Per-zone thermostat with heat/cool/auto/off modes, dual heat+cool setpoints, fan control, and preset support (home/away/sleep/wake + timed holds) |
| **Sensors** | Zone temperature, zone humidity, outdoor air temp, blower RPM, airflow CFM, compressor RPM, ODU demand/stage/modulation, superheat/subcooling targets & actuals, ODU temperatures (outdoor/coil/suction/liquid/discharge), vacation min/max temps |
| **Binary Sensors** | Bus online/offline status, compressor running, electric heat active |
| **Selects** | System mode (heat/cool/auto/off/emergency heat), per-zone fan speed (auto/low/med/high) |
| **Text Sensors** | Zone names, thermostat WiFi SSID/hostname/MAC, proxy server, dealer info, comfort profile dump |


## Suggested Hardware

Any ESP32 with an RS485 transceiver should work. ESP8266 may also work but has not been tested.
The author's setup uses the **[Waveshare ESP32-S3-Relay-6CH](https://amzn.to/4mX6tLp)** board.

[<img src="https://www.waveshare.com/w/upload/thumb/e/ee/ESP32-S3-Relay-6CH.jpg/1200px-ESP32-S3-Relay-6CH.jpg" width="400" />](https://amzn.to/4mX6tLp)

The Waveshare board was chosen as a reference design for its nice case, onboard RS485 interface, and its 6 relay outputs. The relays open the door to emulating other devices beyond the SAM — notably the NIM (Network Interface Module, SYSTXCCNIM01) or a Damper Control Module (SYSTXCC4ZC01), either of which could directly switch equipment via those relays.  The relay GPIOs are exposed in the example YAML config but are not part of the core SAM emulation. The firmware is hardware-agnostic — it just needs a `uart::UARTComponent`. The RS485 transceiver, ESP32 variant, and relay hardware are all irrelevant to the protocol engine.
Generic ESP32 dev boards with a separate RS485 transceiver module should also work.

> **Note:** The Waveshare board's RS485 auto-direction circuit has a time constant too short for reliable operation at 38400 baud. The circuit assumes the UART idle state (HIGH) means "stop transmitting," so runs of consecutive `1` bits cause it to stop driving the bus mid-byte. On an insufficiently-biased bus, the line voltage collapses and the receiver reads garbage. **However**, on a live Carrier ABCD bus, the existing equipment provides strong biasing (pull-up on A, pull-down on B) that holds the bus in a logic `1` state when the driver cuts out — masking the flaw. If you see TX corruption in testing or on an unusual bus configuration, you can potentially add external biasing resistors, bypass the onboard transceiver entirely with a [pico-HAT-compatible RS485 module](https://amzn.to/4eMssT1) which plugs right into the header, or if you're brave, desperate, or foolish modify the SMD components on the board.

### 📣 Calling all NIM and zone board owners:
If you have a Carrier NIM (SYSTXCCNIM01), Damper Control Module (SYSTXCC4ZC01), or any other interesting communicating hardware (remote room sensors, zone controllers, etc.) on your ABCD bus and would be willing to capture raw bus traffic, please open an issue. Emulating these devices requires protocol traces that can only come from real hardware — even a few minutes of logs would be incredibly valuable. You can share captures via a [GitHub discussion on the infinitude project](https://github.com/nebulous/infinitude/discussions) or by contacting the author directly.


## Wiring

```
Carrier ABCD Bus          ESP32 Board
─────────────────         ───────────────
  A (data+)  ──────────── RS485 A
  B (data-)  ──────────── RS485 B
  GND        ──────────── GND
```

The Waveshare board has an auto-direction RS485 transceiver — no DE/RE pin to manage. If using a generic transceiver (e.g. MAX485), tie DE and RE high for transmit or manage them with a GPIO.

## Bus Architecture

```
Carrier ABCD bus (RS485, 38400 baud, 8N1)
    │
    ├── 0x20  Thermostat (Infinity Touch / Evolution Connex)
    ├── 0x40  Indoor Unit / Furnace / Air Handler
    ├── 0x50  Outdoor Unit / Heat Pump
    └── 0x92  InfinitESP (SAM emulator)
```

InfinitESP registers as address `0x92` — the same address used by physical SAM modules and other SAM emulators. **Only one device should be at this address on the bus.** If you have a physical SAM installed, you'll need to disconnect or remove it. Likewise, do not run InfinitESP alongside infinitude (if its SAM emulation is enabled) or infinitive, which also occupy `0x92`.

## Quick Start

### 1. Prerequisites

- [ESPHome](https://esphome.io/) 2026.1 or newer
- An ESP32 with an RS485 transceiver connected to your Carrier ABCD bus

### 2. Clone and Configure

```bash
git clone https://github.com/nebulous/infinitesp.git
cd infinitesp
```

Create `secrets.yaml` with your WiFi credentials:
```yaml
ssid: "YourWiFiNetwork"
password: "YourWiFiPassword"
```

Review `infinitesp.yaml` and remove any zones you don't have. The default config defines all 8 zones; unused ones stay dormant in Home Assistant.

### 3. First Flash (USB)

```bash
# Hold BOOT button while plugging in USB, then release
esphome compile infinitesp.yaml
esphome upload infinitesp.yaml --device /dev/ttyACM0
```

### 4. Subsequent Updates (OTA)

```bash
esphome upload infinitesp.yaml --device infinitesp.local
```

### 5. Connect to the Bus

1. Power off the ESP32
2. Connect RS485 A and B to the Carrier ABCD bus
3. Power on the ESP32
4. Check logs for `Parsed frame` messages — this confirms bus traffic is being decoded:

```bash
esphome logs infinitesp.yaml --device infinitesp.local
```

### 6. Add to Home Assistant

The device appears automatically via ESPHome's native API integration. All entities populate within seconds of the bus coming online.

## UART Transports

InfinitESP supports three UART transport methods. Switch between them by changing `uart_id` on the `infinitesp:` block in your YAML:

### Hardware UART (default)

```yaml
uart:
  id: bus_uart
  tx_pin: GPIO17
  rx_pin: GPIO18
  baud_rate: 38400
  data_bits: 8
  parity: NONE
  stop_bits: 1
  rx_buffer_size: 4096

infinitesp:
  uart_id: bus_uart
```

### USB CDC ACM (virtual serial port via TinyUSB)

```yaml
tinyusb:

usb_cdc_acm:
  rx_buffer_size: 4096
  tx_buffer_size: 1024
  interfaces:
    - id: usb_bus_0

infinitesp:
  uart_id: usb_bus_0
```

Connect a USB cable from the ESP32 to a serial-to-TCP bridge (e.g. `socat`) on a host machine that's wired to the RS485 bus. Useful when the ESP32 isn't physically near the HVAC equipment.

### TCP Serial Bridge

```yaml
uart_tcp_client:
  id: tcp_bus_0
  host: "192.168.1.23"
  port: 23
  rx_buffer_size: 4096

infinitesp:
  uart_id: tcp_bus_0
```

The TCP and USB CDC transports are provided by [esphome-uart-link](https://github.com/nebulous/esphome-uart-link), a companion ESPHome component that implements UART-over-TCP and UART-over-USB transports. InfinitESP neither knows nor cares which transport backs the UART — they're all interchangeable.

## Configuration Reference

### Core Component

```yaml
infinitesp:
  id: infinitesp_hub
  uart_id: bus_uart
  address: 0x92  # SAM address. Use 0x93 for FakeSAM test mode.
  # Optional status LED (mutually exclusive):
  status_light_id: rgb_led    # Reference an existing ESPHome light (RGB supported)
  # status_led_pin: GPIO2      # Or just a GPIO pin for a simple LED
```

The status LED indicates system health:

| Status | RGB Color | Simple LED | Meaning |
|--------|-----------|------------|---------|
| Bus not online | Yellow blink (1s) | Slow blink (1s) | Bus not yet established |
| Bus online, no WiFi | Blue blink (500ms) | Fast blink (250ms) | Bus good, WiFi connecting |
| Both online | Solid green | Solid on | Everything working |

### Climate (per zone)

```yaml
climate:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Living Room"
    zone: 1    # 1–8, matches Carrier zone number
```

Supports `heat`, `cool`, `heat_cool` (auto), and `off` modes. Temperature range: 40–99°F in whole-degree steps. Fan modes: auto, low, medium, high. Presets: home, away, sleep, wake, with optional timed holds (1h/2h/4h/8h).

### Sensors

```yaml
sensor:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Living Room Temperature"
    type: temperature      # temperature, humidity, outdoor_temperature
    zone: 1                # required for temperature and humidity

  # Diagnostic sensors (no zone required)
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Blower RPM"
    type: blower_rpm

  # All diagnostic sensor types:
  #   blower_rpm, airflow_cfm, compressor_rpm
  #   odu_demand, odu_stage, odu_modulation, odu_setpoint, odu_mode
  #   odu_outdoor_temp, odu_coil_temp, odu_suction_temp,
  #   odu_liquid_temp, odu_indoor_coil_temp, odu_discharge_temp
  #   odu_float_1 (superheat target), odu_float_2 (superheat actual),
  #   odu_float_3 (subcooling target), odu_float_4 (subcooling actual),
  #   odu_float_5, odu_float_6
  #   vacation_min_temp, vacation_max_temp
```

### Binary Sensors

```yaml
binary_sensor:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Bus Status"
    type: bus_status       # bus_status, compressor_running, electric_heat
```

### Selects

```yaml
select:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "System Mode"
    type: system_mode      # system_mode (global)

  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Zone 1 Fan Mode"
    type: fan_mode         # fan_mode (per-zone, requires zone:)
    zone: 1
```

### Text Sensors

```yaml
text_sensor:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Zone 1 Name"
    type: zone_name        # zone_name (per-zone)
    zone: 1

  # Diagnostic text sensors (no zone required)
  # tstat_ssid, tstat_hostname, tstat_wifi_mac,
  # tstat_cloud_host, tstat_proxy_server,
  # tstat_dealer_name, tstat_dealer_brand, tstat_dealer_url,
  # comfort_profile
```

## How It Works

### Protocol

The Carrier Infinity system uses an RS485 serial bus with a framed protocol:

```
[DST] [DST_BUS] [SRC] [SRC_BUS] [LEN] [PID] [EXT] [FUNC] [PAYLOAD...] [CRC16_LO] [CRC16_HI]
  1B     1B       1B      1B      1B    1B    1B     1B     LEN bytes       2B
```

- CRC-16 Modbus (init `0x0000`, polynomial `0xA001`)
- Function codes: `0x0B` (read), `0x06` (reply), `0x0C` (write), `0x15` (exception)

### Lifecycle

InfinitESP does four things:

1. **Listens** for read requests addressed to the SAM and replies with emulated register data
2. **Accepts** write requests from the thermostat and stores register updates
3. **Polls** the thermostat every 3 seconds for live state (temperatures, mode, setpoints)
4. **Writes** setpoint/mode/fan changes from Home Assistant to the thermostat as bus writes

Mode and setpoint changes use a two-pronged approach: a 3B03 notification write (with change flags) primes the thermostat, then a direct data write delivers the actual values. Both are required — neither alone works.

### Passive Snooping

In addition to SAM-register traffic, InfinitESP passively observes inter-device traffic on the bus to extract diagnostic data:

- **Indoor unit** (0x40): blower RPM, airflow CFM, electric heat status
- **Outdoor unit** (0x50): compressor RPM, demand %, operating stage, modulation, superheat/subcooling, outdoor/coil/suction/liquid/discharge temperatures

No extra polling needed — the thermostat already queries these devices, and InfinitESP just listens in.

### WiFi Credential Caching

When using hardware UART transport, InfinitESP can automatically discover the thermostat's WiFi credentials from register 4608 and cache them to NVS flash. If the ESP32 subsequently boots without WiFi connectivity (e.g., changed router), it injects the cached credentials after a 15-second grace period. This lets the device self-provision from the bus — no manual WiFi config needed after initial setup.

This feature is not available with the TCP serial bridge transport (circular dependency: need WiFi to reach the TCP bridge, need the bus to get WiFi credentials).

## Compatible Systems

Tested with Carrier Infinity / Bryant Evolution / ICP systems using the ABCD RS485 bus with a System Access Module. Other systems using the same bus protocol may also work:

- **SAM modules**: SYSTXCCSAM01, SYSTXCCSAMC01
- **Thermostats**: Infinity Touch (SYSTXCCITC01), Evolution Connex (SYSTXBBECC01), legacy UID/UIZ controls with firmware 14+
- **HVAC equipment**: Infinity/Evolution furnace, air handler, or heat pump on the ABCD bus

## Troubleshooting

### Thermostat shows "SAM Communication Fault"

- Verify RS485 A/B wiring — try swapping if unsure (wrong polarity won't damage anything)
- Check that UART baud rate is exactly 38400
- Watch ESPHome logs for any `crc_fail` in the STATS line

### No climate entities in Home Assistant

- Check that the Bus Status binary sensor shows `on`
- Zones populate after the ESP sees register data from the thermostat — allow 10–15 seconds after first bus connection
- Verify your zone number (1–8) matches a zone that's active on your thermostat

### Setpoint changes don't take effect

- There's a 1–3 second propagation delay (poll cycle)
- If changes never take effect, check logs for CRC errors indicating RS485 signal quality issues
- Mode OFF may be rejected by some thermostat firmwares — this is a thermostat limitation, not a bug

### Can't flash over USB

- Hold the BOOT button while plugging in USB to enter download mode
- On ESP32-S3, GPIO45 and GPIO46 are strapping pins — the YAML includes `ignore_strapping_warning: true`

### Bus traffic looks garbled

- Check the STATS line in logs every 5 seconds
- Healthy: `crc_fail=0`, `stale=0`, `overflow_evts=0`, `reply_got ≈ reply_exp`
- `crc_fail > 0` → bus noise or bad wiring
- `overflow_evts > 0` → main loop not keeping up (reduce logging verbosity)
- `stale > 0` → bytes arriving with gaps > 100ms (transport issue)

## Project Structure

```
infinitesp.yaml              # Main device config
secrets.yaml                 # WiFi credentials (not tracked)
components/
├── infinitesp/              # ABCD bus protocol engine
│   ├── infinitesp.h/cpp     # Core: frame parsing, register mgmt, polling, CRC
│   ├── climate/             # HA climate entities (setpoint, mode, fan, preset)
│   ├── sensor/              # Temperature, humidity, RPM, ODU diagnostic sensors
│   ├── binary_sensor/       # Bus status, compressor, electric heat
│   ├── select/              # System mode, fan mode selects
│   └── text_sensor/         # Zone names, WiFi info, dealer info, profiles
└── uart_tcp_client/         # UART-over-TCP transport component
```

## Disclaimer

You are connecting a multi-thousand-dollar HVAC system to some random person's code on the internet. This project was built by reverse-engineering a proprietary serial protocol — there is no official documentation, no vendor endorsement, and no guarantee that anything here is correct. A bug, a misinterpreted register, or a corrupted write could in principle send commands to your equipment that it wasn't designed to handle.

If that happens, that's on you. You chose to wire an ESP32 into your furnace. Nobody involved in this project is coming to fix your HVAC, replace your compressor, or explain to your spouse why the house is 90°F in July.

If that bothers you, don't use this.

## Acknowledgments

- [infinitude](https://github.com/nebulous/infinitude) — Perl-based Infinity system controller and the origin of this project. Provides a full web interface for Infinity/Evolution systems, includes a SAM emulator, and was the primary protocol reference for InfinitESP.
- [infinitive](https://github.com/acd/infinitive) — Go-based SAM emulator, reference for the write protocol
- Carrier/ICP protocol details from the open-source HVAC community

## License

MIT
