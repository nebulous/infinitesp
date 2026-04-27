#pragma once
#include "esphome/components/text_sensor/text_sensor.h"
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPTextSensor : public text_sensor::TextSensor, public InfinitESPDevice {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }

 protected:
  std::string sensor_type_;
};

} // namespace infinitesp
} // namespace esphome
