#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include <string>
#include <cstdint>

namespace esphome {
namespace infinitesp {
class InfinitESPComponent;
}
namespace sam_ascii {

class SamAsciiComponent : public Component, public uart::UARTDevice {
 public:
  SamAsciiComponent() = default;

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_infinitesp(esphome::infinitesp::InfinitESPComponent *parent) { parent_ = parent; }

 protected:
  void process_line_(const std::string &line);
  void respond_(const std::string &prefix, const std::string &value);
  void respond_nak_(const std::string &prefix, const std::string &reason = "");
  std::string format_temp_(uint8_t temp_f);
  std::string format_time_(uint16_t minutes);

 private:
  esphome::infinitesp::InfinitESPComponent *parent_{nullptr};

  // Line buffer
  std::string line_buf_;
  uint32_t last_char_time_{0};
  static const size_t MAX_LINE = 64;
  static const uint32_t INTER_CHAR_TIMEOUT_MS = 5000;
};

}  // namespace sam_ascii
}  // namespace esphome
