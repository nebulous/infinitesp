#pragma once
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPBinarySensor : public binary_sensor::BinarySensor, public InfinitESPDevice {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }

 private:
  std::string sensor_type_;
};

} // namespace infinitesp
} // namespace esphome
