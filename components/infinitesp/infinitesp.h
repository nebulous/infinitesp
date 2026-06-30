#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/gpio.h"
#include "esphome/components/uart/uart.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_INFINITESP_STATUS_LIGHT
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#endif
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <map>
#include <vector>

// Temperature unit configuration
enum class TemperatureUnit : uint8_t {
  AUTO = 0,  // heuristic: zone temp <= 50 → °C
  FAHRENHEIT,
  CELSIUS,
};

namespace esphome {
namespace sensor {
class Sensor;
}  // namespace sensor
namespace infinitesp {

// Address constants
// Thermostat install-discovery source address. Used by the thermostat itself
// during initial system discovery on install. Documentary only — InfinitESP
// never transmits as 0x1F. Kept distinct from FAKESAM to avoid conflating the
// thermostat's discovery concept with our table-name probing.
static const uint8_t ADDR_DISCOVERY = 0x1F;
static const uint8_t ADDR_THERMOSTAT = 0x20;
static const uint8_t ADDR_INDOOR_UNIT = 0x40;
static const uint8_t ADDR_OUTDOOR_UNIT = 0x50;
static const uint8_t ADDR_SAM = 0x92;
static const uint8_t ADDR_FAKESAM = 0x93;
static const uint8_t ADDR_ZONE_CTRL = 0x60;
static const uint8_t ADDR_BROADCAST = 0xF1;

// Function codes
static const uint8_t FUNC_READ = 0x0B;
static const uint8_t FUNC_REPLY = 0x06;
static const uint8_t FUNC_WRITE = 0x0C;
static const uint8_t FUNC_EXCEPTION = 0x15;

// Register keys (table<<8 | row)
static const uint16_t REG_DEVICE_INFO = 0x0104;
static const uint16_t REG_SAM_STATUS = 0x030D;
static const uint16_t REG_SAM_STATE = 0x3B02;
static const uint16_t REG_SAM_ZONES = 0x3B03;
static const uint16_t REG_SAM_VACATION = 0x3B04;
static const uint16_t REG_SAM_ACCESSORIES = 0x3B05;
static const uint16_t REG_SAM_DEALER = 0x3B06;
static const uint16_t REG_SAM_ACTIVITY = 0x3B0E;

// Thermostat 0x4xxx registers (polled from thermostat at 0x20)
// These are thermostat-internal configuration tables, not SAM registers.
static const uint16_t REG_TSTAT_SCHEDULE = 0x4002;      // Zone 1 schedule (35 bytes)
static const uint16_t REG_TSTAT_COMFORT = 0x400A;       // Zone 1 comfort profiles: 5 activities × 7 bytes (35 bytes)
static const uint16_t REG_TSTAT_VACATION = 0x4012;      // Zone 1 vacation settings (7 bytes)
static const uint16_t REG_TSTAT_WIFI = 0x4608;          // SSID, password, hostname (~216 bytes)
static const uint16_t REG_TSTAT_CLOUD = 0x4609;         // Cloud host, proxy server IP
static const uint16_t REG_TSTAT_DEALER = 0x460A;        // Dealer name, brand, URL (120 bytes)
static const uint16_t REG_TSTAT_FAULTS = 0x4202;        // Fault history (10 entries × 7 bytes = 70 bytes)
static const uint16_t REG_TSTAT_WIFI_PROFILES = 0x460B; // WiFi profiles (4x 36 bytes)
static const uint16_t REG_TSTAT_WIFI_SCAN = 0x460C;     // WiFi scan results (4x 36 bytes)

// Comfort profile layout (register 400A, 35 bytes)
// 5 activities × 7 bytes: [heat_sp(1), cool_sp(1), fan_mode(1), rclg_rhtg(1), hum_vent(1), unk5(1), unk6(1)]
//   byte[3] = (rhtg << 4) | rclg — reheat heating/cooling dehumidify setpoint indices (nibbles)
//   byte[4] = humidifier/ventilation mode flags (bitfield, exact mapping TBD)
//   byte[5-6] = purpose unclear (always 0x1E on this system)
// Order: home, away, sleep, wake, manual
static const uint8_t COMFORT_ACTIVITY_COUNT = 5;
static const uint8_t COMFORT_ENTRY_SIZE = 7;
static const uint8_t COMFORT_HOME = 0;
static const uint8_t COMFORT_AWAY = 1;
static const uint8_t COMFORT_SLEEP = 2;
static const uint8_t COMFORT_WAKE = 3;
static const uint8_t COMFORT_MANUAL = 4;

// Change flags for 3B03 writes
static const uint8_t CHANGE_FAN = 0x01;
static const uint8_t CHANGE_HOLD = 0x02;
static const uint8_t CHANGE_HEAT = 0x04;
static const uint8_t CHANGE_COOL = 0x08;
static const uint8_t CHANGE_MODE = 0x10;
static const uint8_t CHANGE_OVERRIDE = 0x80;

// System modes (stagmode low nibble)
static const uint8_t SYSMODE_HEAT = 0;
static const uint8_t SYSMODE_COOL = 1;
static const uint8_t SYSMODE_AUTO = 2;
static const uint8_t SYSMODE_EHEAT = 3;
static const uint8_t SYSMODE_OFF = 4;

// Fan modes
static const uint8_t FAN_AUTO = 0;
static const uint8_t FAN_LOW = 1;
static const uint8_t FAN_MED = 2;
static const uint8_t FAN_HIGH = 3;

// Zone Controller registers (device address 0x60)
static const uint16_t REG_ZC_ZONE_STATUS = 0x0302;  // Zone sensor readings (24-byte TLV, see layout below)
static const uint16_t REG_ZC_DAMPER_CMD = 0x0308;   // Damper positions (write from tstat)
static const uint16_t REG_ZC_ZONE_CONFIG = 0x0319;  // Damper state feedback (8 bytes)
static const uint16_t REG_ZC_PRESENCE = 0x3405;      // Presence probe (discovery)
static const uint16_t REG_ZC_HEARTBEAT = 0x3404;     // Write/heartbeat flag (1 byte)
static const uint16_t REG_ZC_CYCLES = 0x0310;       // Cycle counters (12 bytes, 3 KV entries)
static const uint16_t REG_ZC_RUNTIME = 0x0311;      // Runtime hours (12 bytes, 3 KV entries)

// ZC register 0302 (REG_ZC_ZONE_STATUS) — 24-byte TLV: six entries of
// [tag, id, val_hi, val_lo], always sent in id order:
//   0x01 z1, 0x02 z2, 0x03 z3, 0x04 z4, 0x14 LAT, 0x1C HPT.
// tag 0x01 = sensor present (value valid); 0x04 = not installed (0x0000).
// Value = uint16 BE, °F = value / 16. e.g. 0x047E → 1150 → 71.875°F.
// Verified by hooking a thermistor to the LAT/HPT ports and reading the
// thermostat's Furnace Status page (not (°F-64)×16 as once guessed).
static const uint8_t ZC_0302_TAG_PRESENT = 0x01;
static const uint8_t ZC_ID_LAT = 0x14;              // LAT — leaving air temperature thermistor
static const uint8_t ZC_ID_HPT = 0x1C;              // HPT thermistor port
static const float ZC_TEMP_SCALE = 16.0f;
// Sanity band for thermistor feeds (LAT/HPT). Supply air can exceed the
// 40-99°F indoor band used for zones; -40..250°F covers any real HVAC
// thermistor while still catching gross sensor_unit misconfigurations.
static const float ZC_THERMISTOR_MIN_F = -40.0f;
static const float ZC_THERMISTOR_MAX_F = 250.0f;

// Register 3B02 layout offsets (see AGENTS.md for full layout)
static const uint8_t REG3B02_ACTIVE_ZONES = 0;
static const uint8_t REG3B02_TEMPS = 3;             // zone temperatures[8], °F
static const uint8_t REG3B02_HUMIDITY = 11;          // zone humidity[8], %
static const uint8_t REG3B02_OUTDOOR_TEMP = 20;      // outdoor air temp °F
static const uint8_t REG3B02_UNOCCUPIED = 21;        // zones_unoccupied bitmask
static const uint8_t REG3B02_STAGMODE = 22;          // high nibble=stage, low nibble=mode
static const uint8_t REG3B02_WEEKDAY = 25;
static const uint8_t REG3B02_MINUTES = 26;           // uint16 BE, minutes since midnight
static const uint8_t REG3B02_DISPLAYED_ZONE = 28;
static const uint8_t REG3B02_SIZE = 29;

// Register 3B03 layout offsets (see AGENTS.md for full layout)
static const uint8_t REG3B03_ACTIVE_ZONES = 0;
static const uint8_t REG3B03_CHANGE_FLAGS = 2;
static const uint8_t REG3B03_FAN_MODES = 3;           // fan_mode[8]
static const uint8_t REG3B03_ZONES_HOLDING = 11;      // bitmask
static const uint8_t REG3B03_HEAT_SETPOINTS = 12;     // heat_sp[8], °F
static const uint8_t REG3B03_COOL_SETPOINTS = 20;     // cool_sp[8], °F
static const uint8_t REG3B03_HUMIDITY_SETPOINTS = 28;  // humidity_sp[8], %
static const uint8_t REG3B03_HOLD_DURATIONS = 38;     // hold_duration[8], uint16 BE each
static const uint8_t REG3B03_ZONE_NAMES = 54;         // zone_names[8], 12 chars each
static const uint8_t REG3B03_SIZE = 150;

// Register 3B04 (REG_SAM_VACATION) layout — vacation settings (11 bytes)
// NOTE on provenance: these offsets are inherited from Infinitude's CarBus::SAM
// 3B04 parser, which is our own reverse-engineering — NOT a Carrier source. Only
// the metric_units flag at byte 1 has live-test backing (it flips on an F/C
// toggle); the rest (min/max temp at 5/6, humidity at 7/8, fan at 9) is a current
// best guess, unconfirmed against real hardware. The vacation days/hours field
// lives somewhere in the padding region (bytes 2-4) but its exact offset/unit is
// unknown (Infinitude saw "hours region all 0x00" during its verification), so
// VACDAYS is not decoded.
static const uint8_t REG3B04_ACTIVE = 0;        // 0=off, nonzero=on
static const uint8_t REG3B04_METRIC_UNITS = 1;  // 0=English, 1=Metric (shared 0x3B flag)
static const uint8_t REG3B04_MIN_TEMP = 5;       // °F or °C per metric_units
static const uint8_t REG3B04_MAX_TEMP = 6;
static const uint8_t REG3B04_MIN_HUMIDITY = 7;   // 0 = NONE
static const uint8_t REG3B04_MAX_HUMIDITY = 8;   // 100 = NONE
static const uint8_t REG3B04_FAN_MODE = 9;       // 0=auto .. 3=high
static const uint8_t REG3B04_SIZE = 11;

// Register 3B05 (REG_SAM_ACCESSORIES) layout — accessory life & reminders (11 bytes)
// NOTE on provenance: inherited from Infinitude's CarBus::SAM 3B05 parser (our
// own RE, not a Carrier source). Which byte maps to which accessory (filter/UV/
// humidifier/ventilator, life vs. reminder) is unconfirmed vs. real hardware.
// Values are presumed to be consumption % (0 = new/reset, 100 = replace); the
// ASCII `FILTRLVL!0` reset presumably maps to writing 0 here. byte 1 is the
// shared metric_units flag (the one offset with live-test backing).
static const uint8_t REG3B05_FILTER = 3;          // filter life used %
static const uint8_t REG3B05_UV = 4;              // UV lamp life used %
static const uint8_t REG3B05_HUMIDIFIER = 5;      // humidifier pad life used %
static const uint8_t REG3B05_VENTILATOR = 6;      // ventilator filter life used %
static const uint8_t REG3B05_FILTER_RMD = 7;      // 0=off, 1=on
static const uint8_t REG3B05_UV_RMD = 8;
static const uint8_t REG3B05_HUMIDIFIER_RMD = 9;
static const uint8_t REG3B05_VENTILATOR_RMD = 10;
static const uint8_t REG3B05_SIZE = 11;

// Register 3B06 (REG_SAM_DEALER) layout — dealer info & config (52 bytes)
// NOTE on provenance: inherited from Infinitude's CarBus::SAM 3B06 parser (our
// own RE, not a Carrier source) after its 2026-06-26 restructure. The metric_units
// flag at byte 1 (mirror at byte 10) has live-test backing; the rest (deadband,
// cycles_per_hour, schedule_periods, programs_enabled, dealer_name/phone offsets)
// is unconfirmed vs. real hardware. byte 7 was previously guessed 'temp_units'
// but is observed 0xFF on Touch (the F/C ASCII codes live only on the RS-232 port,
// not in this register); the prior 'auto_mode' guess at byte 1 is incompatible
// with the unit flag, so CFGAUTO has no confirmed field and is not exposed.
static const uint8_t REG3B06_BACKLIGHT = 0;         // Touch: ON=level>=3, OFF=<=2
static const uint8_t REG3B06_METRIC_UNITS = 1;      // 0=English, 1=Metric (shared flag)
static const uint8_t REG3B06_DEADBAND = 3;          // 0-6
static const uint8_t REG3B06_CYCLES_PER_HOUR = 4;   // 2-6
static const uint8_t REG3B06_SCHEDULE_PERIODS = 5;  // 2 or 4
static const uint8_t REG3B06_PROGRAMS_ENABLED = 6;  // 0=off, 1=on
static const uint8_t REG3B06_DEALER_NAME = 12;      // 20 bytes (18 usable)
static const uint8_t REG3B06_DEALER_PHONE = 32;     // 20 bytes (18 usable)
static const uint8_t REG3B06_SIZE = 52;

// IDU (Indoor Unit / Furnace / Air Handler) register keys
// These are passively snooped from thermostat↔IDU traffic.
// IDU (Indoor Unit) register keys
// Passively snooped from thermostat<->IDU traffic.
// IDU tables (from device self-described 0xXX01 tabledefs):
//   0x01 DEVCONFG  device configuration (REG_DEVICE_INFO = 0x0104)
//   0x02 SYSTIME  system time/date (thermostat-owned)
//   0x03 RLCSMAIN main controller, RLCS (Residential & Light Commercial Systems) family - IDU-side (498 bytes, 30 rows)
//   0x04 VARSPEED variable-speed ECM blower drive (300 bytes, 17 rows)
// Tables 0x05-0x0F return FUNC 0x15 (not present on this hardware).
//
// Table 0x03 RLCSMAIN:
static const uint16_t REG_IDU_STATUS = 0x0306;     // Blower RPM, operating info (10 bytes)
static const uint16_t REG_IDU_CONFIG = 0x0316;      // Airflow CFM, electric heat (14 bytes)
static const uint16_t REG_IDU_CYCLES = 0x0310;     // Cycle counters (4-byte key-value entries)
static const uint16_t REG_IDU_RUNTIME = 0x0311;    // Runtime hours (4-byte key-value entries)

// ODU (Outdoor Unit) register keys
// Passively snooped from thermostat<->ODU traffic.
// ODU tables (from device self-described 0xXX01 tabledefs):
//   0x01 DEVCONFG  device configuration (REG_DEVICE_INFO = 0x0104)
//   0x02 SYSTIME  system time/date (thermostat-owned; ODU never reads it)
//   0x03 RLCSMAIN main controller, RLCS (Residential & Light Commercial Systems) family - ODU sensors & loop state
//   0x06 VAR COMP variable-speed compressor drive - frequency & stage
// Tables 0x04,0x05,0x07-0x0F return FUNC 0x15 (not present on this hardware).
//
// Table 0x03 RLCSMAIN:
static const uint16_t REG_ODU_STATUS1 = 0x0302;    // Temperatures and operating data
static const uint16_t REG_ODU_STATUS2 = 0x0303;    // Short status (4 bytes)
static const uint16_t REG_ODU_STATUS3 = 0x0304;    // Temperatures and pressures
static const uint16_t REG_ODU_CYCLES = 0x0310;     // Cycle counters (4-byte key-value entries)
static const uint16_t REG_ODU_RUNTIME = 0x0311;    // Runtime hours (4-byte key-value entries)
//
// Table 0x06 VAR COMP:
static const uint16_t REG_ODU_COMP_SPEED = 0x0604;  // Compressor speed: target RPM [0..1], current RPM [2..3] (per stage)
static const uint16_t REG_ODU_DEMAND = 0x0608;     // Compressor drive: frequency uint16 at [5..6] (0.1 Hz), expansion valve % at [2]
static const uint16_t REG_ODU_CMD_STAGE = 0x0605;  // Commanded compressor stage (float32 at [0..3]: 0.0/1.0..5.0)
static const uint16_t REG_ODU_STAGE_INFO = 0x060E;  // Actual stage index (byte 0: 0=off, 1..5=stage)
static const uint16_t REG_ODU_SETPOINT = 0x060B;   // Target value at byte[2], native °F (label TBD; not confirmed a cooling setpoint)
static const uint16_t REG_ODU_FLOATS = 0x061F;     // IEEE754 float32 array (superheat, subcooling, etc.)

// Frame constants
static const uint8_t FRAME_HEADER_SIZE = 8;
static const uint8_t FRAME_CRC_SIZE = 2;
static const uint16_t FRAME_MIN_SIZE = 10;
static const uint16_t FRAME_MAX_SIZE = 266;

struct InfinitESPFrame {
  uint8_t dst;
  uint8_t dst_bus;
  uint8_t src;
  uint8_t src_bus;
  uint8_t length;
  uint8_t pid;
  uint8_t ext;
  uint8_t func;
  std::vector<uint8_t> payload;
  uint16_t checksum;
};

// Extract a null-terminated C string from a byte vector at the given offset
static inline std::string extract_cstr(const std::vector<uint8_t> &data, size_t offset) {
  if (offset >= data.size())
    return "";
  size_t end = offset;
  while (end < data.size() && data[end] != 0)
    end++;
  return std::string(data.begin() + offset, data.begin() + end);
}

// Per-zone configuration for external temperature sensor references
struct ZCZoneConfig {
  sensor::Sensor *temp_sensor{nullptr};
  uint8_t sensor_unit{0};  // 0=inherit from bus, 1=°C, 2=°F
  uint32_t staleness_timeout_ms{120000};  // 2 minutes default
  uint32_t last_sensor_update_ms{0};
  float last_sensor_value{NAN};
};

class InfinitESPComponent;

class InfinitESPEntity {
 public:
  virtual void on_register_update(uint8_t device_addr, uint16_t register_key) = 0;
  void set_parent(InfinitESPComponent *parent) { parent_ = parent; }
  void set_zone(uint8_t zone) { zone_ = zone; }
  uint8_t get_zone() const { return zone_; }
  void set_bus_class(uint8_t cls) { bus_class_ = cls; }
  uint8_t get_bus_class() const { return bus_class_; }

