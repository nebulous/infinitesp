#include "infinitesp_datetime.h"

#ifdef USE_DATETIME_TIME

namespace esphome {
namespace infinitesp {

// Set path: compute the grid-normalized duration from the bus clock (the
// anchor — the thermostat owns the countdown; the ESPHome time source would
// skew the end by the bus-vs-wall drift), publish the normalized end
// optimistically, and enqueue the debounced write on the hub. A target inside
// the 15-min floor window means tomorrow.
void InfinitESPDateTime::control(const datetime::TimeCall &call) {
  auto h = call.get_hour();
  auto m = call.get_minute();
  if (!h.has_value() || !m.has_value())
    return;
  auto *state = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
  if (!state || state->size() < REG3B02_MINUTES + 2) {
    ESP_LOGW("InfinitESP", "Zone %u hold_until: bus clock not yet received; command dropped", zone_);
    return;
  }
  uint16_t now_min = ((uint16_t) (*state)[REG3B02_MINUTES] << 8) |
                     (*state)[REG3B02_MINUTES + 1];
  uint16_t target = ((uint16_t) (*h) * 60 + *m) % 1440;
  uint16_t delta = (target + 1440 - now_min) % 1440;
  if (delta < InfinitESPComponent::HOLD_TIMED_MIN)
    delta += 1440;
  delta = InfinitESPComponent::normalize_timed_hold(delta);
  ESP_LOGD("InfinitESP", "Zone %u hold_until %02u:%02u -> %u min (debounced)", zone_, *h, *m, delta);
  parent_->queue_hold_set(zone_, delta, 1500);
  // Let the adoption land before readback republishes (a poll observing the
  // pre-write state would briefly fling the entity back to the old end).
  this->readback_holdoff_ms_ = millis() + 10000;
  uint16_t end_min = (now_min + delta) % 1440;
  this->hour_ = end_min / 60;
  this->minute_ = end_min % 60;
  this->second_ = 0;
  this->publish_state();
}

// Readback: mirror the running hold's end time. REG_SAM_ZONES fires on hold
// changes; REG_SAM_STATE fires on minute ticks (end = now + remaining holds
// steady only because of the quarter snap).
void InfinitESPDateTime::on_register_update(uint8_t device_addr, uint16_t register_key) {
  if (register_key != REG_SAM_ZONES && register_key != REG_SAM_STATE)
    return;
  if (millis() < this->readback_holdoff_ms_)
    return;  // a set is settling; don't fight the UI value
  auto *state = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
  if (!state || state->size() < REG3B02_MINUTES + 2)
    return;
  uint16_t dur = parent_->get_zone_hold_duration(zone_);
  if (dur == 0 || dur >= InfinitESPComponent::HOLD_PERMANENT)
    return;  // schedule-following or permanent: leave state unchanged
  uint16_t now_min = ((uint16_t) (*state)[REG3B02_MINUTES] << 8) |
                     (*state)[REG3B02_MINUTES + 1];
  uint16_t end_min = (now_min + dur + 7) / 15 * 15;  // same snap as format_hold_end
  end_min %= 1440;
  uint8_t h = end_min / 60;
  uint8_t m = end_min % 60;
  if (this->published_ && h == this->last_hour_ && m == this->last_minute_)
    return;
  this->hour_ = h;
  this->minute_ = m;
  this->second_ = 0;
  this->published_ = true;
  this->last_hour_ = h;
  this->last_minute_ = m;
  this->publish_state();
}

}  // namespace infinitesp
}  // namespace esphome

#endif  // USE_DATETIME_TIME
