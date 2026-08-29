#pragma once
// Guarded: the component root always compiles, but the stock number entity
// only exists when a number entity is registered. defines.h first —
// USE_NUMBER is a generated define, not a -D flag (see infinitesp_datetime.h).
#include "esphome/core/defines.h"

#ifdef USE_NUMBER

#include "esphome/components/number/number.h"
#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

// Per-zone "Hold Minutes": remaining timed-hold minutes, 0 when no timed hold
// is running (schedule or permanent). Setting it arms a native bus timed hold
// for N minutes; setting 0 cancels (back to Per Schedule). Relative-duration
// companion to the "Hold Until" time entity: either can arm, both read back
// the same served 3B03 state.
//
// Like the datetime entity, deliberately NOT a Component (auto-spawned
// Component registrations stall the hub's bus loop; see infinitesp_datetime.h
// and the 2026-08-28 outage). Debounce goes through queue_hold_set on the hub;
// the optimistic publish is the NORMALIZED value so the UI never shows a
// number the bus will not produce.
class InfinitESPNumber : public number::Number, public InfinitESPEntity {
 public:
  void control(float value) override;
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;

 protected:
  uint32_t readback_holdoff_ms_{0};
  float last_published_{NAN};
};

}  // namespace infinitesp
}  // namespace esphome

#endif  // USE_NUMBER
