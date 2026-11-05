#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/infinitesp/infinitesp.h"
#include <string>
#include <vector>
#include <algorithm>
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
  // REG! yaml gate (raw_writes, default false). Forwards to the hub's
  // diagnostic-write gate; called from codegen after set_infinitesp.
  void set_raw_writes(bool v) {
    if (parent_)
      parent_->set_diag_writes_allowed(v);
  }

 protected:
  void process_line_(const std::string &line);
  void respond_(const std::string &prefix, const std::string &value);
  void respond_nak_(const std::string &prefix, const std::string &reason = "");
  std::string format_temp_(int16_t temp_f);
  std::string format_time_(uint16_t minutes);
  // Diagnostic workbench (SCAN? / REG? / REG!): drain engine events each
  // loop and render them as response lines.
  void diag_drain_();
  void diag_render_scan_event_(const infinitesp::InfinitESPComponent::DiagScanEvent &ev);
  void handle_workbench_(const std::string &body);
  std::string diag_hex_(const uint8_t *data, size_t len);
  // JSONL timestamp field: "ts":<epoch_ms> when the hub's time source is
  // valid, "ms":<boot_ms> otherwise (same fallback semantics as bus_jsonl).
  std::string diag_ts_json_();
  // Renders ["1F","2A"] arrays for device_done lines.
  std::string diag_table_array_(const std::vector<uint8_t> &tables);
  // Parses "REG"-command argument tails. addr defaults to the thermostat
  // when absent; reg accepts both 400A and "40 0A" forms. tokens are the
  // space-split parts after "REG"; consumed says how many were used.
  bool diag_parse_reg_args_(const std::vector<std::string> &tokens, size_t from, uint8_t &addr,
                            uint16_t &reg, size_t &consumed);
  // Parses a TIME! write value into bus clock fields. Accepts the SAM-native
  // 12-hour form "HH:MM A/P" (leading zeros required, per SAM01-04XA Table 1
  // examples 8/9; weekday left as CLOCK_KEEP_WEEKDAY) and the InfinitESP
  // ISO8601 extension "YYYY-MM-DDTHH:MM" (naive local wall time - the bus
  // carries local time and no TZ conversion is performed; weekday derived
  // from the date; both fields set). Returns false on any other form.
  bool parse_clock_value_(const std::string &val, uint8_t &weekday, uint16_t &minutes);

 private:
  esphome::infinitesp::InfinitESPComponent *parent_{nullptr};

  // Line buffer. 352: a REG! full-row write on the largest served row
  // (3B03, 150 bytes = 300 hex chars) plus the command itself must fit.
  std::string line_buf_;
  uint32_t last_char_time_{0};
  uint32_t last_activity_time_{0};  // Last time any byte was received
  bool banner_sent_{false};         // True after banner sent for current "connection"
  static const size_t MAX_LINE = 352;
  static const uint32_t INTER_CHAR_TIMEOUT_MS = 5000;
  static const uint32_t CONNECTION_IDLE_MS = 10000;  // ≥10s silence = new connection

  // Diagnostic workbench state: per-device refusal/timeout table lists for
  // SCAN device_done arrays, and pending-command markers for the async
  // replies. Output is JSONL (one JSON object per event line); the vendor
  // ACK/NAK surface stays text.
  bool diag_scan_streaming_{false};
  std::vector<uint8_t> diag_scan_refused_;
  std::vector<uint8_t> diag_scan_timeouts_;
  bool diag_read_pending_{false};
  uint16_t diag_read_reg_{0};
  bool diag_readback_{false};  // pending REG? is the READBACK half of a REG!
  bool diag_write_pending_{false};
  uint16_t diag_write_reg_{0};
  uint8_t diag_write_dest_{0};
};

}  // namespace sam_ascii
}  // namespace esphome
