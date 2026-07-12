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

  // Electric heat: from IDU register 0316 (passive snoop)
  if (sensor_type_ == "electric_heat" && register_key == REG_IDU_CONFIG) {
    auto *data = parent_->get_register(device_addr, REG_IDU_CONFIG);
    if (data && !data->empty())
      publish_state(parent_->idu_electric_heat_(*data));
  }

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

  // Active fault: ON when any thermostat fault-history (0x4202) entry has the
  // active bit set. Primary "something failed right now" alert.
  if (sensor_type_ == "active_fault" && register_key == REG_TSTAT_FAULTS)
    publish_state(parent_->has_active_fault());
}

} // namespace infinitesp
} // namespace esphome
