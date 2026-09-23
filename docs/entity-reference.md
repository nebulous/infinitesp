# Entity and configuration reference

Every InfinitESP config declares one `infinitesp:` hub block and one `climate:` block
per zone. All other entities generate from those declarations. Declare an entity
manually only to customize its name or options, or when the type is manual-only.
See the README "Auto-Generated Entities" section for the generation rules: an explicit
declaration replaces the generated entity of the same type (and zone, and device class
for variant types); opt-out flags on the climate block remove generated entities.

## Hub block (`infinitesp:`)

| Option | Default | Description |
|---|---|---|
| `id` | auto | Hub id, referenced by every entity's `infinitesp_id`. |
| `uart_id` | required | UART or bridge the bus runs on. |
| `sam_address` | `0x92` | Bus address for SAM emulation. `0` disables (see [passive mode](#passive-mode-sam_address-0)). |
| `address` | - | Deprecated alias for `sam_address`. |
| `zone_controller_address` | `0` | `0x60` emulates a zone controller. `0` passively monitors a real one. |
| `idu_address` | `0` | Pin the indoor unit to an exact bus node instead of device-class matching. |
| `odu_address` | `0` | Pin the outdoor unit to an exact bus node instead of device-class matching. |
| `temperature_unit` | `auto` | `auto` (read from the bus), `F`, or `C`. See [temperature unit detection](#temperature-unit-detection). |
| `auto_diagnostics` | `true` | `false` drops the generated diagnostics group (below). |
| `experimental_heat_source_modes` | `false` | Adds `emergency_heat` / `heat_pump` to the system mode select and `MODE!`. Protocol experiment, not control. See [select types](#select-types). |
| `status_light_id` | - | Existing light entity for status. Mutually exclusive with `status_led_pin`. |
| `status_led_pin` | - | GPIO for a simple status LED. |
| `flow_control_pin` | - | RS485 transmit-enable (DE/RE) GPIO. |

### Passive mode (`sam_address: 0`)

Passive mode disables emulation, not all transmission. The firmware still sends an
`0x93` table-name discovery probe to identify observed devices, and with
`temperature_unit: auto` it polls the thermostat's 3B05 to detect the display unit.
Writes (setpoints, holds, mode, vacation) are refused in this mode.

### Zone controller sensor feeds

`zc_zone_2` through `zc_zone_8`, plus `zc_lat` and `zc_hpt`, feed external temperature
sensors into zone controller emulation. Each takes:

| Key | Default | Description |
|---|---|---|
| `temperature_sensor` | - | ESPHome sensor id to feed from. |
| `sensor_unit` | - | `C` or `F`. Set it explicitly. |
| `staleness_timeout` | `120` | Seconds before the entry reverts. Zones fall back to zone 1 ambient, LAT/HPT to not-installed. |

These require `zone_controller_address`. Zones 2-4 sit on the primary controller (0x60),
zones 5-8 on a second controller (0x61, seeded only when a zone 5-8 feed is wired).
A thermostat commissioned for only four zones never polls `0x61`, so the secondary
stays inert unless you wire sensors into `zc_zone_5` through `zc_zone_8`.

**Set `sensor_unit` explicitly for every zone.** It declares the unit your sensor
*publishes*, not the thermostat's display setting. The two are unrelated: flipping
the thermostat between °F/°C display does not change what your sensor publishes.
ESPHome emits a config warning for any zone where `sensor_unit` is missing. The
default is the system unit, and a wrong guess causes silent mis-conversion.

**Sanity band.** InfinitESP rejects any injected reading that converts to values
outside of the 40-99 °F band (the indoor range the thermostat itself uses for
setpoints) and falls back to the primary zone-1 ambient value until a plausible
reading returns. A wrong `sensor_unit` always lands outside this band: a °F sensor
treated as °C reports a 70 °F room as ~160 °F, so the zone reads zone-1 ambient
instead of garbage. The range check is a safety net, not a correctness test: after
configuring, check the zone temperatures on your thermostat and confirm they match
the room. A reading that silently fell back to zone-1 because of a mis-set unit
looks "fine" (a real temperature, just not *that* zone's), so visual confirmation
is the only reliable validation.

**LAT/HPT thermistor ports.** The zone board also has leaving-air-temperature (LAT)
and HPT thermistor ports, reported as TLV entries in the same register (ids `0x14`
and `0x1C`). `zc_lat` and `zc_hpt` feed external sensors into those ports. Unlike
zone temperatures, supply-air temp has no sane ambient fallback: when the fed sensor
goes stale (past `staleness_timeout`), the entry reverts to not-installed so the
thermostat stops seeing it rather than reading a bogus value. These sensors are
disabled by default in Home Assistant. Enable them if your board reports them.

Sensor injection requires emulation. With a passive (physical) zone controller,
the real hardware owns temperature reporting and these blocks have no effect.
If `temperature_sensor` is omitted for a zone, InfinitESP reports whatever the
bus last reported for that zone.

### Temperature unit detection

The Carrier ABCD bus encodes temperatures differently depending on the thermostat's
display unit setting (°F or °C). In `auto` mode InfinitESP reads the active unit
from the bus and applies it automatically.

The unit flag lives at data offset 1 of every table-0x3B register the thermostat
serves (state, zones, accessories, dealer): `0x00` = English/°F, `0x01` = Metric/°C.

- When emulating the SAM, the thermostat pushes its dealer register (3B06) to InfinitESP on every poll cycle. InfinitESP reads the flag from it.
- When not emulating the SAM, InfinitESP polls the thermostat's accessories register (3B05) and reads the flag from the reply.

Until the first authoritative read lands (a few seconds after boot), InfinitESP falls
back to a heuristic: any active zone temperature byte ≤ 50 means °C. No plausible
HVAC zone exceeds 50°C (122°F).

| Value | Behavior |
|-------|----------|
| `auto` (default) | Read the unit from the bus (3B06 when emulating the SAM, else a polled 3B05), with the zone-temperature heuristic as a boot-time fallback. |
| `F` | Force Fahrenheit. |
| `C` | Force Celsius. |

The explicit options exist for edge cases or debugging. All temperature sensors
publish in °C with `device_class: temperature`, so Home Assistant converts to the
user's preferred display unit.

## Climate block

| Key | Description |
|---|---|
| `zone` | 1-8, required. |
| opt-out flags | `temperature`, `humidity`, `occupancy`, `zone_name`, `hold_state`, `comfort_profile`, `fan_mode`, `damper`, `hold_until`, `hold_minutes`. All default `true`; set `false` to drop that generated entity for this zone. |

## Generated entity surface

Per zone (from each climate block): temperature and humidity sensors, occupancy binary
sensor, zone name and hold state text sensors, comfort profile text sensor, fan mode
select, damper cover, hold-until time entity, hold-minutes number.

System-wide core: bus status, electric heat, compressor running binary sensors. Outdoor
temperature, blower RPM, airflow CFM, vacation min/max, ODU outdoor temp, ODU coil temp,
ODU stage, ODU commanded stage, ODU mode, ODU line voltage sensors. System mode select.
Fault history text sensor and fault timestamp sensor.

Diagnostics group (behind `auto_diagnostics`): IDU and ODU cycle/hour counters, thermostat
wifi and dealer strings, thermostat/IDU/ODU manufacture dates, firmware version.

Equipment-conditional group (generated disabled by default, enable in HA if your hardware
serves the registers): compressor RPM, target compressor RPM, ODU requested CFM, ODU
expansion valve, ODU floats 1-6, ODU discharge and suction temps, ODU suction superheat.
The 3E two-stage ODU family does not serve most of these.

## Sensor types (`platform: infinitesp`)

`generation` marks how the entity normally exists: per-zone, core, diagnostic, conditional
(disabled by default), or manual (never generated).

| Type | Zone | Unit | Generation | Notes |
|---|---|---|---|---|
| `temperature` | yes | °C | per-zone | Zone temperature from SAM state. |
| `humidity` | yes | % | per-zone | |
| `outdoor_temperature` | - | °C | core | |
| `vacation_min_temp` | - | °C | core | |
| `vacation_max_temp` | - | °C | core | |
| `blower_rpm` | - | RPM | core | IDU. |
| `airflow_cfm` | - | ft³/min | core | IDU. |
| `compressor_rpm` | - | RPM | conditional | Actual RPM. |
| `target_compressor_rpm` | - | RPM | conditional | |
| `odu_requested_cfm` | - | ft³/min | conditional | ODU 0608. `compressor_frequency` is the deprecated alias. |
| `odu_expansion_valve` | - | % | conditional | |
| `odu_float_1` .. `odu_float_4` | - | °F | conditional | Superheat/subcooling deltas. Native °F, no HA conversion. |
| `odu_float_5` | - | °F | conditional | Delta. |
| `odu_float_6` | - | - | conditional | Dimensionless. |
| `odu_outdoor_temp` | - | °C | core | |
| `odu_coil_temp` | - | °C | core | |
| `odu_discharge_temp` | - | °C | conditional | |
| `odu_suction_temp` | - | °C | conditional | |
| `odu_suction_superheat` | - | °F | conditional | Delta, native °F. |
| `odu_stage` | - | - | core | |
| `odu_commanded_stage` | - | - | core | |
| `odu_mode` | - | - | core | |
| `odu_line_voltage` | - | V | core | |
| `odu_indoor_ambient` | - | °C | manual | Zone temperature covers it on most installs. |
| `idu_low_heat_cycles` / `idu_high_heat_cycles` / `idu_blower_cycles` / `idu_poweron_cycles` | - | cycles | diagnostic | total_increasing. |
| `idu_med_heat_cycles` | - | cycles | manual | |
| `idu_low_heat_hours` / `idu_high_heat_hours` / `idu_blower_hours` / `idu_poweron_hours` | - | h | diagnostic | total_increasing. |
| `idu_med_heat_hours` | - | h | manual | |
| `odu_heat_cycles` / `odu_cool_cycles` / `odu_defrost_cycles` / `odu_poweron_cycles` | - | cycles | diagnostic | |
| `odu_heat_hours` / `odu_cool_hours` / `odu_defrost_hours` / `odu_poweron_hours` | - | h | diagnostic | |
| `fault_timestamp` | - | - | core | Needs `time_id` for the epoch anchor. |
| `zc_zone_temperature` | yes | °C | manual | What the ZC serves for a zone, from the 0302 TLV. |
| `zc_lat` | - | °C | manual | Disabled by default. LAT thermistor port. |
| `zc_hpt` | - | °C | manual | Disabled by default. HPT thermistor port. |
| `raw_register` | - | any | manual | Generic bus field, see below. |

Sensors with a temperature device class convert to °C for HA. Deltas (floats,
superheat) publish native °F with no device class, since HA's conversion would
corrupt them.

### `raw_register`

| Key | Default | Description |
|---|---|---|
| `device_address` | required | Exact bus node. |
| `register` | required | Register number (table<<8 or row). |
| `offset` | `0` | Byte offset into the register payload. |
| `datatype` | - | `uint8`, `int8`, `uint16_be`, `int16_be`, `uint32_be`, `int32_be`, `f32_be`. Mutually exclusive with `lambda`. |
| `lambda` | - | Full-payload decode, `float lambda(const std::vector<uint8_t> &data)`. |
| `scale` | `1.0` | Multiplier for datatype mode. |
| `value_min` / `value_max` | - | Sanity band, set together. Out-of-range values are not published. |

## Binary sensor types

| Type | Zone | Generation | Notes |
|---|---|---|---|
| `bus_status` | - | core | |
| `electric_heat` | - | core | IDU. |
| `compressor_running` | - | core | ODU. |
| `occupancy` | yes | per-zone | Thermostat occupied/away schedule state, not motion. |
| `active_fault` | - | manual | Deprecated, publishes nothing. No fault liveness exists on the bus. |

## Text sensor types

| Type | Zone | Generation | Notes |
|---|---|---|---|
| `zone_name` | yes | per-zone | |
| `hold_state` | yes | per-zone | |
| `comfort_profile` | yes | per-zone | Zone 1 by default. |
| `fault_history` | - | core | |
| `manufacture_date` | - | one per class | Thermostat (bare), IDU, ODU. See matching rules below. |
| `version` | - | diagnostic | Firmware version. |
| `tstat_ssid`, `tstat_hostname`, `tstat_wifi_mac`, `tstat_cloud_host`, `tstat_proxy_server`, `tstat_dealer_name`, `tstat_dealer_brand`, `tstat_dealer_url` | - | diagnostic | |

### `manufacture_date` matching

The generated set covers the three device classes. A manual block replaces the
generated entity for its class:

- `device_address`: exact bus node. For two devices in one class. Must be the node's
  real address (an ODU often answers at 0x52, not 0x50). Check against a `REPORT?` dump.
- `bus_class`: class nibble (1-15). Lenient across installs.
- Bare: the thermostat class.

The two keys are mutually exclusive. `device_address: 0` behaves as unset.

### Unit address pins (`idu_address` / `odu_address`)

By default IDU and ODU entities match their device's class nibble (4 and 5).
Carrier commissions the low nibble per install, and the class itself is not a
guaranteed device-type key: furnaces have been observed at `0x40` and `0x3E`,
and an install can carry a second class-5 node (a refrigerant dissipation
board) alongside the ODU. Setting:

```yaml
infinitesp:
  idu_address: 0x3E
  odu_address: 0x57
```

pins every IDU/ODU-scoped entity (blower RPM, airflow, electric heat, ODU
sensors, unit manufacture dates, slow polls) to that exact node and keeps other
same-class nodes out entirely. `0` (default) keeps class matching. Addresses
equal to `sam_address`, `zone_controller_address`, the thermostat (`0x20`), or
broadcast (`0xF1`) are rejected at validation; an `idu_address` in class 5 (or
`odu_address` in class 4) warns as a probable swap. A manual
`manufacture_date` block pinned to the configured node suppresses the
generated twin for that class.

## Select types

| Type | Zone | Generation | Options |
|---|---|---|---|
| `system_mode` | - | core | heat, cool, auto, emergency_heat, off |
| `fan_mode` | yes | per-zone | auto, low, med, high |
| `displayed_zone` | ignored | manual | 1-8 |

`displayed_zone` is which zone the wall control displays: SAM register 3B02
byte 28, options "1"-"8" mapped straight to the byte. Manual-only and
system-wide (the zone key is ignored, same as system_mode). Reads follow the
wall control's zone button: byte 28 changes within a few seconds of a press,
and the entity publishes on the next 3B02 poll (~6 s fast poll, ~1 min
broadcast). It requires SAM emulation (`sam_address` nonzero); on passive
installs the register is never served and the entity stays unknown. Setting
it sends the same write a real SAM01 makes for `S1ZONE!`. Every thermostat
tested ACKs the write; whether the display switches depends on the wall
control's generation. The older UI family (UIZ-era) are said to adopt it(issue #37)
and newer touch models ack and revert without changing the displayed value.

**Heat-source control is not on the bus.** The `heat_source` text sensor reports what
the system is running (furnace / heat_pump / electric / none), but no bus path sets
it: the selection lives in the thermostat (wall or Carrier app), and the
emergency_heat / heat_pump mode writes never select it. On Next Gen Infinity
thermostats the `heat_pump` write has additionally been observed turning the system
off mid-call, and `off` writes can be ignored while heating. Those two options are
hidden from the System Mode select unless `experimental_heat_source_modes` is set on
the hub, and every use logs a warning. Treat them as protocol experiments, not control.

## Covers

Damper cover, one per zone. `zone` required. `on_change` trigger fires with the new
position (0.0-1.0) when the thermostat commands a damper move. Without a trigger the
cover is a position reporter. Generated for every declared zone. With no zone controller
on the bus it stays unknown.

## Time and number entities

Time entities and hold-minutes numbers are per zone, generated. One system-wide
vacation-hours number is generated alongside them.

### Timed holds

Two entities read and set the same native bus timed hold. The thermostat owns
the countdown, the same mechanism the wall unit uses:

- **Hold until** (`time` entity): set a clock time and the hold ends then. Good for absolute terms ("hold until bedtime").
- **Hold minutes** (`number`, 0-1425 in steps of 15): remaining minutes, 0 when no timed hold is running. Set it to arm a hold for N minutes; set 0 to cancel and return to Per Schedule. Good for durations ("hold two hours") and for automations.

Behavior both entities share:

- They show the value the bus will actually produce: a requested 20 minutes displays as 15 immediately (the thermostat's grid is quarter-hours), and a requested 8:40 PM end may hold until 8:45 PM. The `hold_state` sensor always shows the true end.
- Sets are debounced for about a second and a half after you stop adjusting, because Home Assistant's pickers send every intermediate value. One bus write arms the hold after the value settles.
- A hold-until target within 15 minutes of now means tomorrow.
- While a timed hold runs, both entities mirror it. After the hold ends or is cancelled they keep their last value until the next hold is set.
- Cancel with either zero minutes or the climate entity's "Per Schedule" preset.

### Vacation

**Vacation hours** (`number`, 0-8760 in steps of 1, system-wide, zone key ignored)
arms and clears vacation: set it to the duration in hours and the thermostat clamps
every zone's setpoints to its configured vacation min/max; set 0 to end vacation and
return to schedule. It uses the bus's native one-hour resolution, so durations the
wall UI cannot express ("away for 5 hours") work. The matching ASCII verbs are
`VACDAYS!`/`VACHOURS!`.

- **Tstat-family caveat on sub-day durations:** some thermostat families (our
  SYSTXCC-reference among them) floor the hours value to whole days when
  adopting the write. Anything under 24 hours reads as 0 days and *clears* an
  active vacation instead of arming one (`VACHOURS!5` ends vacation on these;
  24 and 48 arm normally). Older UI-family controls honor native hours
  (verified by a real-SAM01 user). Durations under 24 h are sent exactly as
  commanded; whether they arm depends on the wall control. If a short duration
  does not take, this is why.
- The thermostat does not report the countdown on the bus. Register 4012 carries
  only the vacation config, so the number shows the duration you last set, not a
  ticking remaining. Vacation activity itself is visible on the climate entities
  as the "Vacation" preset, and the wall unit's own vacation banner keeps counting
  down normally.
- Any preset command other than Vacation (Per Schedule, Wake, the standard home/
  away/sleep presets) ends an active vacation, including one armed at the wall unit.
  Arming vacation needs a duration, so setting the Vacation preset itself from HA
  is a no-op. Use the number.
- Vacation min/max setpoints come from the thermostat's own config (visible as the
  vacation min/max temp sensors); `VACMINT!`/`VACMAXT!` can change them.
- Requires SAM emulation; on a passive install the number accepts the value but
  sends nothing (it snaps back), like all write verbs.

## Fault entities

`fault_timestamp` (sensor, timestamp device class) is the time the most recent
fault was logged (thermostat register 0x4202). A state change means a new fault
was logged. Detection can lag by one slow-poll rotation (~5-7 min): the state is
the fault's logging time, not the observation time. One-minute granularity: two
faults logged within the same minute produce one state change. Requires `time_id`
pointing at your `time:` source. No fault-active/cleared state exists on the bus;
the wall thermostat's fault banner and Carrier's cloud `active` field are
internal to the thermostat and are never published to the bus.

`fault_history` (text) renders the ten-entry log, newest first:
`102(x2) today 13:08; 68 ODU yesterday 14:33; ...` — fault code (with
occurrence count when >1), source when not the thermostat, day-relative date,
and time. The status byte also carries a high bit we have not fully
characterized; it is not rendered.

`active_fault` (binary) is deprecated: it warns at validation and publishes
nothing. It can be removed from your yaml and/or disabled in HA.

## Deprecated

- `compressor_frequency` sensor: alias of `odu_requested_cfm`. Warns at validation.
- `active_fault` binary sensor: publishes nothing.
- `address` hub key: alias for `sam_address`. Warns at validation.
