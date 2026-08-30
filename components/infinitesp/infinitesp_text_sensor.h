#pragma once
#include "esphome/components/text_sensor/text_sensor.h"
#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPTextSensor : public text_sensor::TextSensor, public InfinitESPEntity {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }
  // Exact bus node for manufacture_date (manual configs pin a device this
  // way; the low nibble varies across installs, so generated entities never
  // use it and select a class via the base-class bus_class dispatch gate).
  // 0 = unset.
  void set_device_address(uint8_t addr) { target_device_addr_ = addr; }

 protected:
  std::string sensor_type_;
  uint8_t target_device_addr_{0};  // 0 = unset
};

} // namespace infinitesp
} // namespace esphome
