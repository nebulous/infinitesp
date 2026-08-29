#include "infinitesp_number.h"

#ifdef USE_NUMBER

#include <cmath>

namespace esphome {
namespace infinitesp {

// Set path: normalize (0 passes through — it means cancel, not a zero-minute
// hold), publish optimistically, enqueue the debounced write on the hub.
void InfinitESPNumber::control(float value) {
  float clamped = fminf(fmaxf(value, 0.0f), (float) InfinitESPComponent::HOLD_TIMED_MAX);
  uint16_t minutes = (uint16_t) (clamped + 0.5f);
  if (minutes > 0)
    minutes = InfinitESPComponent::normalize_timed_hold(minutes);
  ESP_LOGD("InfinitESP", "Zone %u hold minutes set %u (debounced)", zone_, minutes);
  parent_->queue_hold_set(zone_, minutes, 1500);
  this->readback_holdoff_ms_ = millis() + 10000;
  this->publish_state((float) minutes);
}

// Readback: remaining minutes while a timed hold runs, 0 otherwise
// (schedule-following or permanent — the datetime entity is the end-time
// view, this one is purely the countdown). Raw served value: during descent
// it is the true remaining (15 -> 14 -> ... ), never re-snapped.
void InfinitESPNumber::on_register_update(uint8_t device_addr, uint16_t register_key) {
  if (register_key != REG_SAM_ZONES)
    return;
  if (millis() < this->readback_holdoff_ms_)
    return;  // a set is settling; don't fight the UI value
  uint16_t dur = parent_->get_zone_hold_duration(zone_);
  float value = (dur == 0 || dur >= InfinitESPComponent::HOLD_PERMANENT) ? 0.0f : (float) dur;
  if (this->last_published_ == value)
    return;
  this->last_published_ = value;
  this->publish_state(value);
}

}  // namespace infinitesp
}  // namespace esphome

#endif  // USE_NUMBER
