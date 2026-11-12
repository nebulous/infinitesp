#include "infinitesp_binary_sensor.h"
#include <cmath>

namespace esphome {
namespace infinitesp {

void InfinitESPBinarySensor::on_register_update(uint8_t device_addr, uint16_t register_key) {
  // register_key == 0 means bus status change notification
  if (sensor_type_ == "bus_status") {
    publish_state(parent_->is_bus_online());
    return;
  }

  // electric_heat is deprecated (see binary_sensor/__init__.py): 0316[0] is
  // the IDU heat stage, source-blind. Publishes nothing; the idu_heat_stage
  // sensor and heat_source text sensor carry the fact and interpretation.

  // Compressor running: from ODU register 0604 (passive snoop).
  // Uses actual (measured) RPM [2..3], not the commanded target —
  // non-zero means the compressor is actually spinning.
  if (sensor_type_ == "compressor_running" && register_key == REG_ODU_COMP_SPEED) {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data) {
      float rpm = parent_->odu_compressor_actual_rpm_(*data);
      if (!std::isnan(rpm))
        publish_state(rpm != 0);
    }
  }

  // Per-zone occupancy from SAM 3B02 offset 21 (zones_unoccupied bitmask):
  // bit (zone-1) set = UNOCCUPIED (away / hold-permanent). Publish occupied =
  // bit clear. NOTE this is the thermostat's occupied/away schedule state, not
  // a motion/presence sensor (not carried on the ABCD bus).
  if (sensor_type_ == "occupancy" && register_key == REG_SAM_STATE) {
    if (zone_ >= 1) {
      auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
      if (data && data->size() > REG3B02_UNOCCUPIED)
        publish_state(!((*data)[REG3B02_UNOCCUPIED] & (1 << (zone_ - 1))));
    }
  }

  // Commanded humidifier call: tstat->IDU 0307 byte 0 (issue #63; live both
  // call states 2026-11-09..12). The notification arrives from EITHER the
  // write-capture branch (dedicated members, families that never serve 0307
  // reads) or the passive reply capture (families whose IDU serves 0307 —
  // Heil/ICP class); the getter arbitrates by recency. Publishes the COMMAND:
  // our 58CVA class reports no actual relay state anywhere on the bus.
  if (sensor_type_ == "idu_humidifier" && register_key == REG_IDU_HUMID) {
    bool fresh = false;
    bool call = parent_->idu_humidifier_call(fresh);
    if (fresh) {
      if (call != last_published_call_) {
        ESP_LOGI("InfinitESP", "IDU humidifier call: %s", call ? "ON" : "OFF");
        last_published_call_ = call;
      }
      publish_state(call);
    } else if (!warned_stale_) {
      warned_stale_ = true;
      ESP_LOGW("InfinitESP", "IDU humidifier: no fresh 0307 source (write or reply) in %u s",
               (unsigned) (IDU_HUMID_STALE_MS / 1000));
    }
  }

  // active_fault is deprecated (see binary_sensor/__init__.py): no fault-active
  // state exists on the bus. The type stays config-valid but publishes nothing.
}

} // namespace infinitesp
} // namespace esphome