 protected:
  InfinitESPComponent *parent_{nullptr};
  uint8_t zone_{0};
  uint8_t bus_class_{0};  // upper nibble of bus address: 0=any, 2=tstat, 4=IDU, 5=ODU, 9=SAM
};

class InfinitESPComponent : public Component, public uart::UARTDevice {
 public:
  InfinitESPComponent() = default;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_sam_address(uint8_t addr) { sam_address_ = addr; }
  uint8_t get_sam_address() const { return sam_address_; }
  bool sam_enabled() const { return sam_address_ != 0; }
  void set_zc_address(uint8_t addr) { zc_address_ = addr; }
  uint8_t get_zc_address() const { return zc_address_; }
  bool zc_enabled() const { return zc_address_ != 0; }

  // Multi-ZC mapping. A Carrier damper system uses one SYSTXCC4ZC01 per four
  // zones: the primary controller (base = zc_address_ when emulating, else
  // ADDR_ZONE_CTRL/0x60) serves system zones 1-4, and a second controller at
  // base+1 serves zones 5-8. Verified on hardware: two physical controllers
  // answer the thermostat at 0x60 and 0x61 (issue #9). Each controller numbers
  // its own four zones 1-4 (local id), so system zone N maps to a (controller,
  // local id, byte) triple.
  uint8_t zc_addr_for_zone_(uint8_t zone) const {
    uint8_t base = zc_enabled() ? zc_address_ : ADDR_ZONE_CTRL;
    return base + (zone >= 1 ? (zone - 1) / 4 : 0);
  }
  // Local zone id (1-4) within that controller for system zone N.
  uint8_t zc_local_id_for_zone_(uint8_t zone) const {
    return ((zone >= 1 ? zone - 1 : 0) % 4) + 1;
  }
  // Byte offset (0-3) within a 4-zone register (0308 dampers, 0319 state) for zone N.
  uint8_t zc_byte_for_zone_(uint8_t zone) const {
    return (zone >= 1 ? zone - 1 : 0) % 4;
  }
  // True if addr is one of our emulated ZC addresses (primary 0x60, and
  // secondary 0x61 only when a zone >4 is configured with a sensor).
  bool is_emu_zc_addr_(uint8_t addr) const {
    if (!zc_enabled())
      return false;
    if (addr == zc_address_)
      return true;
    return addr == (uint8_t)(zc_address_ + 1) && zc_secondary_enabled_();
  }
  // Secondary ZC (0x61, zones 5-8) is only emulated when at least one of those
  // zones has a temperature_sensor wired. Emulating an empty 0x61 would cause
  // the thermostat to commission zones 5-8 during discovery even though no
  // zones live there.
  bool zc_secondary_enabled_() const {
    for (uint8_t z = 5; z <= 8; z++)
      if (zc_zones_[z].temp_sensor != nullptr)
        return true;
    return false;
  }

