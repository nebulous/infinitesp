# InfinitESP

ESPHome firmware for ESP32 that emulates Carrier/Bryant/ICP devices on the "ABCD" RS485 bus. System Access Module(SAM) emulation gives Home Assistant native control HVAC systems and Damper Control Module emulation gives the thermostat access to any physical hardware that esphome can actuate.

No cloud or Carrier API required, just a serial bus and a microcontroller.

> **Disclaimer:** This firmware was developed by reverse-engineering a proprietary protocol. Everything described below has been confirmed working in the author's system. Your mileage may vary with different equipment, firmware versions, or bus configurations.

## Design

InfinitESP speaks the Carrier ABCD bus protocol and registers as a SAM (address `0x92`). The thermostat sees a valid SAM on the bus and InfinitESP creates a climate device in Home Assistant to control each active zone:

| Entity Type | What You Get |
|---|---|
| **Climate** | Per-zone thermostat with heat/cool/auto/off modes, dual heat+cool setpoints, fan control, and preset support (per schedule, home, away, sleep, wake, hold timer, hold indefinitely) |
| **Covers** | Per-zone damper position (0–100%) from the zone controller(either an emulated or a real physical zone controller) |
| **Sensors** | Zone temperature, zone humidity, outdoor air temp, blower RPM, airflow CFM, compressor RPM, ODU demand/stage/modulation, expansion valve position, superheat/subcooling targets & actuals, ODU temperatures (outdoor/coil/suction/discharge) plus suction superheat, vacation min/max temps |
| **Binary Sensors** | Bus online/offline status, compressor running, electric heat active |
| **Selects** | System mode (heat/cool/auto/off/emergency heat), per-zone fan speed (auto/low/med/high) |
| **Text Sensors** | Zone names, hold state, thermostat WiFi SSID/hostname/MAC, proxy server, dealer info, comfort profile dump |


## Hardware Options

Any ESP32 with an RS485 transceiver works. ESP8266 is untested.
The author's setup uses the **[Waveshare ESP32-S3-Relay-6CH](https://amzn.to/4mX6tLp)** board.

