#include "infinitesp_select.h"

namespace esphome {
namespace infinitesp {

// Index must match SYSMODE_* constants: HEAT=0, COOL=1, AUTO=2, EHEAT=3, OFF=4
static const char *const SYSTEM_MODES[] = {"heat", "cool", "auto", "emergency_heat", "off"};
static const char *const FAN_MODES[] = {"auto", "low", "med", "high"};

void InfinitESPSelect::control(const std::string &value) {
  if (select_type_ == "system_mode") {
    // Map string back to SYSMODE_* constant
    uint8_t mode = SYSMODE_OFF;  // default
    for (uint8_t i = 0; i < 5; i++) {
      if (value == SYSTEM_MODES[i]) {
        mode = i;
        break;
      }
    }
    parent_->set_system_mode(mode);
    current_mode_ = mode;
  } else if (select_type_ == "fan_mode") {
    for (uint8_t i = 0; i < 4; i++) {
      if (value == FAN_MODES[i]) {
        parent_->set_zone_fan(zone_, i);
        current_mode_ = i;
        break;
      }
    }
  }
  publish_state(value);
}

void InfinitESPSelect::on_register_update(uint8_t device_addr, uint16_t register_key) {
  if (select_type_ == "system_mode" && register_key == REG_SAM_STATE) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (data && data->size() >= REG3B02_STAGMODE + 1) {
      uint8_t stagmode = data->at(REG3B02_STAGMODE);
      uint8_t mode = stagmode & 0x0F;
      if (mode != current_mode_ && mode <= 4) {
        current_mode_ = mode;
        publish_state(SYSTEM_MODES[mode]);
      }
    }
  } else if (select_type_ == "fan_mode" && register_key == REG_SAM_ZONES) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_ZONES);
    if (data && data->size() >= REG3B03_FAN_MODES + 8) {
      uint8_t idx = zone_ - 1;
      if (data->at(REG3B03_ACTIVE_ZONES) & (1 << idx)) {
        uint8_t fan = data->at(REG3B03_FAN_MODES + idx);
        if (fan != current_mode_ && fan <= 3) {
          current_mode_ = fan;
          publish_state(FAN_MODES[fan]);
        }
      }
    }
  }
}

} // namespace infinitesp
} // namespace esphome
