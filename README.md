# InfinitESP

ESPHome firmware for ESP32 that emulates Carrier/Bryant/ICP devices on the "ABCD" RS485 bus. System Access Module (SAM) emulation gives Home Assistant native control of HVAC systems, and Damper Control Module emulation gives the thermostat access to any physical hardware that ESPHome can actuate.

No cloud or Carrier API required, just a serial bus and a microcontroller.

> **Disclaimer:** This firmware was developed by reverse-engineering a proprietary protocol. Everything has been confirmed working on the author's system; your mileage may vary with different equipment, firmware versions, or bus configurations. Read the [full disclaimer](#disclaimer) before wiring anything.

## Design

InfinitESP speaks the Carrier ABCD bus protocol and registers as a SAM (address `0x92`). The thermostat sees a valid SAM on the bus and InfinitESP creates a climate device in Home Assistant to control each active zone:

| Entity Type | What You Get |
|---|---|
| **Climate** | Per-zone thermostat with heat/cool/auto/off modes, dual heat+cool setpoints, fan control, and preset support (per schedule, home, away, sleep, wake, hold timer, hold indefinitely) |
| **Covers** | Per-zone damper position (0–100%) from the zone controller (either an emulated or a real physical zone controller) |
| **Sensors** | Zone temperature, zone humidity, outdoor air temp, blower RPM, airflow CFM, compressor RPM, ODU demand/stage/modulation, expansion valve position, superheat/subcooling targets & actuals, ODU temperatures (outdoor/coil/suction/discharge) plus suction superheat, vacation min/max temps |
| **Binary Sensors** | Bus online/offline status, compressor running, electric heat active, per-zone occupancy (active_fault deprecated) |
| **Selects** | System mode (heat/cool/auto/off; emergency heat/heat pump behind an experimental flag), per-zone fan speed (auto/low/med/high) |
| **Text Sensors** | Zone names, hold state, thermostat WiFi SSID/hostname/MAC, proxy server, dealer info, comfort profile dump |

## Targeted Systems

InfinitESP targets Carrier Infinity / Bryant Evolution / ICP systems that communicate over the ABCD RS485 bus. The protocol is shared across this family. Only the author's own system has been confirmed working. The model numbers below identify the kinds of devices found on that bus. They are not a verified compatibility list, and behavior can vary across firmware revisions:

- **SAM modules** (the device InfinitESP emulates): SYSTXCCSAM01, SYSTXCCSAMC01
- **Thermostats**: Infinity Touch (SYSTXCCITC01), Evolution Connex (SYSTXBBECC01), legacy UID/UIZ controls with firmware 14+
- **HVAC equipment**: Infinity/Evolution furnace, air handler, or heat pump on the ABCD bus

See the [NOTICE](NOTICE).

## Hardware Options

Any ESP32 with an RS485 transceiver works. ESP8266 is untested.
The author's setup uses the **[Waveshare ESP32-S3-Relay-6CH](https://amzn.to/4mX6tLp)** board.