[<img src="https://www.waveshare.com/w/upload/thumb/e/ee/ESP32-S3-Relay-6CH.jpg/1200px-ESP32-S3-Relay-6CH.jpg" width="400" />](https://amzn.to/4mX6tLp)

The Waveshare board was chosen as a reference design for its case, onboard RS485 interface, and 6 relay outputs. The relays open the door to emulating other devices beyond the SAM, notably the NIM (Network Interface Module, SYSTXCCNIM01) the Damper Control Module (SYSTXCC4ZC01) is already part of InfinitESP. Damper actuation works through a trigger callback: each zone's cover fires `on_change` whenever the thermostat commands a new damper position, handing the new position to whatever relay or motor driver your hardware uses (see the [Covers](#covers) examples). When emulating the zone controller, each zone also takes a temperature sensor so the thermostat sees a room reading, falling back to the zone 1 temperature when no sensor is configured (see [Zone Controller](#zone-controller-optional)). The relay GPIOs are exposed in the example YAML config but are not part of the core SAM emulation. The firmware is hardware-agnostic. It just needs a `uart::UARTComponent`. The RS485 transceiver, ESP32 variant, and relay hardware are all irrelevant to the protocol engine.
Generic ESP32 dev boards with a separate RS485 transceiver module should also work.

> **Note:** The Waveshare board's RS485 auto-direction circuit has a time constant too short for reliable operation at 38400 baud. The circuit assumes the UART idle state (HIGH) means "stop transmitting," so runs of consecutive `1` bits cause it to stop driving the bus mid-byte. On an insufficiently-biased bus, the line voltage collapses and the receiver reads garbage.
>
> InfinitESP ships with an experimental `uart_rmtx` UART component that drives the RS485 interface through the ESP32's Remote Control Transceiver (RMT) peripheral instead of the hardware UART. It emits a sub-bit line code that keeps the transceiver's auto-direction DE line primed during long constant-level runs, avoiding the dropout with no hardware modification. Bench-validated against a single physical SAM (100% reply rate on 160 short-frame queries, zero CRC errors). See its [README](components/uart_rmtx/README.md) for the mechanism, configuration, and current validation status.
>
> A live Carrier ABCD bus may tolerate the flaw without any fix, depending on bus topology. If `uart_rmtx` does not resolve it, any other fix requires adding or modifying hardware (for example, bypassing the onboard transceiver with a separate RS485 module).

### 📣 Calling all users:
If you have hardware such as a Carrier NIM (SYSTXCCNIM01), ~Damper Control Module (SYSTXCC4ZC01)~(implemented, but new logs are always good validation), or any other interesting communicating hardware (remote room sensors, zone controllers, etc.) on your ABCD bus and would be willing to capture raw bus traffic, please open an issue. Understanding and emulating these devices requires protocol traces that can only come from real hardware. Even a few minutes of logs would be valuable. See [Reporting Issues](#reporting-issues) for how to collect them. A `REPORT?` snapshot helps too, but full protocol logs are best for emulation work since they show the timing and framing that static register dumps miss. Share captures via a [GitHub discussion on the infinitude project](https://github.com/nebulous/infinitude/discussions) or by contacting the author directly.


## Wiring

```
Carrier ABCD Bus          ESP32 Board
─────────────────         ───────────────
  A (data+)  ──────────── RS485 A
  B (data-)  ──────────── RS485 B
  GND        ──────────── GND
```

The Waveshare board has an auto-direction RS485 transceiver. No DE/RE pin to manage. If using a generic transceiver (e.g. MAX485), tie DE and RE high for transmit or manage them with a GPIO.

## Bus Architecture

```
Carrier ABCD bus (RS485, 38400 baud, 8N1)
    │
    ├── 0x20  Thermostat (Infinity Touch / Evolution Connex)
    ├── 0x40  Indoor Unit / Furnace / Air Handler
    ├── 0x50  Outdoor Unit / Heat Pump
    ├── 0x60  Zone Controller (Damper Control Module, SYSTXCC4ZC01)
    ├── 0x61  Zone Controller (second board, zones 5-8 on large systems)
    └── 0x92  InfinitESP (SAM emulator)
```

InfinitESP registers as address `0x92`, the same address used by physical SAM modules and other SAM emulators. **Only one device can be at this address on the bus.** If you have a physical SAM installed, disconnect or remove it. Likewise, do not run InfinitESP alongside infinitude (if its SAM emulation is enabled) or infinitive, which also occupy `0x92`. The same single-occupant rule applies to the zone-controller address `0x60`: if you already have a physical Damper Control Module on the bus, do **not** also enable `zone_controller_address: 0x60`. Either let InfinitESP passively monitor the real one, or emulate it after removing the hardware.

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
4. Check logs for `Parsed frame` messages to confirm bus traffic is being decoded:

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

The TCP and USB CDC transports are provided by [esphome-uart-link](https://github.com/nebulous/esphome-uart-link), a companion ESPHome component that implements UART-over-TCP and UART-over-USB transports. InfinitESP neither knows nor cares which transport backs the UART. They're all interchangeable.

### Raw Bus Tap (Network)

Expose the raw ABCD serial bus over TCP so external tools (Infinitude, Wireshark dissector, custom scripts) can observe the bus in real time. This is useful for protocol analysis and for running Infinitude alongside InfinitESP.

```yaml
# Add these alongside the normal infinitesp config.
# IMPORTANT: infinitesp must read from the bridge, not the hardware UART.
# Change uart_id on the infinitesp: block from bus_uart to bus_bridge.

uart_tcp_server:
  - id: raw_bus_tap
    port: 4242
    max_clients: 2
    client_mode: fanout
    idle_timeout: 30s

uart_bridge:
  id: bus_bridge
  uarts:
    - bus_uart                                           # hardware RS485 (bidirectional)
    - uart: raw_bus_tap
      flow: from_bridge      # monitor-only; use 'both' to allow TCP client writes
```

Connect with `nc infinitesp.local 4242` to see raw hex traffic. The `from_bridge` flow direction means TCP clients can only observe, not inject bytes onto the bus. This is the safer default. Change to `both` for bidirectional access (e.g., running Infinitude through the tap).

> **Warning:** When using `flow: both`, TCP clients share the bus with InfinitESP. The ABCD bus has no arbitration. Simultaneous transmits will collide. Only use bidirectional mode if your tool understands the protocol timing.

## Configuration Reference

### Core Component

```yaml
infinitesp:
  id: infinitesp_hub
  uart_id: bus_uart
  sam_address: 0x92  # SAM address. 0x93 = FakeSAM test mode, 0 = disabled (passive monitor)
  zone_controller_address: 0x60  # Zone Controller address. 0 = no emulation (passive monitor of a real ZC if present)
  # Optional: temperature unit detection (default: auto)
  # temperature_unit: auto     # read from bus, or force F / C
  # Optional status LED (mutually exclusive):
  status_light_id: rgb_led    # Reference an existing ESPHome light (RGB supported)
  # status_led_pin: GPIO2      # Or just a GPIO pin for a simple LED
```

#### Zone Controller (optional)

Carrier zoned systems put a Damper Control Module (SYSTXCC4ZC01) on the bus at address `0x60`. InfinitESP can either **emulate** one or **passively monitor** a real one:

- **`zone_controller_address: 0x60`**: InfinitESP emulates the zone controller. The thermostat talks to InfinitESP as if it were the hardware. This lets you inject temperatures from external sensors (below). Do **not** enable this if a physical zone controller is already on the bus; the two would collide at `0x60`.

> **Warning — emulation requires real dampers, or it halts conditioning.** When the thermostat commissions a zone it runs an automated duct checkout: it commands different damper positions and blower speeds, then reads the indoor unit's fan telemetry to build a damper-position-to-airflow map. That map only converges if closing a damper actually raises duct static pressure, which needs real physical dampers and ductwork. An emulated zone controller reports damper position on the bus but produces no airflow change **unless** you have wired real dampers to the cover's [`on_change`](#on_change-trigger-on-commanded-moves) trigger to actually open and close them. With no dampers on the trigger, the checkout never validates. It runs both at install and as a recurring daily checkout, and **it stops all heating and cooling while it runs, potentially indefinitely until resolved**. This is a property of the emulation, not a bug InfinitESP can fix. Emulate a zone controller only on a system with real dampers and ductwork installed (and wire them to `on_change` so they actually move). To add a temperature sensor to an existing zoned system, leave `zone_controller_address: 0` and let InfinitESP passively monitor the real hardware.
- **`zone_controller_address: 0`** (default): no emulation. InfinitESP passively snoops the thermostat↔zone-controller traffic and still reports damper positions and per-zone conditioning state from the real hardware. This is the mode to use if you already have a physical Damper Control Module installed.

In either case the damper `cover` entities (see [Covers](#covers)) and per-zone climate heating/cooling action reflect the real damper state.

**Injecting external zone temperature sensors (emulation only).**
When emulating the zone controller, the thermostat expects each zone to report a temperature. InfinitESP can source these from any ESPHome sensor (a local Dallas 1-Wire or DHT/BME wired to the ESP32, or a Home Assistant entity via the `homeassistant` platform) instead of letting the thermostat read its own remote sensors. Configure one block per zone (zones 2–8; zone 1 is reported by the thermostat itself):

Systems with more than four zones use a second controller at `0x61`. InfinitESP emulates both the primary (`0x60`, zones 1–4) and secondary (`0x61`, zones 5–8) when `zone_controller_address` is set. Zone 5 maps to local zone 1 on the secondary controller, zone 6 to local 2, and so on. A thermostat commissioned for only four zones never polls `0x61`, so the secondary stays inert unless you wire sensors into `zc_zone_5` through `zc_zone_8`.

```yaml
# Option A: local Dallas 1-Wire sensor wired to the ESP32
dallas:
  - pin: GPIO4

sensor:
  - platform: dallas
    address: 0x1c000003ebee    # unique ROM address; run without this to log discovered addresses
    id: upstairs_temp
    internal: true

  # Option B: a temperature entity already in Home Assistant
  # - platform: homeassistant
  #   id: upstairs_temp
  #   entity_id: sensor.upstairs_temperature
  #   internal: true

infinitesp:
  zone_controller_address: 0x60
  zc_zone_2:
    temperature_sensor: upstairs_temp
    staleness_timeout: 120   # seconds; fall back to bus/thermostat value if no update
    sensor_unit: F           # REQUIRED. "C" or "F": the unit your sensor publishes in
  zc_zone_3:
    temperature_sensor: ...
  zc_zone_4:
    temperature_sensor: ...
  # Zones 5-8 live on a second emulated controller at 0x61 (only if the
  # thermostat is commissioned for more than four zones).
  # zc_zone_5:
  #   temperature_sensor: office_temp
  #   sensor_unit: F
```

Only one `sensor:` block is needed per zone. InfinitESP reads whichever `id` you wire in. `internal: true` keeps the raw sensor out of Home Assistant since its value surfaces through the zone controller emulation. If `temperature_sensor` is omitted, InfinitESP reports whatever the bus last reported for that zone. `staleness_timeout` (default 120s) controls how long to keep using the external value before falling back.

**Set `sensor_unit` explicitly for every zone.** It declares the unit your sensor *publishes*, not the thermostat's display setting. For example, `F` for a sensor that emits °F, `C` for one that emits °C. The two are unrelated: flipping the thermostat between °F/°C display does not change what your sensor publishes. ESPHome emits a config warning for any zone where `sensor_unit` is missing. The default is the system unit; a wrong guess causes silent mis-conversion (see below).

**Why it matters / sanity check.** InfinitESP rejects any injected reading that converts to values outside of the **40-99 °F** band (the indoor range the thermostat itself uses for setpoints) and falls back to the primary zone-1 ambient value until a plausible reading returns. A wrong `sensor_unit` always lands outside this band. For example, a °F sensor treated as °C reports a 70 °F room as ~160 °F, so the zone reads zone-1 ambient instead of garbage. The range check is a safety net, not a correctness test: **after configuring, check the zone temperatures on your thermostat and confirm they match the room.** A reading that silently fell back to zone-1 because of a mis-set unit looks "fine" (a real temperature, just not *that* zone's), so visual confirmation is the only reliable validation.

**LAT/HPT thermistor ports (emulation only).** The zone board also has leaving-air-temperature (LAT) and HPT thermistor ports, reported as TLV entries in the same register (ids `0x14` and `0x1C`). `zc_lat` and `zc_hpt` feed external sensors into those ports. Unlike zone temperatures, supply-air temp has no sane ambient fallback: when the fed sensor goes stale (past `staleness_timeout`), the entry reverts to not-installed so the thermostat stops seeing it rather than reading a bogus value. These sensors are disabled by default in Home Assistant; enable them if your board reports them.

This sensor-injection feature requires emulation. With a passive (physical) zone controller, the real hardware owns temperature reporting and these blocks have no effect.

#### Temperature Unit Detection

The Carrier ABCD bus encodes temperatures differently depending on the thermostat's display unit setting (°F or °C). InfinitESP reads the active unit from the bus and applies it automatically.

The unit flag lives at data offset 1 of every table-0x3B register the thermostat serves (state, zones, accessories, dealer): `0x00` = English/°F, `0x01` = Metric/°C. In `auto` mode InfinitESP reads that flag:

- When emulating the SAM, the thermostat pushes its dealer register (3B06) to InfinitESP on every poll cycle. InfinitESP reads the flag from it.
- When not emulating the SAM, InfinitESP polls the thermostat's accessories register (3B05) and reads the flag from the reply.

Until the first authoritative read lands (a few seconds after boot), InfinitESP falls back to a heuristic: any active zone temperature byte ≤ 50 means °C. No plausible HVAC zone exceeds 50°C (122°F).

The `temperature_unit` option forces a unit instead of reading it:

| Value | Behavior |
|-------|----------|
| `auto` (default) | Read the unit from the bus (3B06 when emulating the SAM, else a polled 3B05), with the zone-temperature heuristic as a boot-time fallback. |
| `F` | Force Fahrenheit. |
| `C` | Force Celsius. |

Most users never change this from `auto`. The explicit options exist for edge cases or debugging.

All temperature sensors publish in °C with `device_class: temperature`, so Home Assistant converts to the user's preferred display unit.

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

Supports `heat`, `cool`, `heat_cool` (auto), and `off` modes. Temperature range: 40-99°F in whole-degree steps. Fan modes: auto, low, medium, high. Presets: per schedule, home, away, sleep, wake, hold timer, hold indefinitely.

Each climate entity's `action` (heating/cooling/idle) is gated on its zone's damper state when a zone controller is present (emulated or physical), so on a zoned system only the zones actually receiving conditioned air report `heating`/`cooling`.

### Covers

```yaml
cover:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Living Room Damper"
    zone: 1    # 1–8, matches Carrier zone number
    device_class: damper
```

Reports each zone's commanded damper position (0–100%) from register `0308` (mirrored to `0319`). The thermostat uses 16 steps (`0x00`–`0x0F`); cover position = `step / 15.0`. Works whether you emulate the zone controller or passively monitor a physical one. Without any zone controller on the bus, these covers never publish (they report `unknown`).

The cover is a position reporter, not a bus actuator: **it never writes the ABCD bus**. Its job is to expose damper position to Home Assistant and to fire a trigger when that position is commanded to change.

#### on_change (trigger on commanded moves)

Add an `on_change` trigger to run automations whenever the cover's reported position doesn't match the commanded position (from the thermostat bus or from Home Assistant). It fires once per real step change (periodic bus re-asserts and the `0308`/`0319` mirror are de-duped, comparisons are in the protocol's 16-step space so re-commanding the current step is a no-op) and passes the new position as `pos` (a float `0.0`–`1.0`):

```yaml
cover:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Living Room Damper"
    zone: 1
    device_class: damper
    on_change:
      then:
        - logger.log:
            format: "Zone 1 damper -> %.0f%%"
            args: [ 'pos * 100.0' ]
```

Because the cover never writes the bus, what the trigger's actions do with `pos` is entirely up to you. `on_change` is a standard ESPHome trigger, so it takes any standard ESPHome action (`logger.log`, `switch.turn_on`, `homeassistant.event`, etc.). For example, to drive a binary damper relay (ON when the damper is commanded open at any step, OFF when closed), use `switch.control` with a templated boolean:

```yaml
cover:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Living Room Damper"
    zone: 1
    device_class: damper
    on_change:
      then:
        - switch.control:
            id: living_room_damper_relay
            state: !lambda 'return pos > 0.0f;'
```

`pos * 15.0` gives the protocol's 16 damper steps if you need proportional control; you handle any relay timing in the lambda. Home Assistant-set positions are transient: the cover acknowledges the command immediately, but the next bus-commanded change overwrites the reported position with what the thermostat actually commands. InfinitESP does not ship a built-in damper/relay driver. Any real-world actuation lives in your `on_change` actions.

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
  #   blower_rpm, airflow_cfm, compressor_rpm (actual), target_compressor_rpm
  #   (`compressor_rpm` = measured RPM [2..3]; `target_compressor_rpm` = commanded [0..1].)
  #   compressor_frequency, odu_expansion_valve, odu_commanded_stage, odu_stage, odu_mode, odu_line_voltage
  #   odu_outdoor_temp, odu_coil_temp, odu_suction_temp,
  #   odu_suction_superheat, odu_indoor_ambient, odu_discharge_temp
  #   odu_float_1 (superheat target), odu_float_2 (superheat actual),
  #   odu_float_3 (subcooling target), odu_float_4 (subcooling actual),
  #   odu_float_5, odu_float_6
  #   vacation_min_temp, vacation_max_temp
  #
  # Zone controller sensors (need zone_controller on the bus; zc_lat/zc_hpt are
  # the board's LAT/HPT thermistor ports, disabled by default):
  #   zc_zone_temperature (requires zone:), zc_lat, zc_hpt
  #
  # Cycle counters and runtime hours (may not be available on all systems):
  #   idu_low_heat_cycles, idu_high_heat_cycles, idu_med_heat_cycles,
  #   idu_blower_cycles, idu_poweron_cycles
  #   idu_low_heat_hours, idu_high_heat_hours, idu_med_heat_hours,
  #   idu_blower_hours, idu_poweron_hours
  #   odu_heat_cycles, odu_cool_cycles, odu_defrost_cycles, odu_poweron_cycles
  #   odu_heat_hours, odu_cool_hours, odu_defrost_hours, odu_poweron_hours
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

  # Per-zone hold state: "Schedule", "Hold until HH:MM PM", or "Hold - Permanent"
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Zone 1 Hold State"
    type: hold_state        # hold_state (per-zone, requires zone:)
    zone: 1

  # Diagnostic text sensors (no zone required)
  # tstat_ssid, tstat_hostname, tstat_wifi_mac,
  # tstat_cloud_host, tstat_proxy_server,
  # tstat_dealer_name, tstat_dealer_brand, tstat_dealer_url,
  # comfort_profile, fault_history
```

## SAM ASCII Interface

InfinitESP implements the Carrier SAM ASCII serial protocol — the same text command/response interface a real SYSTXCCSAM01 exposes on its DB-9 RS-232 port (9600 8N1, CRLF). The `sam_ascii` component is a plain ESPHome `UARTDevice`: it speaks the protocol over whatever `uart_id` you bind it to and has no transport of its own. The command set works the same way regardless of transport.

**Over the network (the default in the example config).** The bundled config binds `sam_ascii` to a `uart_tcp_server` on port 23, so you can drive it with any telnet client:

```bash
nc infinitesp.local 23
```

**Over a real RS-232 port (a true SAM replacement).** Point `sam_ascii` at a second hardware UART wired to an RS-232 transceiver and it behaves like the physical SAM's serial port — useful for replacing a faulty module or feeding a legacy automation controller that expects a SAM:

```yaml
uart:
  - id: bus_uart       # ABCD RS485 bus (38400)
    # ... RS485 pins ...
    baud_rate: 38400
  - id: ascii_uart     # SAM RS-232 port (9600 8N1)
    tx_pin: GPIO4
    rx_pin: GPIO5
    baud_rate: 9600
    data_bits: 8
    parity: NONE
    stop_bits: 1

sam_ascii:
  infinitesp_id: infinitesp_hub
  uart_id: ascii_uart
```

The rest of this section shows the commands; they work identically over either transport.

### Read Commands

| Command | Description |
|---------|-------------|
| `MODE?` | System mode (HEAT/COOL/AUTO/OFF/EMERGENCY HEAT) |
| `OAT?` | Outdoor air temperature (°F) |
| `TIME?` | Current time from bus clock |
| `DAY?` | Current day of week |
| `ZONE?` | Active zone bitmask |
| `BLIGHT?` | Backlight (ON/OFF) |
| `CFGEM?` | Display units (F/C) |
| `CFGDEAD?` | Heat/cool deadband (0-6) |
| `CFGCPH?` | Cycles per hour (2-6) |
| `CFGPER?` | Schedule periods per day (2 or 4) |
| `CFGPGM?` | Programming enabled (ON/OFF) |
| `DEALER?` | Dealer name |
| `DEALERPH?` | Dealer phone |
| `FILTRLVL?` / `UVLVL?` / `HUMLVL?` / `VENTLVL?` | Accessory life used % |
| `FILTRRMD?` / `UVRMD?` / `HUMRMD?` / `VENTRMD?` | Accessory reminder (ON/OFF) |
| `VACAT?` | Vacation state (ON/OFF) |
| `VACMINT?` / `VACMAXT?` | Vacation min/max temperature |
| `VACMINH?` / `VACMAXH?` | Vacation min/max humidity |
| `VACFAN?` | Vacation fan mode |
| `Z1RT?` | Zone 1 room temperature (°F) |
| `Z1RH?` | Zone 1 humidity (%) |
| `Z1RHTG?` | Zone 1 humidification target (%) |
| `Z1HTSP?` | Zone 1 heat setpoint (°F) |
| `Z1CLSP?` | Zone 1 cool setpoint (°F) |
| `Z1FAN?` | Zone 1 fan mode (AUTO/LOW/MED/HIGH) |
| `Z1HOLD?` | Hold state (OFF / ON until HH:MM PM / PERMANENT) |
| `Z1OVR?` | Timed override active (ON/OFF) |
| `Z1OTMR?` | Override timer (HH:MM) |
| `Z1UNOCC?` | Zone unoccupied (ON/OFF) |
| `Z1NAME?` | Zone 1 name |
| `HELP` | List all commands |

Prefix with `Z#` for other zones (e.g., `Z2HTSP?`).

### Write Commands

Append `!` and a value to set parameters:

```
MODE!COOL           # Set system mode
Z1HTSP!72            # Set zone 1 heat setpoint
Z1CLSP!68            # Set zone 1 cool setpoint
Z1FAN!AUTO           # Set zone 1 fan mode
Z1HOLD!120           # Hold zone 1 for 120 minutes
Z1HOLD!on            # Permanent hold
Z1HOLD!off           # Cancel hold, resume schedule
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

Mode and setpoint changes use a two-pronged approach: a 3B03 notification write (with change flags) primes the thermostat, then a direct data write delivers the actual values. Both are required. Neither alone works.

### Passive Snooping

In addition to SAM-register traffic, InfinitESP passively observes inter-device traffic on the bus to extract diagnostic data:

- **Indoor unit** (0x40): blower RPM, airflow CFM, electric heat status
- **Outdoor unit** (0x50): compressor RPM, demand %, operating stage, modulation, expansion valve position, superheat/subcooling, outdoor/coil/suction/discharge temperatures plus suction superheat
- **Zone controller** (0x60 / 0x61): zone temperatures and damper positions. When you are **not** emulating the zone controller, InfinitESP passively snoops the thermostat↔zone-controller traffic to report damper positions and drive per-zone climate conditioning state. (When you *are* emulating it, the same data is captured directly through the emulation path.)

No extra polling needed. The thermostat already queries these devices, and InfinitESP just listens in.

### WiFi Credential Caching

When using hardware UART transport, InfinitESP can automatically discover the thermostat's WiFi credentials from register 4608 and cache them to NVS flash. If the ESP32 subsequently boots without WiFi connectivity (e.g., changed router), it injects the cached credentials after a 15-second grace period. This lets the device self-provision from the bus. No manual WiFi config needed after initial setup.

This feature is not available with the TCP serial bridge transport (circular dependency: need WiFi to reach the TCP bridge, need the bus to get WiFi credentials).

## Targeted Systems

InfinitESP targets Carrier Infinity / Bryant Evolution / ICP systems that communicate over the ABCD RS485 bus. The protocol is shared across this family; only the author's own system has been confirmed working. The model numbers below identify the kinds of devices found on that bus. They are not a verified compatibility list, and behavior can vary across firmware revisions:

- **SAM modules** (the device InfinitESP emulates): SYSTXCCSAM01, SYSTXCCSAMC01
- **Thermostats**: Infinity Touch (SYSTXCCITC01), Evolution Connex (SYSTXBBECC01), legacy UID/UIZ controls with firmware 14+
- **HVAC equipment**: Infinity/Evolution furnace, air handler, or heat pump on the ABCD bus

What matters is the bus protocol, not the badge. See the [Disclaimer](#disclaimer) and [NOTICE](NOTICE).

## Troubleshooting

### Thermostat shows "SAM Communication Fault"

- Verify RS485 A/B wiring. Try swapping if unsure (wrong polarity won't damage anything)
- Check that UART baud rate is exactly 38400
- Watch ESPHome logs for any `crc_fail` in the STATS line

### No climate entities in Home Assistant

- Check that the Bus Status binary sensor shows `on`
- Zones populate after the ESP sees register data from the thermostat. Allow 10-15 seconds after first bus connection
- Verify your zone number (1–8) matches a zone that's active on your thermostat

### Setpoint changes don't take effect

- There's a 1–3 second propagation delay (poll cycle)
- If changes never take effect, check logs for CRC errors indicating RS485 signal quality issues
- Mode OFF may be rejected by some thermostat firmwares. This is a thermostat limitation, not a bug.

### Can't flash over USB

- Hold the BOOT button while plugging in USB to enter download mode
- On ESP32-S3, GPIO45 and GPIO46 are strapping pins. The YAML includes `ignore_strapping_warning: true`.

### Bus traffic looks garbled

- Check the STATS line in logs every 5 seconds
- Healthy: `crc_fail=0`, `stale=0`, `overflow_evts=0`, `reply_got ≈ reply_exp`
- `crc_fail > 0` → bus noise or bad wiring
- `overflow_evts > 0` → main loop not keeping up (reduce logging verbosity)
- `stale > 0` → bytes arriving with gaps > 100ms (transport issue)

## Reporting Issues

To help diagnose the problem, it's useful to include any of the following that you can gather:

**ESPHome logs** are the most useful for protocol issues:

```bash
esphome logs infinitesp.yaml --device infinitesp.local
```

30 seconds of output is usually plenty. The `STATS` line printed every 5 seconds contains bus health diagnostics (`crc_fail`, `reply_exp`, `reply_got`, etc.).

**The REPORT? command** provides a quick bus snapshot and often eliminates the need for full logs. If you can reach the device over the network, connect to the SAM ASCII interface (TCP port 23 in the default config) and run `REPORT?`. For example:

```bash
nc infinitesp.local 23
REPORT?
```

This produces a JSON dump of all observed bus traffic, device info, cached registers, and diagnostic counters: a self-contained snapshot of the bus state.

**Hardware details** help narrow down firmware-specific issues:

- Thermostat model and firmware version (shown in REPORT? under the `device` line for address `20`)
- Indoor unit model (furnace/air handler)
- Outdoor unit model (condenser/heat pump)
- Zone controller (installed or not)
- RS485 transport (direct UART or TCP serial bridge)

## Project Structure

```
infinitesp.yaml              # Main device config
secrets.yaml                 # WiFi credentials (not tracked)
components/
├── infinitesp/              # ABCD bus protocol engine
│   ├── infinitesp.h/cpp     # Core: frame parsing, register mgmt, polling, CRC
│   ├── climate/             # HA climate entities (setpoint, mode, fan, preset)
│   ├── cover/              # HA cover entities (per-zone damper position)
│   ├── sensor/              # Temperature, humidity, RPM, ODU diagnostic sensors
│   ├── binary_sensor/       # Bus status, compressor, electric heat
│   ├── select/              # System mode, fan mode selects
│   └── text_sensor/         # Zone names, WiFi info, dealer info, profiles
└── sam_ascii/               # SAM ASCII serial CLI (REPORT?, setpoints, etc.)
#
# Transport components (uart_tcp_client, usb_cdc_acm, uart_tcp_server,
# uart_bridge) come from esphome-uart-link, pulled in via external_components.
# See the UART Transports section above.
```

## Disclaimer

You are connecting a multi-thousand-dollar HVAC system to some random person's code on the internet. This project was built by reverse-engineering a proprietary serial protocol. There is no official documentation, no vendor endorsement, and no guarantee that anything here is correct. A bug, a misinterpreted register, or a corrupted write could send commands to your equipment that it wasn't designed to handle.

If that happens, that's on you. You chose to wire an ESP32 into your furnace. Nobody involved in this project is coming to fix your HVAC, replace your compressor, or explain to your spouse why the house is 90°F in July.

If that bothers you, don't use this.

## Acknowledgments

- [infinitude](https://github.com/nebulous/infinitude): Perl-based Infinity system controller and the origin of this project. Provides a full web interface for Infinity/Evolution systems, includes a SAM emulator, and was the primary protocol reference for InfinitESP.
- [infinitive](https://github.com/acd/infinitive): Go-based SAM emulator, reference for the write protocol
- Carrier/ICP protocol details from the open-source HVAC community

## License

MIT
