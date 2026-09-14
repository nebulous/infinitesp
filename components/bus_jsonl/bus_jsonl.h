#pragma once

#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/infinitesp/infinitesp.h"

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <string>

namespace esphome {
namespace bus_jsonl {

static const char *const TAG = "bus_jsonl";

// Streams every dispatched bus frame as one JSON object per line to a
// uart_tcp_server (reference yaml: port 2373 - "room temperature", 23C/73F). The hub pushes frames at
// dispatch via offer_frame(); lines are rendered at enqueue into a stack
// buffer and drained one per loop(), so the dispatch path never blocks on a
// slow or absent client. The hub applies privacy redaction before the call:
// credential/dealer registers are never offered, and serial-bearing
// registers arrive pre-masked (serial prefixes kept, suffixes replaced with
// '*' bytes).
//
// Timestamps: "ts" renders ISO8601 UTC with millisecond precision when the
// config declares a time platform (auto-discovered by the Python codegen;
// no time_id knob) and the clock has synced; otherwise lines carry
// boot-relative "ms". The epoch provider is generated as a lambda in
// main.cpp (issue #26 pattern) so this header needs no time includes and
// time-less configs still build.
//
// Give the underlying uart_tcp_server a tx_buffer_size (reference: 2048) so
// a briefly stalled client backlogs in the server instead of dropping bytes.
class BusJsonlComponent : public Component, public uart::UARTDevice,
                          public infinitesp::BusFrameSink {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // BusFrameSink. Called from the hub's dispatch path.
  void offer_frame(uint32_t ms, uint8_t src, uint8_t dst, uint8_t func, uint16_t reg,
                   const uint8_t *data, size_t len) override;

  // Lambda returning epoch seconds (0 when unsynced); generated in main.cpp.
  void set_epoch_provider(std::function<time_t()> fn) { epoch_provider_ = std::move(fn); }

  uint32_t get_dropped_lines() const { return dropped_lines_; }

 protected:
  // Burst absorber between dispatch and the loop() drain. Caps are counts
  // AND bytes; overflow drops the oldest lines (a live stream should show
  // recent activity) with a counter and a throttled warning.
  static const size_t MAX_QUEUED_LINES = 48;
  static const size_t MAX_QUEUED_BYTES = 8192;
  // Largest non-dropped register observed is 3B03 at 150 bytes -> 300 hex
  // chars plus the ~85-char header with an ISO timestamp. Oversize payloads
  // degrade to "data":null. (Named LINE_BUF_MAX: LINE_MAX is a libc macro.)
  static const size_t LINE_BUF_MAX = 640;
  std::deque<std::string> queue_;
  size_t queued_bytes_{0};
  uint32_t dropped_lines_{0};
  uint32_t last_drop_log_ms_{0};

  // Output-only service: discard anything a client sends (a stray
  // keystroke into nc, a tool's protocol probe) so the server's RX ring
  // stays empty. The throttled log explains the silence to anyone
  // expecting a prompt like the SAM ASCII port.
  uint32_t last_input_log_ms_{0};

  // Epoch-ms correlation: the RTC is re-read whenever its whole second
  // advances (rebase_epoch_), and millis() carries the sub-second precision
  // between rolls. Survives RTC steps (NTP corrections) by rebasing on the
  // next frame after a step.
  std::function<time_t()> epoch_provider_{nullptr};
  time_t last_sec_{0};
  int64_t base_epoch_ms_{0};
  uint32_t base_boot_ms_{0};
  void rebase_epoch_();
};

}  // namespace bus_jsonl
}  // namespace esphome
