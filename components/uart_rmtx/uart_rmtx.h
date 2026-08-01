#pragma once

#ifdef USE_ESP32

#include "esphome/components/uart/uart_component.h"          // UARTComponent (abstract interface)
#include "esphome/components/uart/uart_component_esp_idf.h"  // IDFUARTComponent (contained, for RX)
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "line_code_encoder.h"

#include <driver/rmt_tx.h>

namespace esphome::uart_rmtx {

/// UART component that RECEIVES on a contained IDF UART and TRANSMITS via an RMT
/// line-code waveform on a separate GPIO. Purpose: keep the Waveshare 6CH's
/// auto-direction one-shot primed without a hardware bodge, while leaving the
/// proven hardware-RX path intact. See private/hardware-research/rmt-linecode-component-plan.md.
///
/// Composition, not inheritance. esp-idf 5.5.5 (ESPHome 2026.7.x) marks
/// uart::IDFUARTComponent `final`, so this class can no longer subclass it.
/// Instead it implements the abstract uart::UARTComponent interface, holds an
/// IDFUARTComponent for RX, and overrides only write_array/flush for RMT TX.
///
/// The base HW UART is opened RX-only (no tx_pin forwarded to idf_rx_), so the
/// RMT channel owns the TX GPIO.
class RmtTxUARTComponent : public uart::UARTComponent, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::BUS; }

  // RX — delegate to the contained IDF UART.
  bool read_array(uint8_t *data, size_t len) override { return idf_rx_.read_array(data, len); }
  bool peek_byte(uint8_t *data) override { return idf_rx_.peek_byte(data); }
  size_t available() override { return idf_rx_.available(); }
  void load_settings(bool dump_config) override { idf_rx_.load_settings(dump_config); }
  // TX — RMT line-code (see .cpp).
  void write_array(const uint8_t *data, size_t len) override;
  uart::UARTFlushResult flush() override;

  // Config forwarders. The codegen configures THIS object; each setter forwards
  // to the contained IDF UART so its setup() applies them. (esp-idf 5.5.5 made
  // IDFUARTComponent final, so we compose it rather than inherit its setters.)
  void set_baud_rate(uint32_t b) { idf_rx_.set_baud_rate(b); }
  void set_data_bits(uint8_t d) { idf_rx_.set_data_bits(d); }
  void set_stop_bits(uint8_t s) { idf_rx_.set_stop_bits(s); }
  void set_parity(uart::UARTParityOptions p) { idf_rx_.set_parity(p); }
  void set_rx_buffer_size(size_t s) { idf_rx_.set_rx_buffer_size(s); }
  void set_rx_pin(InternalGPIOPin *p) { idf_rx_.set_rx_pin(p); }
  void set_rx_full_threshold(size_t t) { idf_rx_.set_rx_full_threshold(t); }
  void set_rx_timeout(size_t t) { idf_rx_.set_rx_timeout(t); }
  void set_flush_timeout(uint32_t t) { idf_rx_.set_flush_timeout(t); }

  // RMT TX GPIO and line-code config.
  void set_rmt_tx_pin(InternalGPIOPin *pin) { rmt_tx_pin_ = pin; }
  void set_sub_bits(uint8_t n) { line_enc_.set_sub_bits(n); }
  void set_rmt_mem_symbols(uint32_t n) { rmt_mem_symbols_ = n; }

 protected:
  // Satisfy UARTComponent's pure virtual. idf_rx_ performs the real logger-conflict
  // check inside its own setup(), so nothing to do here.
  void check_logger_conflict() override {}

  uart::IDFUARTComponent idf_rx_;

  InternalGPIOPin *rmt_tx_pin_{nullptr};
  uint32_t rmt_mem_symbols_{64};
  rmt_channel_handle_t tx_ch_{nullptr};
  rmt_encoder_handle_t copy_encoder_{nullptr};
  LineCodeEncoder line_enc_;
  // Pre-allocated symbol buffer, grown on demand to fit the largest frame sent.
  rmt_symbol_word_t *sym_buf_{nullptr};
  size_t sym_buf_cap_{0};
  uint32_t resolution_hz_{0};
  bool tx_in_flight_{false};

  void ensure_sym_buf_(size_t needed_symbols);
};

}  // namespace esphome::uart_rmtx
#endif  // USE_ESP32
