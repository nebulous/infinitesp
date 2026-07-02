#pragma once

#ifdef USE_ESP32

#include "esphome/components/uart/uart_component_esp_idf.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "line_code_encoder.h"

#include <driver/rmt_tx.h>

namespace esphome::uart_rmtx {

/// UART component that RECEIVES on the hardware UART (inherited from
/// IDFUARTComponent, untouched) and TRANSMITS via an RMT line-code waveform on
/// a separate GPIO. Purpose: keep the Waveshare 6CH's auto-direction one-shot
/// primed without a hardware bodge, while leaving the proven hardware-RX path
/// intact. See private/hardware-research/rmt-linecode-component-plan.md.
///
/// The hardware UART is opened RX-only (no tx_pin forwarded to the base), so
/// the RMT channel is free to own the TX GPIO.
class RmtTxUARTComponent : public uart::IDFUARTComponent {
 public:
  void setup() override;
  void dump_config() override;

  // TX overrides — RX (read_array/peek_byte/available) inherited as-is.
  void write_array(const uint8_t *data, size_t len) override;
  uart::UARTFlushResult flush() override;

  // The RMT TX GPIO. NOT forwarded to the base UART (the base opens RX-only).
  void set_rmt_tx_pin(InternalGPIOPin *pin) { rmt_tx_pin_ = pin; }
  void set_sub_bits(uint8_t n) { line_enc_.set_sub_bits(n); }
  void set_rmt_mem_symbols(uint32_t n) { rmt_mem_symbols_ = n; }

 protected:
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
