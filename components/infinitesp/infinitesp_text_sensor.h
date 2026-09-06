#pragma once
#include "esphome/components/text_sensor/text_sensor.h"
#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPTextSensor : public text_sensor::TextSensor, public InfinitESPEntity {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }
  // set_device_address lives on the InfinitESPEntity base: manual configs
  // pin an exact bus node for manufacture_date (the low nibble varies across
  // installs), and the hub idu_address/odu_address overrides push pins into
  // class-scoped entities at setup().

 protected:
  std::string sensor_type_;
};

} // namespace infinitesp
} // namespace esphome