  // True if this zone's damper is open (zone is receiving conditioned air).
  // Reads register 0319 (per-controller damper state reply) from the
  // controller serving this zone. No data yet (no ZC seen for this zone's
  // controller): true — single-zone / unknown systems track the system 1:1,
  // and nothing should be suppressed. Otherwise the zone's damper byte
  // (0x00-0x0F) is nonzero.
  bool zone_damper_open(uint8_t zone) const {
    auto *data = get_register(zc_addr_for_zone_(zone), REG_ZC_ZONE_CONFIG);
    if (!data || zone < 1)
      return true;
    uint8_t idx = zc_byte_for_zone_(zone);
    if (idx >= data->size())
      return true;
    return (*data)[idx] != 0;
  }
  void set_temperature_unit(TemperatureUnit unit) { temperature_unit_ = unit; }
  TemperatureUnit get_temperature_unit() const { return temperature_unit_; }
  void register_entity(InfinitESPEntity *entity) { entities_.push_back(entity); }

  // Status LED configuration
#ifdef USE_INFINITESP_STATUS_LED_PIN
  void set_status_led_pin(GPIOPin *pin) { status_led_gpio_ = pin; }
#endif
#ifdef USE_INFINITESP_STATUS_LIGHT
  void set_status_light(light::LightState *light) { status_light_ = light; }
#endif

