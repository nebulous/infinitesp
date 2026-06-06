#pragma once
#include "esphome/components/sensor/sensor.h"
#include <cmath>
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPSensor : public sensor::Sensor, public InfinitESPEntity {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }

 private:
  std::string sensor_type_;
};

} // namespace infinitesp
} // namespace esphome
