#pragma once
#include "esphome/components/climate/climate.h"
#include <cmath>
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

// Custom preset names
static const char *const PRESET_WAKE = "Wake";
static const char *const PRESET_HOLD_PERM = "Hold Indefinitely";
static const char *const PRESET_HOLD_TIMED = "Hold Timer";
static const char *const PRESET_SCHEDULE = "Per Schedule";

// Map from comfort profile activity index to HA preset for readback.
// Since the bus doesn't carry activity info, we track what WE set.
// When hold_duration changes from bus data, we infer:
//   0 → NONE,  0xFFFF → HOME,  other → keep current
static const uint8_t NO_ACTIVITY = 0xFF;

// How long to suppress stale poll data after a setpoint write (ms)
static const uint32_t PENDING_SETPOINT_WINDOW_MS = 8000;
// How long to hold a COMMANDED system mode against a lagging bus frame (ms).
// The bus confirm lags 1-2 poll cycles; during that window a stale
// AUTO-direction nibble (stage>0 on variable-speed gear) would otherwise
// revert a just-commanded heat/cool via the mode-trust branch.
static const uint32_t PENDING_MODE_WINDOW_MS = 8000;

class InfinitESPClimate : public climate::Climate, public InfinitESPEntity {
 public:
  void control(const climate::ClimateCall &call) override;
  climate::ClimateTraits traits() override;
  virtual void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void on_system_mode_commanded(uint8_t sys) override;

  void set_pending_setpoint_(uint8_t heat, uint8_t cool);
  // Recompute climate action from cached stage/mode + current damper state.
  // Returns true if this->action changed.
  bool compute_action_();

  // Hold state readback (for text_sensor display)
  uint16_t get_hold_duration() const { return hold_duration_; }
  const std::string &get_hold_end_time() const { return hold_end_time_; }

 protected:
  float current_temp_{NAN};
  climate::ClimateAction current_action_{climate::CLIMATE_ACTION_OFF};
  uint8_t last_stage_{0};   // last stage nibble from 3B02 stagmode
  uint8_t last_mode_{0};    // last mode nibble from 3B02 stagmode (direction when stage>0)
  uint8_t heat_sp_{68};
  uint8_t cool_sp_{76};
  uint8_t fan_mode_{0};
  uint8_t sys_mode_{0xFF}; // 0xFF = unknown/first boot
  uint16_t hold_duration_{0};  // last known hold_duration for this zone
  uint8_t last_activity_{NO_ACTIVITY};  // last activity we applied (for readback)
  std::string hold_end_time_;  // "HH:MM AP" or "Permanent" for text_sensor

  // Pending setpoint overlay — suppresses stale poll data after a write
  uint32_t pending_until_ms_{0};   // millis() deadline: ignore bus setpoints until this time
  uint8_t pending_heat_{0};        // the setpoint we just wrote
  uint8_t pending_cool_{0};        // the setpoint we just wrote
  bool pending_active_{false};     // whether we have a pending overlay

  // Pending system-mode overlay — holds a commanded mode until the bus confirms,
  // so a stale AUTO-direction nibble can't revert it. See PENDING_MODE_WINDOW_MS.
  uint32_t pending_mode_until_ms_{0};
  uint8_t pending_mode_{0};
  bool pending_mode_active_{false};
};

} // namespace infinitesp
} // namespace esphome