  void set_zone_setpoint(uint8_t zone, uint8_t heat_sp, uint8_t cool_sp);
  void set_zone_fan(uint8_t zone, uint8_t fan_mode);
  void set_zone_hold(uint8_t zone, uint16_t duration_minutes);
  void set_system_mode(uint8_t mode);

  // RS485 transmit enable pin
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  void set_flow_control_pin(GPIOPin *pin) { flow_control_pin_ = pin; }
#endif

  // Apply a comfort profile activity: writes setpoints+fan from 400A and sets hold
  void apply_activity(uint8_t zone, uint8_t activity_index, uint16_t hold_duration);

  // One-shot poll of a specific thermostat register (for discovery / debugging)
  void poll_register(uint8_t table, uint8_t row);

  // ZC zone temperature sensor references
  void set_zc_temperature_sensor(uint8_t zone, sensor::Sensor *s);
  void set_zc_sensor_is_fahrenheit(uint8_t zone, bool is_f) {
    if (zone >= 2 && zone <= 8) zc_zones_[zone].sensor_unit = is_f ? 2 : 1;
  }

  // Resolve sensor unit: explicit setting, or inherit from bus
  bool zc_sensor_is_fahrenheit_(uint8_t zone) const {
    if (zone < 2 || zone > 8) return false;
    return zc_unit_is_fahrenheit_(zc_zones_[zone]);
  }
  void set_zc_staleness_timeout(uint8_t zone, uint32_t timeout_ms);