[<img src="https://www.waveshare.com/w/upload/thumb/e/ee/ESP32-S3-Relay-6CH.jpg/1200px-ESP32-S3-Relay-6CH.jpg" width="400" />](https://amzn.to/4mX6tLp)

The Waveshare board was chosen as a reference design for its case, onboard RS485 interface, and 6 relay outputs. The relays open the door to emulating other devices beyond the SAM, notably the NIM (Network Interface Module, SYSTXCCNIM01). The Damper Control Module (SYSTXCC4ZC01) is already part of InfinitESP. Damper actuation works through a trigger callback: each zone's cover fires `on_change` whenever the thermostat commands a new damper position, handing the new position to whatever relay or motor driver your hardware uses (see the [Covers](#covers) examples). When emulating the zone controller, each zone also takes a temperature sensor so the thermostat sees a room reading, falling back to the zone 1 temperature when no sensor is configured (see [Zone Controller](#zone-controller-optional)). The relay GPIOs are exposed in the example YAML config but are not part of the core SAM emulation. The firmware is hardware-agnostic. It just needs a `uart::UARTComponent`. The RS485 transceiver, ESP32 variant, and relay hardware are all irrelevant to the protocol engine.
Generic ESP32 dev boards with a separate RS485 transceiver module should also work. Community reports of boards and transceivers working in the field are collected on the wiki's [Known-Working Hardware](https://github.com/nebulous/infinitesp/wiki/Known-Working-Hardware) page.

> **Note:** The Waveshare board's RS485 auto-direction circuit has a time constant too short for reliable operation at 38400 baud. The circuit assumes the UART idle state (HIGH) means "stop transmitting," so runs of consecutive `1` bits cause it to stop driving the bus mid-byte. On an insufficiently-biased bus, the line voltage collapses and the receiver reads garbage.
>
> InfinitESP ships with an experimental `uart_rmtx` UART component that drives the RS485 interface through the ESP32's Remote Control Transceiver (RMT) peripheral instead of the hardware UART. It emits a sub-bit line code that keeps the transceiver's auto-direction DE line primed during long constant-level runs, avoiding the dropout with no hardware modification. Bench-validated against a single physical SAM (100% reply rate on 160 short-frame queries, zero CRC errors). See its [README](components/uart_rmtx/README.md) for the mechanism, configuration, and current validation status.
>
> A live Carrier ABCD bus may tolerate the flaw without any fix, depending on bus topology. If `uart_rmtx` does not resolve it, any other fix requires adding or modifying hardware (for example, bypassing the onboard transceiver with a separate RS485 module).
>
> Not every "auto-direction" module has this flaw. The Waveshare's comes from its RS485 chip (SP485EEN) being driven by a discrete RC one-shot, which is what drops out. Modules built around a transceiver with built-in AutoDirection control do it on-chip with a state machine instead. The MAX13487E found on the common HiLetgo and generic "TTL to RS485 hardware automatic flow control" boards is the usual one. They transmit long frames cleanly on a properly biased bus and do **not** need `uart_rmtx`. Their tradeoff is bias and loading: they depend on correct bus biasing for the idle state, and some setups report bus disruption from their collision sensing. If you see CRC errors clustered around your own transmissions with a MAX13487E-class module, look at wiring, bus loading, or biasing rather than the one-shot dropout.

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

> **Recommissioning is required when emulating a SAM or zone controller.** The thermostat does not recognize a newly emulated device until you run its commissioning process. This is the same dealer setup performed after installing physical hardware: the thermostat scans the bus, registers the device, and (for a zone controller) defines its zones. Run it from the thermostat's advanced setup menu. Until commissioning completes, the thermostat ignores the emulated device.

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

Review `infinitesp.yaml` and set up one `climate:` block per zone you have (1-8). Everything else — per-zone sensors, hold state, fan and damper entities, and the full system sensor set — is generated automatically from your zone list. Declare any entity explicitly only if you want to customize its name or behavior. The config enables SAM emulation only; zone controller emulation ships commented out and is for zoned systems with real dampers (see the [Zone Controller](#zone-controller-optional) section).

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

The device appears automatically via ESPHome's native API integration. All entities populate after the bus comes online.

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

The JSONL capture stream (port 2373) and the raw bus tap (port 4242) are diagnostics outputs, not transports. They are documented in [Bus Capture and Diagnostics](docs/bus-capture.md).

## Configuration Reference

Full type-by-type reference for every hub option and entity type: [docs/entity-reference.md](docs/entity-reference.md)

### Auto-Generated Entities

Each zone's sensors and controls, plus the system-wide entities, are generated automatically from the `climate:` blocks you declare. You only need to declare them if you want to change defaults.

- **Per zone** (from each climate block): temperature, humidity, occupancy, zone name, hold state, comfort profile, fan mode select, hold-until time, hold-minutes number, and the damper cover (generated on every zone; it publishes only when a zone controller is on the bus, see [Covers](#covers)).
- **System-wide**: outdoor temperature, blower RPM, airflow, IDU heat stage, compressor running, bus status, ODU temperature/stage sensors, vacation setpoints, vacation duration (number, see below), heat source (furnace / heat_pump / electric / none), fault history and fault timestamp. The deprecated `electric_heat` binary sensor no longer publishes (0316[0] is the source-blind IDU heat stage; use `idu_heat_stage` + `heat_source`).
- **Diagnostics** (entity category diagnostic): IDU/ODU cycle and hour counters, thermostat wifi/dealer strings, thermostat/IDU/ODU manufacture dates, firmware version. Disable the whole group with `auto_diagnostics: false` in the `infinitesp:` block.
- **Equipment-conditional** (generated disabled by default, enable in HA if your hardware serves them): the variable-speed ODU family — compressor RPM, ODU requested CFM, expansion valve, float registers, discharge/suction temperatures, superheat.

Rules:

- **Explicit definitions get priority.** An entity you declare yourself (same type and zone) replaces the generated one, keeping your name and options. Existing fully-explicit configs compile unchanged.
- **Per-zone opt-outs.** Each generated per-zone entity can be disabled with an option on its climate block, default true, named after the entity (`temperature: false`, `damper: false`; full list in the [reference](docs/entity-reference.md#climate-block)). The option affects that zone only. System-wide entities have no individual switches: declare one explicitly to take control of it, or set `auto_diagnostics: false` to drop the diagnostics group.
- **Manual-only**: `raw_register` sensors and the zone controller sensor feeds (`zc_zone_temperature`, `zc_lat`, `zc_hpt`).
- **Manufacture-date matching.** The three generated date sensors each cover one device class (thermostat, IDU, ODU); a manual `manufacture_date` block replaces the generated one for its class. Matching rules: [reference](docs/entity-reference.md#manufacture_date-matching).

### Core Component

```yaml
infinitesp:
  id: infinitesp_hub
  uart_id: bus_uart
  sam_address: 0x92    # 0x93 = FakeSAM test mode, 0 = disabled (passive
                       # monitor; set 0 if a physical SAM is installed)
  # Zone controller emulation, OFF by default: enable only on a zoned system
  # with real dampers (see the warning below).
  # zone_controller_address: 0x60
  # Pin the indoor/outdoor unit to an exact bus node when the unit sits off
  # the usual class nibble, or to keep a second class-5 node out of ODU
  # entities (e.g. a furnace at 0x3E, an ODU at 0x57):
  # idu_address: 0x3E
  # odu_address: 0x57
  # temperature_unit: auto    # auto (default), F, or C
  # status_light_id: rgb_led  # or status_led_pin: GPIO2 (see Status LED)
  # experimental_heat_source_modes: true   # protocol experiment, see Selects
```

Full option list with semantics, including what passive mode (`sam_address: 0`) still transmits: [entity reference](docs/entity-reference.md#hub-block-infinitesp).

#### Zone Controller (optional)

Carrier zoned systems put a Damper Control Module (SYSTXCC4ZC01) on the bus at address `0x60`. InfinitESP can either **emulate** one or **passively monitor** a real one:

- **`zone_controller_address: 0x60`**: InfinitESP emulates the zone controller. The thermostat talks to InfinitESP as if it were the hardware. This lets you inject temperatures from external sensors (below). Do **not** enable this if a physical zone controller is already on the bus, since the two would collide at `0x60`.

> **Warning: emulation requires real dampers, or it halts conditioning.** When the thermostat commissions a zone it runs an automated duct checkout: it commands different damper positions and blower speeds, then reads the indoor unit's fan telemetry to build a damper-position-to-airflow map. That map only converges if closing a damper actually raises duct static pressure, which needs real physical dampers and ductwork. An emulated zone controller reports damper position on the bus but produces no airflow change **unless** you have wired real dampers to the cover's [`on_change`](#on_change-trigger-on-commanded-moves) trigger to actually open and close them. With no dampers on the trigger, the checkout never validates. It runs both at install and as a recurring daily checkout, and **it stops all heating and cooling while it runs, potentially indefinitely until resolved**. This is a property of the emulation, not a bug InfinitESP can fix. Emulate a zone controller only on a system with real dampers and ductwork installed (and wire them to `on_change` so they actually move). To add a temperature sensor to an existing zoned system, leave `zone_controller_address: 0` and let InfinitESP passively monitor the real hardware.
- **`zone_controller_address: 0`** (default): no emulation. InfinitESP passively snoops the thermostat↔zone-controller traffic and still reports damper positions and per-zone conditioning state from the real hardware. This is the mode to use if you already have a physical Damper Control Module installed.

In either case the damper `cover` entities (see [Covers](#covers)) and per-zone climate heating/cooling action reflect the real damper state.

**Injecting external zone temperature sensors (emulation only).**
When emulating the zone controller, the thermostat expects each zone to report a temperature. InfinitESP can source these from any ESPHome sensor (a local Dallas 1-Wire or DHT/BME wired to the ESP32, or a Home Assistant entity via the `homeassistant` platform) instead of letting the thermostat read its own remote sensors. Configure one block per zone for zones 2-8 (zone 1 is reported by the thermostat itself):

```yaml
infinitesp:
  zone_controller_address: 0x60
  zc_zone_2:
    temperature_sensor: upstairs_temp   # any ESPHome sensor id (set internal: true on it)
    sensor_unit: F                      # REQUIRED: the unit your sensor publishes
    # staleness_timeout: 120            # seconds before the value reverts (default 120)
```

**Set `sensor_unit` explicitly for every zone.** It declares the unit your sensor *publishes*, not the thermostat's display setting; a wrong guess causes silent mis-conversion. After configuring, check the zone temperatures on your thermostat and confirm they match the room: out-of-band readings fall back to the zone-1 ambient value, which looks plausible without being that zone's reading.

Systems with more than four zones use a second controller at `0x61` (`zc_zone_5` through `zc_zone_8`). The LAT/HPT thermistor ports, the 40-99 °F sanity band, staleness behavior, and the secondary-controller mapping: [entity reference](docs/entity-reference.md#zone-controller-sensor-feeds).

### Status LED

Optional, set on the hub: `status_light_id` (an existing ESPHome light, RGB supported) or `status_led_pin` (a simple GPIO). Mutually exclusive.

| Status | RGB Color | Simple LED | Meaning |
|--------|-----------|------------|---------|
| Bus not online | Yellow blink (1s) | Slow blink (1s) | Bus not yet established |
| Bus online, no WiFi | Blue blink (500ms) | Fast blink (250ms) | Bus good, WiFi connecting |
| Both online | Solid green | Solid on | Everything working |

### Temperature Units

Temperature sensors publish in °C and Home Assistant converts for display. InfinitESP reads the thermostat's °F/°C setting from the bus automatically and decodes accordingly. Force a unit with `temperature_unit: F` or `C` if the detection misbehaves. Register mechanics and the boot-time heuristic: [entity reference](docs/entity-reference.md#temperature-unit-detection).

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

Reports each zone's commanded damper position (0–100%) from register `0308` (mirrored to `0319`). The thermostat uses 16 steps (`0x00`–`0x0F`): cover position = `step / 15.0`. Works whether you emulate the zone controller or passively monitor a physical one. Without any zone controller on the bus, these covers never publish (they report `unknown`).

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

`pos * 15.0` gives the protocol's 16 damper steps if you need proportional control. You handle any relay timing in the lambda. Home Assistant-set positions are transient: the cover acknowledges the command immediately, but the next bus-commanded change overwrites the reported position with what the thermostat actually commands. InfinitESP does not ship a built-in damper/relay driver. Any real-world actuation lives in your `on_change` actions.

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

  # Full sensor type list with units, zones, and which are generated:
  # docs/entity-reference.md
```

### Binary Sensors

```yaml
binary_sensor:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Bus Status"
    type: bus_status       # bus_status, compressor_running, occupancy (electric_heat deprecated)
  # (active_fault is deprecated and publishes nothing — no fault-active state
  # exists on the bus; its old decode misread a transient flag. Use the
  # fault_timestamp sensor instead.)
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Zone 1 Occupancy"
    type: occupancy        # per-zone: occupied vs the thermostat's away state (not motion)
    zone: 1
```

### Selects

```yaml
select:
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "System Mode"
    type: system_mode      # system_mode (global): heat/cool/auto/off
                           # (+ emergency_heat/heat_pump only with
                           #  experimental_heat_source_modes, see the note below)

  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Zone 1 Fan Mode"
    type: fan_mode         # fan_mode (per-zone, requires zone:)
    zone: 1

  # Which zone the wall control displays. Manual, system-wide, zone key ignored.
  - platform: infinitesp
    infinitesp_id: infinitesp_hub
    name: "Displayed Zone"
    type: displayed_zone   # options 1-8
```

Setting the displayed zone sends the same write a real SAM01 makes for `S1ZONE!`. Whether the wall control switches is generation-dependent: older UI-family controls adopt it, newer SYSTXCC touch controls ACK and revert. Details: [entity reference](docs/entity-reference.md#select-types).

**Heat-source control is not on the bus.** The `heat_source` text sensor reports what the system is running, but no bus path sets it: the selection lives in the thermostat (wall or Carrier app). The `emergency_heat` / `heat_pump` select options are hidden unless `experimental_heat_source_modes` is set on the hub, and on Next Gen Infinity thermostats the `heat_pump` write has been observed turning the system off mid-call. Treat them as protocol experiments, not control. Details: [entity reference](docs/entity-reference.md#select-types).

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

  # Full text sensor type list and the manufacture_date matching rules:
  # docs/entity-reference.md
```

### Timed Holds

Each zone gets two entities that read and set the same native bus timed hold, with the thermostat owning the countdown (the same mechanism the wall unit uses):

- **Zone N Hold Until** (a `time` entity): set a clock time and the hold ends then. Good for absolute terms ("hold until bedtime").
- **Zone N Hold Minutes** (a `number`, 0-1425 in steps of 15): remaining minutes, 0 when no timed hold is running. Set it to arm a hold for N minutes; set 0 to cancel and return to Per Schedule. Good for durations ("hold two hours") and for automations.

Both entities mirror a running hold, land on the thermostat's quarter-hour grid (a requested 20 minutes holds for 15), and debounce picker chatter before writing the bus. Full semantics, including the within-15-minutes-means-tomorrow rule: [entity reference](docs/entity-reference.md#timed-holds).

### Vacation

A system-wide **Vacation Hours** number (0-8760) arms and clears vacation: set the duration in hours and the thermostat clamps every zone's setpoints to its vacation min/max; set 0 to end it and return to schedule. It works at the bus's native one-hour resolution, so durations the wall UI cannot express ("away for 5 hours") are available.

One caveat before you rely on short durations: some thermostat families (the SYSTXCC reference among them) floor sub-day values to whole days, so anything under 24 hours *clears* an active vacation instead of arming one. 24 and 48 hours arm normally.

Any preset command other than Vacation also ends an active vacation. Full rules and the ASCII verbs: [entity reference](docs/entity-reference.md#vacation).

### Fault Entities

`fault_timestamp` (sensor) changes state each time the thermostat logs a new fault (register 0x4202); it needs `time_id`. `fault_history` (text sensor) renders the ten-entry fault log. No fault-active/cleared state exists on the bus, so these are event-style entities, not liveness flags. Semantics: [entity reference](docs/entity-reference.md#fault-entities).

## SAM ASCII Interface

InfinitESP implements the Carrier SAM ASCII serial protocol, the same text command/response interface a real SYSTXCCSAM01 exposes on its DB-9 RS-232 port (9600 8N1, CRLF). The bundled config binds it to a TCP server on port 23, so you can drive it with any telnet client:

```bash
nc infinitesp.local 23
```

Every `?` verb reads state; appending `!` and a value writes (writes need SAM emulation):

```
MODE?                # system mode
OAT?                 # outdoor air temperature (°F)
Z1RT? / Z1HTSP?      # zone 1 room temp / heat setpoint
Z1HOLD!120           # hold zone 1 for 120 minutes
VACDAYS!7            # arm vacation for 7 days
REPORT?              # full bus snapshot as JSON
HELP                 # all commands
```

Prefix zone verbs with `Z#` for other zones (`Z2HTSP?`, `Z3FAN!`, ...).

`sam_ascii` is a plain ESPHome `UARTDevice`: it can also bind to a second hardware UART wired to an RS-232 transceiver, acting as a true SAM replacement for legacy automation controllers. Full verb tables, the RS-232 binding, and the `REPORT?` capture recipe with privacy notes: [SAM ASCII interface](docs/sam-ascii.md).

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
- Mode OFF may be rejected by some thermostat firmware versions. This is a thermostat limitation, not a bug.

### Can't flash over USB

- Hold the BOOT button while plugging in USB to enter download mode
- On ESP32-S3, GPIO45 and GPIO46 are strapping pins. The YAML includes `ignore_strapping_warning: true`.

### Bus traffic looks garbled

- Check the STATS line in logs every 5 seconds
- Healthy: `crc_fail=0`, `stale=0`, `overflow_evts=0`, `reply_got ≈ reply_exp`
- `crc_fail > 0` → bus noise or bad wiring
- `overflow_evts > 0` → main loop not keeping up (reduce logging verbosity)
- `stale > 0` → bytes arriving with gaps > 100ms (transport issue)
- Counter-by-counter decoding: [Bus Capture and Diagnostics](docs/bus-capture.md#stats-bus-health)

### A manufacture-date sensor stays unknown

- Generated date sensors cover a whole device class and populate on their own. A manually declared one with `device_address` matches that exact node only: verify the address against a `REPORT?` dump (an ODU often answers at 0x52, not 0x50).
- The IDU/ODU dates decode from register 0104 or, on two-stage ODUs, 3E09. If the thermostat never polls device info, no date ever arrives.

## Reporting Issues

To help diagnose the problem, include any of the following you can gather:

- **`capture.jsonl`** (best for timing-sensitive problems: mode changes that don't stick, flapping entities, suspected collisions): `nc infinitesp.local 2373 | tee capture.jsonl` for 30 seconds around the behavior. Format spec: [Bus Capture and Diagnostics](docs/bus-capture.md).
- **`report.json`**: a `REPORT?` snapshot over the SAM ASCII port, a self-contained JSON dump of observed bus traffic, cached registers, and counters. Redacted for public posting. Recipe: [SAM ASCII interface](docs/sam-ascii.md#report).
- **ESPHome logs**: `esphome logs infinitesp.yaml --device infinitesp.local` for 30 seconds. The `STATS` line printed every 5 seconds carries bus health counters.

**Hardware details** help narrow down firmware-specific issues:

- Thermostat model and firmware version (shown in REPORT? under the `device` line for address `20`)
- Indoor unit model (furnace/air handler)
- Outdoor unit model (condenser/heat pump)
- Zone controller (installed or not)
- RS485 transport (direct UART or TCP serial bridge)

### 📣 Calling all users:

If you have hardware such as a Carrier NIM (SYSTXCCNIM01), ~Damper Control Module (SYSTXCC4ZC01)~(implemented, but new logs are always good validation), or any other interesting communicating hardware (remote room sensors, zone controllers, etc.) on your ABCD bus and would be willing to capture raw bus traffic, please open an issue. Understanding and emulating these devices requires protocol traces that can only come from real hardware. Even a few minutes of logs would be valuable. A `REPORT?` snapshot helps too, but the [JSONL capture stream](docs/bus-capture.md) is best for emulation work since it shows the timing and framing that static register dumps miss. Share captures via a [GitHub discussion on the infinitude project](https://github.com/nebulous/infinitude/discussions) or by contacting the author directly.

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
├── uart_rmtx/              # RMT-line-code UART driver (RS485 auto-direction error workaround)
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
