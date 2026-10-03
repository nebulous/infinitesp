#pragma once

#include "esphome/components/button/button.h"
#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

// Manual clock-sync button (clock phase 3, issue #45). Pressing it runs the
// shared sync core; the button itself is fire-and-forget — the diagnostic
// clock_sync text sensor carries the outcome. No state to mirror, so
// on_register_update is a no-op (the base class requires the override).
class InfinitESPButton : public button::Button, public InfinitESPEntity {
 public:
  void press_action() override {
    std::string detail;
    parent_->sync_clock_from_source(detail);
  }
  void on_register_update(uint8_t device_addr, uint16_t register_key) override {}
};

}  // namespace infinitesp
}  // namespace esphome