  // ZC thermistor sensor references — register 0302 ids 0x14 (LAT) / 0x1C (HPT).
  // Emulation only: feeds an external ESPHome sensor into the ZC 0302 TLV as a
  // present (tag 0x01) reading. Unlike zones, supply-air thermistors have no
  // sane ambient fallback — when the sensor is stale/unavailable the entry
  // reverts to not-installed (tag 0x04) so the thermostat stops seeing it.
  void set_zc_lat_sensor(sensor::Sensor *s);
  void set_zc_hpt_sensor(sensor::Sensor *s);
  void set_zc_lat_is_fahrenheit(bool is_f) { zc_lat_.sensor_unit = is_f ? 2 : 1; }
  void set_zc_hpt_is_fahrenheit(bool is_f) { zc_hpt_.sensor_unit = is_f ? 2 : 1; }
  void set_zc_lat_staleness(uint32_t ms) { zc_lat_.staleness_timeout_ms = ms; }
  void set_zc_hpt_staleness(uint32_t ms) { zc_hpt_.staleness_timeout_ms = ms; }

  const std::vector<uint8_t> *get_register(uint8_t addr, uint16_t key) const;
  uint8_t get_zone_count() const;
  bool is_bus_online() const { return bus_online_; }
  bool has_real_state() const { return sam_state_received_; }

  // Bus unit detection: returns true if bus values are in °C.
  // In AUTO mode, uses heuristic (active zone temp <= 50 → °C).
  // In explicit F/C mode, returns the configured value.
  bool bus_uses_celsius() const;

  // Convert a bus temperature value to °C for HA display.
  // Bus is °F or °C depending on detected/configured mode.
  float bus_temp_to_celsius(float bus_value) const;

  // Convert a HA °C value to bus units (°F or °C depending on mode).
  float celsius_to_bus_temp(float celsius) const;

  // Convert a comfort profile raw byte to whole-degree °C for comparison/display.
  // In °F mode, comfort bytes are whole °F.
  // In °C mode, comfort bytes are half-degrees (byte/2 = °C).
  float comfort_byte_to_celsius(uint8_t raw) const;

  // Convert a whole-degree °C value to comfort profile raw byte.
  // Inverse of comfort_byte_to_celsius.
  uint8_t celsius_to_comfort_byte(float celsius) const;

  // Convert a whole-degree bus setpoint to °C for HA display.
  // Setpoints (3B03) are always whole degrees in both modes.
  float setpoint_to_celsius(uint8_t raw) const;

  // Convert a HA °C value to a whole-degree bus setpoint.
  uint8_t celsius_to_setpoint(float celsius) const;

  // Get normalized hold duration for a zone (1-indexed).
  // Carrier protocol uses zones_holding bit + duration<=1 to mean permanent hold;
  // this method normalizes that to 0xFFFF so callers see a consistent encoding.
  // Returns 0 = no hold, 0xFFFF (65535) = permanent, else minutes remaining.
  static constexpr uint16_t HOLD_PERMANENT = 0xFFFF;
  uint16_t get_zone_hold_duration(uint8_t zone) const;

  // Format a hold's end time as "HH:MM AP" from the current bus clock (3B02)
  // plus hold_minutes. Returns empty string if the bus clock isn't available yet.
  std::string format_hold_end(uint16_t hold_minutes) const;

  // Bus diagnostic report — streams directly via callback (no heap allocation)
  void stream_bus_report_(void (*write_fn)(const uint8_t *, size_t, void *), void *ctx);
  // Direct register manipulation (used by button platform for virtual registers)
  void store_register_(uint8_t addr, uint16_t key, const std::vector<uint8_t> &data);
  void notify_entities_(uint8_t device_addr, uint16_t register_key);
  // Mirror a register into the SAM's own address space (so SAM-served READs return
  // current values). Marks bus state received for REG_SAM_STATE / REG_SAM_ZONES.
  void mirror_to_sam_(uint16_t reg_key, const std::vector<uint8_t> &data);

  // Decode big-endian IEEE754 float32 from byte vector
  static float decode_f32_be_(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size())
      return NAN;
    // Reorder BE bytes to LE for ESP32
    uint8_t le[4] = {data[offset + 3], data[offset + 2], data[offset + 1], data[offset]};
    float f;
    memcpy(&f, le, sizeof(f));
    return f;
  }

  // Decode big-endian int16 / 16 from byte vector (temperature encoding)
  static float decode_int16_f_(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 2 > data.size())
      return NAN;
    int16_t raw = (int16_t) ((uint16_t) data[offset] << 8) | data[offset + 1];
    return (float) raw / 16.0f;
  }

  // --- IDU/ODU field accessors: single source of truth for register offsets ---
  // Pure decoders of a register's byte vector. Native units (°F for ODU temps,
  // raw counts for RPM/CFM, native float for 061F). Return NAN if the field
  // isn't fully present; callers apply unit conversion (°F→°C) and publish.
  // Both the dispatch logger and the sensor/binary_sensor platforms call these
  // so the offset table lives in exactly one place.

