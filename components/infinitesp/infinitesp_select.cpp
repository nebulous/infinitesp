#include "infinitesp_select.h"

namespace esphome {
namespace infinitesp {

// SYSMODE_NAMES (infinitesp.h) is index-aligned with the SYSMODE_*
// constants (HEAT=0, COOL=1, AUTO=2, EHEAT=3, HEATPUMP=4, OFF=5) and is the
// shared name table; guards below stay for safety.
static const uint8_t SYSTEM_MODE_COUNT = 6;
static const char *const FAN_MODES[] = {"auto", "low", "med", "high"};

// Displayed-zone options are "1".."8", index-aligned with the zone number
// (byte 28 value), so no name table is needed.
static const uint8_t DISPLAYED_ZONE_MIN = 1;
static const uint8_t DISPLAYED_ZONE_MAX = 8;

void InfinitESPSelect::control(const std::string &value) {
  if (select_type_ == "system_mode") {
    // Map string back to SYSMODE_* constant
    uint8_t mode = SYSMODE_OFF;  // default
    for (uint8_t i = 0; i < SYSTEM_MODE_COUNT; i++) {
      if (SYSMODE_NAMES[i] && value == SYSMODE_NAMES[i]) {
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
  } else if (select_type_ == "displayed_zone") {
    long zone = strtol(value.c_str(), nullptr, 10);
    if (zone < DISPLAYED_ZONE_MIN || zone > DISPLAYED_ZONE_MAX)
      return;  // not one of our options; publish nothing
    parent_->set_displayed_zone((uint8_t) zone);
    current_mode_ = (uint8_t) zone;
  }
  publish_state(value);
}

void InfinitESPSelect::on_register_update(uint8_t device_addr, uint16_t register_key) {
  if (select_type_ == "system_mode" && register_key == REG_SAM_STATE) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (data && data->size() >= REG3B02_STAGMODE + 1) {
      uint8_t stagmode = data->at(REG3B02_STAGMODE);
      uint8_t mode = stagmode & 0x0F;
      if (mode != current_mode_ && mode < SYSTEM_MODE_COUNT && SYSMODE_NAMES[mode]) {
        current_mode_ = mode;
        publish_state(SYSMODE_NAMES[mode]);
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
  } else if (select_type_ == "displayed_zone" && register_key == REG_SAM_STATE) {
    // The mirror holds the commanded value until the next tstat data, so a
    // non-adopting wall control reverts the display here on its next poll
    // (~6 s). No holdoff needed: the mirror update after our own write is a
    // no-op (current_mode_ already matches).
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (data && data->size() > REG3B02_DISPLAYED_ZONE) {
      uint8_t disp = data->at(REG3B02_DISPLAYED_ZONE);
      if (disp != current_mode_ && disp >= DISPLAYED_ZONE_MIN && disp <= DISPLAYED_ZONE_MAX) {
        current_mode_ = disp;
        publish_state(std::to_string(disp));
      }
    }
  }
}

} // namespace infinitesp
} // namespace esphome
