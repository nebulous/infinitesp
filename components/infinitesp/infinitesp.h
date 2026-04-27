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
#include <string>
#include <map>
#include <vector>

namespace esphome {
namespace infinitesp {

// Address constants
static const uint8_t ADDR_DISCOVERY = 0x1F;
static const uint8_t ADDR_THERMOSTAT = 0x20;
static const uint8_t ADDR_INDOOR_UNIT = 0x40;
static const uint8_t ADDR_OUTDOOR_UNIT = 0x50;
static const uint8_t ADDR_SAM = 0x92;
static const uint8_t ADDR_FAKESAM = 0x93;
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

// Register 3B02 layout offsets (see AGENTS.md for full layout)
static const uint8_t REG3B02_ACTIVE_ZONES = 0;
static const uint8_t REG3B02_TEMPS = 3;             // zone temperatures[8], °F
static const uint8_t REG3B02_HUMIDITY = 11;          // zone humidity[8], %
static const uint8_t REG3B02_OUTDOOR_TEMP = 20;      // outdoor air temp °F
static const uint8_t REG3B02_UNOCCUPIED = 21;        // zones_unoccupied bitmask
static const uint8_t REG3B02_STAGMODE = 22;          // high nibble=stage, low nibble=mode
static const uint8_t REG3B02_WEEKDAY = 25;
static const uint8_t REG3B02_MINUTES = 26;           // uint16 LE, minutes since midnight
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

// ODU (Outdoor Unit / Heat Pump) register keys
// Passively snooped from thermostat↔ODU traffic.
static const uint16_t REG_ODU_STATUS1 = 0x0302;    // Temperatures and operating data
static const uint16_t REG_ODU_STATUS2 = 0x0303;    // Short status (compressor stage)
static const uint16_t REG_ODU_STATUS3 = 0x0304;    // Temperatures and pressures
static const uint16_t REG_ODU_COMP_SPEED = 0x0604;  // Compressor speed (uint16 pairs, first = current RPM)
static const uint16_t REG_ODU_DEMAND = 0x0608;     // Demand/stage indicator
static const uint16_t REG_ODU_SETPOINT = 0x060B;   // Target temperature setpoint (°F)
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

class InfinitESPComponent;

class InfinitESPDevice {
 public:
  virtual void on_register_update(uint8_t device_addr, uint16_t register_key) = 0;
  void set_parent(InfinitESPComponent *parent) { parent_ = parent; }
  void set_zone(uint8_t zone) { zone_ = zone; }
  uint8_t get_zone() const { return zone_; }

 protected:
  InfinitESPComponent *parent_{nullptr};
  uint8_t zone_{0};
};

class InfinitESPComponent : public Component, public uart::UARTDevice {
 public:
  InfinitESPComponent() = default;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_address(uint8_t addr) { address_ = addr; }
  uint8_t get_address() const { return address_; }
  void register_device(InfinitESPDevice *device) { devices_.push_back(device); }

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

  // Apply a comfort profile activity: writes setpoints+fan from 400A and sets hold
  void apply_activity(uint8_t zone, uint8_t activity_index, uint16_t hold_duration);

  // One-shot poll of a specific thermostat register (for discovery / debugging)
  void poll_register(uint8_t table, uint8_t row);

  const std::vector<uint8_t> *get_register(uint8_t addr, uint16_t key) const;
  uint8_t get_zone_count() const;
  bool is_bus_online() const { return bus_online_; }

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

 protected:
  void parse_byte_(uint8_t byte);
  bool validate_frame_();
  void dispatch_frame_();
  void handle_passive_frame_();

  void transmit_frame_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus, uint8_t func,
                       const std::vector<uint8_t> &payload);
  void send_frame_(uint8_t dst, uint8_t dst_bus, uint8_t func, const std::vector<uint8_t> &payload);
  void send_reply_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus, const std::vector<uint8_t> &payload);
  void send_exception_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus, uint8_t table, uint8_t row, uint8_t code);

  void handle_read_request_();
  void handle_write_request_();
  void handle_reply_();

  void poll_thermostat_();
  void initialize_defaults_();
  void store_register_(uint8_t addr, uint16_t key, const std::vector<uint8_t> &data);
  void notify_devices_(uint8_t device_addr, uint16_t register_key);

  uint16_t compute_crc_(const uint8_t *data, uint16_t len) const;

  // Per-device register storage: address → (register_key → data)
  std::map<uint8_t, std::map<uint16_t, std::vector<uint8_t>>> device_registers_;

  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> rx_hex_log_;
  InfinitESPFrame current_frame_;
  std::vector<InfinitESPDevice *> devices_;
  uint8_t address_{ADDR_FAKESAM};
  uint32_t last_rx_time_{0};
  uint32_t last_poll_time_{0};
  uint8_t poll_index_{0};
  uint8_t slow_poll_index_{0};
  uint32_t last_slow_poll_time_{0};
  bool bus_online_{false};
  uint32_t last_reply_time_{0};

  // Diagnostics
  uint32_t diag_total_rx_bytes_{0};
  uint32_t diag_total_tx_bytes_{0};
  uint32_t diag_frames_parsed_{0};
  uint32_t diag_crc_fail_{0};
  uint32_t diag_stale_discard_{0};
  uint32_t diag_last_stats_time_{0};
  bool diag_in_tx_{false};

  // Echo suppression: RS485 transceivers echo TX bytes back to RX.
  // After transmitting, we drain echo bytes from the RX buffer.
  uint32_t last_tx_done_time_{0};   // millis() when last TX completed
  uint32_t tx_echo_drain_count_{0};  // bytes drained as echo

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
};

}  // namespace infinitesp
}  // namespace esphome
