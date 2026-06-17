#pragma once
#include "esphome/components/cover/cover.h"
#include "esphome/core/automation.h"
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

// Zone damper cover. Reports the damper position commanded by the thermostat
// (register 0308, mirrored to 0319) as cover position 0.0-1.0, and exposes an
// on_change trigger that fires whenever the cover's reported position does not
// match the commanded position — whether the command came from the thermostat
// (bus) or from Home Assistant.
//
// Contract: the cover NEVER writes the ABCD bus. It is a position reporter
// plus a trigger hub, not an actuator. There is no bus-write API for it to
// call, and actuating real dampers/relays is the user's responsibility, done
// inside their on_change lambda. This is also why a single trigger for
// both sources is safe: with no bus write, a commanded move cannot echo back
// as a bus event and re-fire the trigger (no feedback loop).
//
// REQUIRES either zone_controller_address (ZC emulation) OR a real physical
// zone controller on the bus. The damper register (0308, mirrored to 0319) is
// captured either from the emulated ZC's write handler or — when not emulating
// — passively snooped from the tstat→ZC WRITE on the bus. Without either (no
// ZC at all), on_register_update() never fires and this cover never publishes
// (reports unknown in HA).
//
// Bus encoding: 4 bytes in 0308, one per zone (zones 1-4). Each byte is a
// damper step 0x00-0x0F (16 positions). Cover position = byte / 15.0.
//
// Trigger: change_trigger_ (exposed to YAML as `on_change`) fires with the new
// 0.0-1.0 position whenever the commanded step differs from the last step we
// acted on — from the bus (thermostat moves the damper) or from HA
// (slider/open/close). One rule, one anchor (last_step_).
//
// Floating point: comparisons are done in step space (the protocol's 16
// positions, 0x00-0x0F), not on the raw float. The bus byte is itself the
// step; HA values are quantized via lroundf(pos * 15). So an HA re-command of
// the current step (e.g. slider at 0.53 while the cover sits at step 8 =
// 0.533…) does not spuriously fire, and a genuine step change (including an
// HA transient snapping back to the bus value) always does. There are no
// true FP errors to guard (a stored value compares bitwise against the same
// computation); quantization is purely to suppress same-step spurious fires.
//
// Boot: last_step_ is initialized to 0xFF (a sentinel that never matches a
// real 0x00-0x0F step), so the FIRST commanded position always fires and
// publishes — even if it happens to equal Cover's default position (1.0 =
// step 15, COVER_OPEN). Without this, a zone that is fully open (0x0F) at
// boot would never actuate its relay, since the bogus COVER_OPEN default
// would compare equal to the real bus value.
//
// HA-set positions are transient: control() publishes them so HA acknowledges
// the command, but the next bus-commanded change overwrites them.
//
// publish_state() does not re-invoke control(), so publishing the bus-driven
// position never re-fires the trigger through the HA-command path either.
//
// Actuation is optional. Configure an on_change trigger to run when the
// position is commanded (bus or HA). Without one, the trigger fires to
// nothing and the cover is a pure position reporter.
class InfinitESPCover : public cover::Cover, public InfinitESPEntity {
 public:
  void control(const cover::CoverCall &call) override;
  cover::CoverTraits get_traits() override;
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;

  // on_change trigger. Fires with the new 0.0-1.0 position whenever the
  // reported position doesn't match the commanded one (bus or HA command).
  Trigger<float> *get_change_trigger() { return &change_trigger_; }

 private:
  Trigger<float> change_trigger_;
  uint8_t last_step_{0xFF};  // last commanded step acted on; 0xFF = never (forces first fire)
};

}  // namespace infinitesp
}  // namespace esphome
