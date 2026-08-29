#pragma once
// Guarded: the component root always compiles, but the stock datetime time
// entity only exists when a datetime entity is registered. defines.h first —
// USE_DATETIME_TIME is a generated define, not a -D flag (see
// infinitesp_datetime.h history).
#include "esphome/core/defines.h"

#ifdef USE_DATETIME_TIME

#include "esphome/components/datetime/time_entity.h"
#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

// Per-zone "Hold Until" time entity. Setting it arms a native bus timed hold
// ending at that clock time (set_zone_hold; the thermostat owns the
// countdown, no firmware timer). While a finite timed hold runs, the entity
// mirrors its true end time from the served 3B03 (bus clock + remaining
// minutes, quarter-snapped like format_hold_end). On schedule or permanent
// holds the state is left unchanged (stale until superseded); cancelling
// stays on the climate "Per Schedule" preset.
//
// Deliberately NOT a Component: auto-spawned Component registrations corrupt
// ESPHome's loop-slot scheduling and stall the hub's bus loop (the 2026-08-28
// outage; the entities compiled and registered fine while the hub went deaf).
// The set path debounces through the hub's queue_hold_set instead; readback
// suppression is millis-based and checked in on_register_update.
//
// The optimistic publish is the NORMALIZED end (grid-rounded duration from
// the bus clock), so the UI never shows a time the bus will not produce:
// pick 8:40 and it shows the 8:45 it will actually hold until.
class InfinitESPDateTime : public datetime::TimeEntity, public InfinitESPEntity {
 public:
  void control(const datetime::TimeCall &call) override;
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;

 protected:
  uint32_t readback_holdoff_ms_{0};
  bool published_{false};
  uint8_t last_hour_{0xFF};
  uint8_t last_minute_{0xFF};
};

}  // namespace infinitesp
}  // namespace esphome

#endif  // USE_DATETIME_TIME
