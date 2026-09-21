#include "infinitesp_number.h"

#ifdef USE_NUMBER

#include <cmath>

namespace esphome {
namespace infinitesp {

// Set path, hold flavor: normalize (0 passes through — it means cancel, not a
// zero-minute hold), publish optimistically, enqueue the debounced write on
// the hub. last_published_ follows the optimistic publish so the readback
// dedupe never mistakes the pre-command value for "no change" and suppresses
// the revert.
//
// Set path, vacation flavor: clamp to the 8760-h UI/SAM ceiling and hand the
// raw hours to the hub setter (guarded there by sam_enabled()). No debounce —
// discrete 1-h steps. On a passive install the setter no-ops, so republish
// the hub member instead of the commanded value: there is no readback path
// that would revert a lie, so don't tell it.
void InfinitESPNumber::control(float value) {
  if (number_type_ == NUMBER_VACATION_HOURS) {
    float clamped = fminf(fmaxf(value, 0.0f), 8760.0f);
    uint16_t hours = (uint16_t)(clamped + 0.5f);
    ESP_LOGD("InfinitESP", "Vacation hours set %u", hours);
    parent_->set_vacation_hours(hours);
    float publish = (float) parent_->get_vacation_hours();
    this->last_published_ = publish;
    this->publish_state(publish);
    return;
  }

  float clamped = fminf(fmaxf(value, 0.0f), (float) InfinitESPComponent::HOLD_TIMED_MAX);
  uint16_t minutes = (uint16_t)(clamped + 0.5f);
  if (minutes > 0)
    minutes = InfinitESPComponent::normalize_timed_hold(minutes);
  ESP_LOGD("InfinitESP", "Zone %u hold minutes set %u (debounced)", zone_, minutes);
  parent_->queue_hold_set(zone_, minutes, 1500);
  this->readback_holdoff_ms_ = millis() + 10000;
  this->last_published_ = (float) minutes;
  this->publish_state((float) minutes);
}

// Readback, hold flavor: remaining minutes while a timed hold runs, 0
// otherwise (schedule-following or permanent — the datetime entity is the
// end-time view, this one is purely the countdown). Raw served value: during
// descent it is the true remaining (15 -> 14 -> ...), never re-snapped.
//
// Readback, vacation flavor: the hub member (last commanded hours). 4012
// carries config only and never changes with vacation state, so the notify
// just re-asserts the member after boot / passive transitions.
void InfinitESPNumber::on_register_update(uint8_t device_addr, uint16_t register_key) {
  if (number_type_ == NUMBER_VACATION_HOURS) {
    if (register_key != REG_TSTAT_VACATION)
      return;
    float value = (float) parent_->get_vacation_hours();
    if (this->last_published_ == value)
      return;
    this->last_published_ = value;
    this->publish_state(value);
    return;
  }

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
