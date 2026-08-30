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
| `sam_address` | `0x92` | Bus address for SAM emulation. `0` disables. |
| `address` | - | Deprecated alias for `sam_address`. |
| `zone_controller_address` | `0` | `0x60` emulates a zone controller. `0` passively monitors a real one. |
| `temperature_unit` | `auto` | `auto` (bus heuristic), `F`, or `C`. The thermostat's unit setting is authoritative once read. |
| `auto_diagnostics` | `true` | `false` drops the generated diagnostics group (below). |
| `status_light_id` | - | Existing light entity for status. Mutually exclusive with `status_led_pin`. |
| `status_led_pin` | - | GPIO for a simple status LED. |
| `flow_control_pin` | - | RS485 transmit-enable (DE/RE) GPIO. |

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

## Select types

| Type | Zone | Generation | Options |
|---|---|---|---|
| `system_mode` | - | core | heat, cool, auto, emergency_heat, off |
| `fan_mode` | yes | per-zone | auto, low, med, high |

## Covers

Damper cover, one per zone. `zone` required. `on_change` trigger fires with the new
position (0.0-1.0) when the thermostat commands a damper move. Without a trigger the
cover is a position reporter. Generated for every declared zone. With no zone controller
on the bus it stays unknown.

## Time and number entities

Both per zone, generated:

- Hold until (`time` entity): clock time the hold ends at.
- Hold minutes (`number`, 0-1425 in steps of 15): remaining or to-arm minutes. 0 cancels.

## Deprecated

- `compressor_frequency` sensor: alias of `odu_requested_cfm`. Warns at validation.
- `active_fault` binary sensor: publishes nothing.
- `address` hub key: alias of `sam_address`. Warns at validation.
