#include "infinitesp_cover.h"

namespace esphome {
namespace infinitesp {

cover::CoverTraits InfinitESPCover::get_traits() {
  cover::CoverTraits traits;
  traits.set_supports_position(true);
  traits.set_supports_stop(false);
  return traits;
}

void InfinitESPCover::control(const cover::CoverCall &call) {
  // HA command. ESPHome maps open→1.0, close→0.0, slider→value, all as a
  // position. The cover never writes the bus: fire the same trigger the bus
  // path uses and publish so HA acknowledges. HA-set positions are transient —
  // the next bus-commanded change overwrites them.
  const auto &pos = call.get_position();
  if (!pos.has_value())
    return;
  // Quantize to the protocol's 16 steps so a re-command of the current step
  // (e.g. HA slider at 0.53 while the cover sits at step 8 = 0.533…) does not
  // spuriously fire. Anchor: last_step_ (0xFF sentinel fires the first time).
  uint8_t new_step = (uint8_t) lroundf(*pos * 15.0f);
  if (new_step == last_step_)
    return;  // same step we last acted on — no change, no fire, no publish
  last_step_ = new_step;
  this->position = new_step / 15.0f;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->change_trigger_.trigger(this->position);
  this->publish_state();
}

void InfinitESPCover::on_register_update(uint8_t device_addr, uint16_t register_key) {
  // Damper positions arrive as a 4-byte WRITE to 0308 (one byte per zone,
  // zones 1-4) and are mirrored to 0319. Both keys notify; react to either.
  if (register_key != REG_ZC_DAMPER_CMD && register_key != REG_ZC_ZONE_CONFIG)
    return;
  if (zone_ < 1 || zone_ > 4)
    return;

  auto *data = parent_->get_register(device_addr, REG_ZC_DAMPER_CMD);
  if (!data || data->size() < 4)
    return;

  // The damper byte is itself the step (0x00-0x0F). Compare in step space so
  // the 0308/0319 mirror (same value, two notifies) and the thermostat's
  // periodic re-asserts fire exactly once per real change, and so an HA-set
  // transient (control()) snapping back to the bus value still fires when the
  // steps actually differ. Anchor: last_step_ (0xFF sentinel fires the first
  // time, so a zone fully-open at boot still actuates).
  uint8_t new_step = data->at(zone_ - 1);
  if (new_step == last_step_)
    return;  // same step we last acted on — nothing to do
  last_step_ = new_step;
  this->position = new_step / 15.0f;
  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->change_trigger_.trigger(this->position);
  this->publish_state();
}

}  // namespace infinitesp
}  // namespace esphome
