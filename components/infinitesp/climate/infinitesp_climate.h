#pragma once
#include "esphome/components/climate/climate.h"
#include <cmath>
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

// Custom preset names
static const char *const PRESET_WAKE = "Wake";
static const char *const PRESET_HOLD_1H = "Hold 1h";
static const char *const PRESET_HOLD_2H = "Hold 2h";
static const char *const PRESET_HOLD_4H = "Hold 4h";
static const char *const PRESET_HOLD_8H = "Hold 8h";

// Map from comfort profile activity index to HA preset for readback.
// Since the bus doesn't carry activity info, we track what WE set.
// When hold_duration changes from bus data, we infer:
//   0 → NONE,  65535 → HOME,  other → keep current
static const uint8_t NO_ACTIVITY = 0xFF;

class InfinitESPClimate : public climate::Climate, public InfinitESPDevice {
 public:
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;
  virtual void on_register_update(uint8_t device_addr, uint16_t register_key) override;

 protected:
  float current_temp_{NAN};
  uint8_t current_action_{0};
  uint8_t heat_sp_{68};
  uint8_t cool_sp_{76};
  uint8_t fan_mode_{0};
  uint8_t sys_mode_{4}; // off
  uint16_t hold_duration_{0};  // last known hold_duration for this zone
  uint8_t last_activity_{NO_ACTIVITY};  // last activity we applied (for readback)
};

} // namespace infinitesp
} // namespace esphome