  // IDU register 0306 (REG_IDU_STATUS): blower RPM, u16 BE at [1..2]
  static float idu_blower_rpm_(const std::vector<uint8_t> &data) {
    if (data.size() < 3) return NAN;
    return (float) (((uint16_t) data[1] << 8) | data[2]);
  }
  // IDU register 0316 (REG_IDU_CONFIG): airflow CFM u16 BE at [4..5]
  static float idu_airflow_cfm_(const std::vector<uint8_t> &data) {
    if (data.size() < 6) return NAN;
    return (float) (((uint16_t) data[4] << 8) | data[5]);
  }
  // IDU register 0316: electric heat present, data[0] & 0x03
  static bool idu_electric_heat_(const std::vector<uint8_t> &data) {
    return !data.empty() && (data[0] & 0x03) != 0;
  }
  // ODU register 0604 (REG_ODU_COMP_SPEED): two uint16 BE pairs per stage.
  //   [0..1] = target (commanded) RPM  — holds round rated stage speeds
  //            {0,1500,1700,2460,2800,3650}
  //   [2..3] = actual (measured) RPM — fluctuates around target
  //            (~3·drive_hz with slip)
  // Verified via 0604-vs-060e stage crosstab and the v=0x0484 frame where
  // actual(3640) != target(2800). See DEVLOG 2026-06-21 and Infinitude
  // OutdoorUnit.pm 0604 (target_rpm / current_rpm — Infinitude uses 'current'
  // here but 'actual' elsewhere; InfinitESP unifies on 'actual').
  static float odu_compressor_target_rpm_(const std::vector<uint8_t> &data) {
    if (data.size() < 2) return NAN;
    return (float) (((uint16_t) data[0] << 8) | data[1]);
  }
  static float odu_compressor_actual_rpm_(const std::vector<uint8_t> &data) {
    if (data.size() < 4) return NAN;
    return (float) (((uint16_t) data[2] << 8) | data[3]);
  }
  // ODU register 0608 (REG_ODU_DEMAND): compressor drive frequency, u16 BE at [5..6], 0.1 Hz
  // Scale confirmed for stages 1-4 against Carrier rated RPM (4-pole motor, sync rpm = 3*v);
  // stage 5 (144 Hz) predicted, not yet measured. See private/DEVLOG.md 2026-06-23.
  static float odu_compressor_frequency_(const std::vector<uint8_t> &data) {
    if (data.size() < 7) return NAN;
    return (float) (((uint16_t) data[5] << 8) | data[6]) / 10.0f;
  }
  // ODU register 0608 (REG_ODU_DEMAND): expansion valve position at byte [2], 0-100 percent.
  // Proven from bus captures: ramps through intermediate values (39-95%) over 10-15s on
  // compressor start/stop transitions, settles at 100% while running and 0% while off.
  // The full-stroke travel time rules out a boolean status flag or a fan/load percent.
  // Discovered by feisley; confirmed across 8 transitions in the bus-logger archive.
  static float odu_expansion_valve_(const std::vector<uint8_t> &data) {
    return data.size() >= 3 ? (float) data[2] : NAN;
  }
  // ODU register 060e (REG_ODU_STAGE_INFO): variable-speed stage index at byte 0
  // {0=off, 1..5=stage}. Verified against rpm-derived stage; resolves the
  // frequency ambiguity (1500/1700 rpm both read stage 1).
  static float odu_stage_(const std::vector<uint8_t> &data) {
    return data.size() >= 1 ? (float) data[0] : NAN;
  }
  // ODU register 060B (REG_ODU_SETPOINT): variable target value at byte[2], native °F whole degrees.
  // Write-only (thermostat→ODU). Offset fix: was data[4] (always 0); data[2] carries the value.
  // Range 25-115°F, varies independently of OAT and zone setpoints. NOT confirmed to be a
  // cooling setpoint (the thermostat does not tell the ODU an indoor setpoint); likely a
  // refrigerant-loop coil/discharge control target. No sensor exposed - kept for future decode.
  static float odu_setpoint_f_(const std::vector<uint8_t> &data) {
    return data.size() >= 3 ? (float) data[2] : NAN;
  }
  // ODU register 0605 (REG_ODU_CMD_STAGE): commanded compressor stage, float32 BE at [0..3]
  // (0.0=off, 1.0..5.0=stage). Write-only; drives actual stage (060e) with ~15s lag.
  static float odu_commanded_stage_(const std::vector<uint8_t> &data) {
    return decode_f32_be_(data, 0);
  }
  // ODU register 0304 (REG_ODU_STATUS3): line voltage at data[7], whole volts.
  // Validated against Carrier cloud linevolt field: bus 0304[7]=238-240 vs cloud 239V.
  // State-independent (held tight ±1 LSB across idle and across user's wider observations).
  static float odu_line_voltage_(const std::vector<uint8_t> &data) {
    return data.size() >= 8 ? (float) data[7] : NAN;
  }
  // ODU register 0304 (REG_ODU_STATUS3): operating mode at data[10]
  static float odu_operating_mode_(const std::vector<uint8_t> &data) {
    return data.size() >= 11 ? (float) data[10] : NAN;
  }
  // ODU register 061F (REG_ODU_FLOATS): float idx 1..6 at offset 1 + (idx-1)*4.
  // idx 1..5 are °F deltas (ΔT, not absolute temps); idx 6 is dimensionless.
  // idx 5 is a discharge-related control delta (NOT discharge superheat - goes
  // negative ~75% while running; see Infinitude OutdoorUnit.pm 061F).
  // Caller converts ΔF→ΔC (×5/9) for idx 1..5; idx 6 passed through.
  static float odu_float_(const std::vector<uint8_t> &data, uint8_t idx) {
    return decode_f32_be_(data, 1 + (idx - 1) * 4);
  }
  // ODU register 0302 (REG_ODU_STATUS1): measurement slot idx 0..5 at offset 2+idx*4.
  //   idx 0=outdoor 1=coil 2=suction 3=subcooling(ΔT) 4=indoor_amb 5=discharge
  // Native °F via decode_int16_f_. idx 3 is a delta (caller skips the -32).
  static float odu_status1_meas_f_(const std::vector<uint8_t> &data, uint8_t idx) {
    return decode_int16_f_(data, 2 + idx * 4);
  }

