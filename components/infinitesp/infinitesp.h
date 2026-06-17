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
static const uint16_t REG_ZC_ZONE_STATUS = 0x0302;  // Zone sensor readings (24 bytes, TLV)
static const uint16_t REG_ZC_DAMPER_CMD = 0x0308;   // Damper positions (write from tstat)
static const uint16_t REG_ZC_ZONE_CONFIG = 0x0319;  // Damper state feedback (8 bytes)
static const uint16_t REG_ZC_PRESENCE = 0x3405;      // Presence probe (discovery)
static const uint16_t REG_ZC_HEARTBEAT = 0x3404;     // Write/heartbeat flag (1 byte)
static const uint16_t REG_ZC_CYCLES = 0x0310;       // Cycle counters (12 bytes, 3 KV entries)
static const uint16_t REG_ZC_RUNTIME = 0x0311;      // Runtime hours (12 bytes, 3 KV entries)

// ZC zone sensor encoding: uint16 BE, °F = value / 16
// e.g. 0x047E = 1150 → 71.875°F. Range limited only by uint16 (4095°F).
static const float ZC_TEMP_SCALE = 16.0f;

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

// IDU (Indoor Unit / Furnace / Air Handler) register keys
// These are passively snooped from thermostat↔IDU traffic.
static const uint16_t REG_IDU_STATUS = 0x0306;     // Blower RPM, operating info (10 bytes)
static const uint16_t REG_IDU_CONFIG = 0x0316;      // Airflow CFM, electric heat (14 bytes)
static const uint16_t REG_IDU_CYCLES = 0x0310;     // Cycle counters (4-byte key-value entries)
static const uint16_t REG_IDU_RUNTIME = 0x0311;    // Runtime hours (4-byte key-value entries)

// ODU (Outdoor Unit / Heat Pump) register keys
// Passively snooped from thermostat↔ODU traffic.
static const uint16_t REG_ODU_STATUS1 = 0x0302;    // Temperatures and operating data
static const uint16_t REG_ODU_STATUS2 = 0x0303;    // Short status (compressor stage)
static const uint16_t REG_ODU_STATUS3 = 0x0304;    // Temperatures and pressures
static const uint16_t REG_ODU_COMP_SPEED = 0x0604;  // Compressor speed (uint16 pairs, first = current RPM)
static const uint16_t REG_ODU_DEMAND = 0x0608;     // Demand/stage indicator
static const uint16_t REG_ODU_SETPOINT = 0x060B;   // Target temperature setpoint (°F)
static const uint16_t REG_ODU_FLOATS = 0x061F;     // IEEE754 float32 array (superheat, subcooling, etc.)
static const uint16_t REG_ODU_CYCLES = 0x0310;     // Cycle counters (4-byte key-value entries)
static const uint16_t REG_ODU_RUNTIME = 0x0311;    // Runtime hours (4-byte key-value entries)

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
// True if this zone's damper is open (zone is receiving conditioned air).
  // Consults register 0308 under the ZC address: our emulated ZC (zc_address_)
  // when emulating, or the standard 0x60 when passively snooping a real
  // physical ZC. No damper data yet (no ZC on the bus, or not yet seen), or
  // zone index beyond the 4-byte register (zones 5-8): true — nothing to
  // suppress on, and single-zone systems track the system 1:1. Otherwise the
  // zone's damper byte (0x00-0x0F) is nonzero.
  bool zone_damper_open(uint8_t zone) const {
    uint8_t zc = zc_enabled() ? zc_address_ : ADDR_ZONE_CTRL;
    auto *data = get_register(zc, REG_ZC_DAMPER_CMD);
    if (!data || zone < 1 || zone > data->size())
      return true;
    return (*data)[zone - 1] != 0;
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
    if (zone >= 2 && zone <= 4) zc_zones_[zone].sensor_unit = is_f ? 2 : 1;
  }

  // Resolve sensor unit: explicit setting, or inherit from bus
  bool zc_sensor_is_fahrenheit_(uint8_t zone) const {
    if (zone < 2 || zone > 4) return false;
    switch (zc_zones_[zone].sensor_unit) {
      case 1: return false;  // explicit °C
      case 2: return true;   // explicit °F
      default: return !bus_uses_celsius();  // inherit from bus
    }
  }
  void set_zc_staleness_timeout(uint8_t zone, uint32_t timeout_ms);

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
  // ODU register 0604 (REG_ODU_COMP_SPEED): current compressor RPM, u16 BE at [0..1]
  static float odu_compressor_rpm_(const std::vector<uint8_t> &data) {
    if (data.size() < 2) return NAN;
    return (float) (((uint16_t) data[0] << 8) | data[1]);
  }
  // ODU register 0608 (REG_ODU_DEMAND): demand / stage / modulation
  static float odu_demand_(const std::vector<uint8_t> &data) {
    return data.size() >= 7 ? (float) data[3] : NAN;
  }
  static float odu_stage_(const std::vector<uint8_t> &data) {
    return data.size() >= 7 ? (float) data[5] : NAN;
  }
  static float odu_modulation_(const std::vector<uint8_t> &data) {
    return data.size() >= 7 ? (float) data[6] : NAN;
  }
  // ODU register 060B (REG_ODU_SETPOINT): target temp at data[4], native °F whole degrees
  static float odu_setpoint_f_(const std::vector<uint8_t> &data) {
    return data.size() >= 5 ? (float) data[4] : NAN;
  }
  // ODU register 0304 (REG_ODU_STATUS3): operating mode at data[10]
  static float odu_operating_mode_(const std::vector<uint8_t> &data) {
    return data.size() >= 11 ? (float) data[10] : NAN;
  }
  // ODU register 061F (REG_ODU_FLOATS): float idx 1..6 at offset 1 + (idx-1)*4.
  // idx 1..5 are °F deltas, idx 6 is dimensionless — caller applies conversion.
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

  void poll_thermostat_();
  void initialize_defaults_();
  void update_zc_zone_temp_(uint8_t zone, float temp_f);
  void check_zc_sensor_fallback_();
  void on_zc_sensor_update_(uint8_t zone, float value);

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

  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> rx_hex_log_;
  InfinitESPFrame current_frame_;
  std::vector<InfinitESPEntity *> entities_;
  uint8_t sam_address_{ADDR_FAKESAM};
  uint8_t zc_address_{0};  // 0 = zone controller emulation disabled
  ZCZoneConfig zc_zones_[5];  // index 0=unused, 1-4=zones (only 2-4 have sensors)
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

  // RS485 transmit enable pin (optional)
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  GPIOPin *flow_control_pin_{nullptr};
#endif
};

}  // namespace infinitesp
}  // namespace esphome
