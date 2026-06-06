#pragma once
#include "esphome/components/select/select.h"
#include "../infinitesp.h"

namespace esphome {
namespace infinitesp {

class InfinitESPSelect : public select::Select, public InfinitESPEntity {
 public:
  void control(const std::string &value) override;
  void on_register_update(uint8_t device_addr, uint16_t register_key) override;
  void set_select_type(const std::string &type) { select_type_ = type; }

 protected:
  std::string select_type_;
  uint8_t current_mode_{0xFF}; // invalid sentinel — forces first publish
};

} // namespace infinitesp
} // namespace esphome