 protected:
  void parse_byte_(uint8_t byte);
  bool validate_frame_();
  void dispatch_frame_();
  void handle_passive_frame_();

  void transmit_frame_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus, uint8_t func,
                       const std::vector<uint8_t> &payload);
  void send_frame_(uint8_t dst, uint8_t dst_bus, uint8_t func, const std::vector<uint8_t> &payload);
  // Send a WRITE frame now and queue one retransmit RETRANSMIT_DELAY_MS later.
  // Rides through sporadic bus drops without readback verification. All callers
  // write idempotent values (setpoints/fan/mode/permanent holds); timed holds
  // see a sub-minute countdown reset, below the field's minute resolution.
  // Coupling: RETRANSMIT_DELAY_MS must stay <= PENDING_SETPOINT_WINDOW_MS/2
  // (InfinitESPClimate) so a retransmit always lands inside the newest
  // change's overlay window and cannot cause UI snapback.
  void send_write_frame_(uint8_t dst, uint8_t dst_bus, const std::vector<uint8_t> &payload);
  void send_reply_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus, const std::vector<uint8_t> &payload);
  void send_exception_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus, uint8_t table, uint8_t row, uint8_t code);

  void handle_read_request_();
  void handle_write_request_();
  void handle_reply_();
  void handle_discovery_reply_();
  void handle_metric_units_reply_(uint8_t device_addr, uint16_t reg_key, const std::vector<uint8_t> &data);
  void poll_metric_units_();

  void poll_thermostat_();
  void poll_discovery_();
  void initialize_defaults_();
  void update_zc_zone_temp_(uint8_t zone, float temp_f);
  void write_zc_zone_temp_entry_(uint8_t zone, float temp_f, bool present);
  void check_zc_sensor_fallback_();
  void on_zc_sensor_update_(uint8_t zone, float value);
  void register_zc_thermistor_(ZCZoneConfig &slot, uint8_t tlv_id, sensor::Sensor *s);
  void on_zc_thermistor_update_(ZCZoneConfig &slot, uint8_t tlv_id, float value);
  bool zc_unit_is_fahrenheit_(const ZCZoneConfig &slot) const;
  // Write a ZC 0302 TLV entry by id: tag (0x01 present / 0x04 not-installed) and
  // uint16-BE value (temp_f * 16). Idempotent; stores + notifies on change.
  void write_zc_temp_entry_(uint8_t zc_addr, uint8_t tlv_id, float temp_f, bool present);
  // Mirror a 4-byte damper command (0308) into the 8-byte 0319 state register:
  // bytes 0-3 = damper positions, bytes 4-7 = 0xFF. Shared by the emulated-ZC
  // write path and the passive physical-ZC capture.
  void mirror_damper_to_0319_(uint8_t addr, const std::vector<uint8_t> &damper);

  // Bus traffic capture for diagnostics / protocol reverse engineering
  struct TrafficEntry {
    uint32_t count{0};
    std::vector<uint8_t> last_payload;
    uint32_t last_seen_ms{0};
  };
  // Key: (src << 24) | (dst << 16) | (func << 8) | (reg_key & 0xFF)
  // Uses reg_key low byte only to keep key in 32 bits — sufficient for traffic grouping
  // since the table byte is already encoded in reg_key's high byte, which we fold differently.
  // Actually: key = (src << 24) | (dst << 16) | (func << 8) | ((table ^ row) & 0xFF) is lossy.
  // Better: use uint64_t or a small struct key.
  struct TrafficKey {
    uint8_t src, dst, func;
    uint16_t reg_key;
    bool operator<(const TrafficKey &o) const {
      if (src != o.src) return src < o.src;
      if (dst != o.dst) return dst < o.dst;
      if (func != o.func) return func < o.func;
      return reg_key < o.reg_key;
    }
  };
  std::map<TrafficKey, TrafficEntry> traffic_log_;
  void log_traffic_(uint8_t src, uint8_t dst, uint8_t func, uint16_t reg_key,
                     const std::vector<uint8_t> &payload);

  // Recent write frame capture (for protocol reverse engineering via REPORT)
  static const size_t WRITE_CAPTURE_MAX = 32;
  struct WriteCapture {
    uint8_t src;
    uint8_t dst;
    uint16_t reg_key;
    std::vector<uint8_t> payload;
  };
  std::deque<WriteCapture> write_captures_;

  uint16_t compute_crc_(const uint8_t *data, uint16_t len) const;

  // Per-device register storage: address → (register_key → data)
  std::map<uint8_t, std::map<uint16_t, std::vector<uint8_t>>> device_registers_;

  // Table names learned by probing each observed device's 0xNN01 tabledef
  // register as ADDR_FAKESAM (0x93). Keyed by (device addr, table number).
  std::map<std::pair<uint8_t, uint8_t>, std::string> table_names_;
  std::map<std::pair<uint8_t, uint8_t>, uint32_t> discovery_query_ms_;  // last query ts (retry backoff)
  uint32_t last_discovery_poll_ms_{0};

  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> rx_hex_log_;
  InfinitESPFrame current_frame_;
  std::vector<InfinitESPEntity *> entities_;
  uint8_t sam_address_{ADDR_FAKESAM};
  uint8_t zc_address_{0};  // 0 = zone controller emulation disabled
  ZCZoneConfig zc_zones_[9];  // index 0=unused, 1-8=zones (2-8 may have external sensors)
  ZCZoneConfig zc_lat_;       // LAT thermistor (register 0302 id 0x14)
  ZCZoneConfig zc_hpt_;       // HPT thermistor (register 0302 id 0x1C)
  uint32_t last_zc_sensor_check_{0};
  uint32_t last_rx_time_{0};
  uint32_t last_poll_time_{0};
  uint8_t poll_index_{0};
  uint8_t slow_poll_index_{0};
  uint32_t last_slow_poll_time_{0};
  bool bus_online_{false};
  bool sam_state_received_{false};
  uint32_t last_reply_time_{0};

  // Diagnostics
  uint32_t diag_total_rx_bytes_{0};
  uint32_t diag_total_tx_bytes_{0};
  uint32_t diag_frames_parsed_{0};
  uint32_t diag_crc_fail_{0};
  uint32_t diag_stale_discard_{0};
  uint32_t diag_last_stats_time_{0};
  bool diag_in_tx_{false};



  // Frame-drop debugging
  uint32_t diag_rx_seq_{0};        // monotonically increasing RX frame sequence
  uint32_t diag_tx_seq_{0};        // monotonically increasing TX frame sequence
  uint32_t diag_uart_hwm_{0};      // UART RX buffer high-water mark (bytes available at loop entry)
  uint32_t diag_uart_overflow_events_{0};  // times available() > 75% of rx_buffer_size
  uint32_t diag_reply_expected_{0};   // total REPLY frames we expected (matched to our READs)
  uint32_t diag_reply_received_{0};   // total REPLY frames we actually received
  uint32_t diag_reply_timeout_{0};    // polls that timed out without a reply
  uint32_t diag_tx_flush_max_ms_{0};  // max time spent in flush()
  uint32_t diag_loop_max_ms_{0};      // max time spent in a single loop() iteration
  uint32_t diag_last_frame_time_{0};  // millis() of last complete frame
  uint32_t diag_inter_frame_min_ms_{UINT32_MAX};  // min inter-frame interval
  uint32_t diag_inter_frame_max_ms_{0};            // max inter-frame interval

  // Outstanding poll tracking: maps (dest_addr << 16 | reg_key) -> TX seq number
  struct PendingPoll {
    uint32_t tx_seq;
    uint32_t sent_ms;
    uint8_t dest;
    uint16_t reg_key;
  };
  std::vector<PendingPoll> pending_polls_;

  // Queued write retransmits. Each entry fires once from loop() at fire_ms.
  // Drained FIFO; with RETRANSMIT_DELAY_MS <= overlay/2, the last-sent value
  // for a zone always wins on the bus regardless of rapid change ordering.
  struct PendingRetransmit {
    uint8_t dst;
    uint8_t dst_bus;
    uint8_t func;   // FUNC_WRITE
    std::vector<uint8_t> payload;
    uint32_t fire_ms;
  };
  std::deque<PendingRetransmit> pending_retransmits_;
  // Cached WiFi credentials discovered from thermostat register 4608
  // Stored in NVS, injected into WiFi component on boot if WiFi hasn't connected yet
  struct CachedWifi {
    char ssid[33];     // null-terminated, max 32 chars + \0
    char password[65]; // null-terminated, max 64 chars + \0
  };
  ESPPreferenceObject wifi_pref_;
  std::string cached_wifi_ssid_;
  std::string cached_wifi_password_;
  bool wifi_cache_dirty_{false};
  bool wifi_injected_{false};  // one-shot: inject cached WiFi once after boot

  void cache_wifi_credentials_(const std::string &ssid, const std::string &password);
  void inject_cached_wifi_();

  uint32_t diag_poll_purged_{0};  // polls purged due to timeout

  // Status LED support (optional, configured via YAML)
  // Two mutually exclusive modes:
  //   USE_INFINITESP_STATUS_LED_PIN  — direct GPIO pin (simple on/off/brightness via LEDC)
  //   USE_INFINITESP_STATUS_LIGHT   — reference to an ESPHome light entity (RGB support)
