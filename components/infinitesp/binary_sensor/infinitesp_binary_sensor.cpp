#include "infinitesp_binary_sensor.h"

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
    // IDU can be at any address in class 4 (e.g., 0x40, 0x41)
    uint8_t src_class = device_addr >> 4;
    if (src_class != 4)
      return;
    auto *data = parent_->get_register(device_addr, REG_IDU_CONFIG);
    if (data && data->size() >= 1) {
      publish_state((data->at(0) & 0x03) != 0);
    }
  }

  // Compressor running: from ODU register 0604 (passive snoop)
  // First uint16 BE pair = current compressor RPM; non-zero means running
  if (sensor_type_ == "compressor_running" && register_key == REG_ODU_COMP_SPEED) {
    uint8_t src_class = device_addr >> 4;
    if (src_class != 5)
      return;
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data && data->size() >= 2) {
      uint16_t rpm = ((uint16_t) data->at(0) << 8) | data->at(1);
      publish_state(rpm != 0);
    }
  }
}

} // namespace infinitesp
} // namespace esphome
