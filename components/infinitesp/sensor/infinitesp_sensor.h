#pragma once
#include "esphome/components/sensor/sensor.h"
#include <cmath>
#include <functional>
#include <string>
#include <vector>
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPSensor : public sensor::Sensor, public InfinitESPEntity {
 public:
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_sensor_type(const std::string &type) { sensor_type_ = type; }

  // --- raw_register: generic bus-field sensor (user-defined decode) ---
  using RawLambda = std::function<float(const std::vector<uint8_t> &)>;
  void set_raw_target(uint8_t device_addr, uint16_t reg) {
    raw_device_ = device_addr;
    raw_register_ = reg;
  }
  void set_raw_offset(uint8_t off) { raw_offset_ = off; }
  void set_raw_scale(float s) { raw_scale_ = s; }
  void set_raw_datatype(const std::string &d) { raw_datatype_ = d; }
  void set_raw_value_range(float lo, float hi) {
    raw_min_ = lo;
    raw_max_ = hi;
    raw_has_range_ = true;
  }
  void set_raw_lambda(RawLambda &&fn) { raw_lambda_ = std::move(fn); }

 private:
  std::string sensor_type_;

  // raw_register config (decode mode uses offset/datatype/scale; lambda mode uses raw_lambda_).
  uint8_t raw_device_{0};
  uint16_t raw_register_{0};
  uint8_t raw_offset_{0};
  float raw_scale_{1.0f};
  std::string raw_datatype_;  // empty in lambda mode
  float raw_min_{0}, raw_max_{0};
  bool raw_has_range_{false};
  RawLambda raw_lambda_;  // set in lambda mode; full register data is passed in
};

}  // namespace infinitesp
}  // namespace esphome