#ifdef USE_INFINITESP_STATUS_LED_PIN
  GPIOPin *status_led_gpio_{nullptr};
  bool status_led_physical_{false};   // current physical state
  uint32_t status_led_last_toggle_{0}; // last blink toggle time
#endif
#ifdef USE_INFINITESP_STATUS_LIGHT
  light::LightState *status_light_{nullptr};
  bool status_light_has_rgb_{false};   // cached from traits check
  uint32_t status_light_last_update_{0};
#endif

  // Unified status LED state tracking (used by both modes)
  // Sequential narrative: yellow blink (bus not ready) → blue blink (wifi not ready) → green (all good)
  enum class StatusLedState : uint8_t {
    BUS_NOT_READY = 0,  // bus not fully online (yellow blink / mono blink)
    WIFI_NOT_READY,     // bus ok, wifi not connected (blue blink / mono fast blink)
    ALL_GOOD,           // both bus and wifi up (solid green / mono solid)
  };
  StatusLedState status_led_state_{StatusLedState::BUS_NOT_READY};

  void update_status_led_();

  // Temperature unit configuration and detected state
  TemperatureUnit temperature_unit_{TemperatureUnit::AUTO};
  bool bus_celsius_detected_{false};  // cached heuristic result (AUTO mode)
  bool bus_unit_detected_{false};     // true after first successful detection
  // Authoritative metric-units flag, read from the thermostat's 3B06 push
  // (when sam emulated — the tstat pushes 3B06 to 0x92, captured under addr
  // 0x20) or polled from 3B05 as FakeSAM (when sam NOT emulated). 3B05/3B06
  // data[1]: 0=English(°F), 1=Metric(°C). Verified live 2026-06-26.
  // metric_units_known_=false until first authoritative read → heuristic fallback.
  bool metric_units_known_{false};
  bool metric_units_{false};         // valid only when metric_units_known_
  uint32_t last_unit_poll_ms_{0};

  // RS485 transmit enable pin (optional)
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  GPIOPin *flow_control_pin_{nullptr};
#endif
};

}  // namespace infinitesp
}  // namespace esphome
