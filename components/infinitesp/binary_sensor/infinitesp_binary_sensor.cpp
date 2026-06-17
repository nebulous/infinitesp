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

  // Compressor running: from ODU register 0604 (passive snoop)
  // First uint16 BE pair = current compressor RPM; non-zero means running
  if (sensor_type_ == "compressor_running" && register_key == REG_ODU_COMP_SPEED) {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data) {
      float rpm = parent_->odu_compressor_rpm_(*data);
      if (!std::isnan(rpm))
        publish_state(rpm != 0);
    }
  }
}

} // namespace infinitesp
} // namespace esphome
