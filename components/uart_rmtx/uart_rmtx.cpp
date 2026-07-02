#ifdef USE_ESP32

#include "uart_rmtx.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <cstring>
#include <driver/rmt_types.h>
#include <esp_clk_tree.h>
#include <soc/rmt_struct.h>

namespace esphome::uart_rmtx {

static const char *const TAG = "uart.rmtx";

void RmtTxUARTComponent::setup() {
  // 1) Bring up the hardware UART RX-only. We intentionally do NOT forward a
  //    tx_pin to the base (set_tx_pin is never called), so IDFUARTComponent's
  //    load_settings() passes tx=-1 (UART_PIN_NO_CHANGE) and opens the HW UART
  //    RX-only. RMT then owns the TX GPIO. (rx_full_threshold is set by
  //    codegen — see AGENTS.md "Subclassing IDFUARTComponent" for why a 0
  //    default silently fails the component.)
  uart::IDFUARTComponent::setup();
  if (this->is_failed())
    return;

  if (this->rmt_tx_pin_ == nullptr) {
    ESP_LOGE(TAG, "rmt tx_pin is required");
    this->mark_failed();
    return;
  }

  // 2) Resolve the RMT clock source frequency (80MHz APB on ESP32-S3).
  uint32_t freq = 0;
  esp_err_t err = esp_clk_tree_src_get_freq_hz((soc_module_clk_t) RMT_CLK_SRC_DEFAULT,
                                               ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &freq);
  if (err != ESP_OK || freq == 0) {
    ESP_LOGE(TAG, "esp_clk_tree_src_get_freq_hz failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }
  this->resolution_hz_ = freq;

  // 3) Configure and enable the RMT TX channel. with_dma=0: non-DMA ping-pong
  //    streams arbitrarily long frames through the 48-word/channel RAM via the
  //    threshold ISR (proven sufficient — see rs485-rmt-linecode-hack.md).
  gpio_num_t rmt_gpio = static_cast<gpio_num_t>(this->rmt_tx_pin_->get_pin());
  rmt_tx_channel_config_t ch_cfg{};
  ch_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  ch_cfg.resolution_hz = this->resolution_hz_;
  ch_cfg.gpio_num = rmt_gpio;
  ch_cfg.mem_block_symbols = this->rmt_mem_symbols_;
  ch_cfg.trans_queue_depth = 1;
  ch_cfg.flags.invert_out = this->rmt_tx_pin_->is_inverted();
  ch_cfg.flags.with_dma = 0;
  ch_cfg.intr_priority = 0;

  err = rmt_new_tx_channel(&ch_cfg, &this->tx_ch_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // 4) Copy encoder: we pre-encode the whole frame into sym_buf_ on the main
  //    loop, then the driver's ISR just memcpys symbols into RMT RAM.
  rmt_copy_encoder_config_t enc_cfg{};
  err = rmt_new_copy_encoder(&enc_cfg, &this->copy_encoder_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  err = rmt_enable(this->tx_ch_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
    this->mark_failed();
    return;
  }

  // 5) Finalize the line-code timing math for this baud/N at this clock.
  this->line_enc_.set_resolution_hz(this->resolution_hz_);
  this->line_enc_.init();

  // Park the line at UART idle (high) immediately — the channel's idle level
  // is undefined until something has been transmitted.
  rmt_symbol_word_t idle{};
  idle.duration0 = 1;
  idle.level0 = 1;
  idle.duration1 = 1;
  idle.level1 = 1;
  rmt_transmit_config_t tx_cfg{};
  tx_cfg.flags.eot_level = 1;
  rmt_transmit(this->tx_ch_, this->copy_encoder_, &idle, sizeof(idle), &tx_cfg);
  rmt_tx_wait_all_done(this->tx_ch_, 100);
}

void RmtTxUARTComponent::ensure_sym_buf_(size_t needed_symbols) {
  if (sym_buf_cap_ >= needed_symbols)
    return;
  // Free old, allocate new (internal RAM — RMT reads it via CPU-cached path).
  if (sym_buf_ != nullptr)
    free(sym_buf_);
  sym_buf_ = static_cast<rmt_symbol_word_t *>(malloc(needed_symbols * sizeof(rmt_symbol_word_t)));
  sym_buf_cap_ = needed_symbols;
}

void RmtTxUARTComponent::write_array(const uint8_t *data, size_t len) {
  if (this->is_failed() || len == 0)
    return;

  // Encode the whole frame into sym_buf_ (coalesced line-code symbols).
  const size_t max_sym = LineCodeEncoder::symbols_for_bytes(len);
  this->ensure_sym_buf_(max_sym);
  size_t n = this->line_enc_.encode(data, len, this->sym_buf_, this->sym_buf_cap_);
  if (n == 0)
    return;

  // Hand it to the driver. trans_queue_depth=1 → blocks until any previous TX
  // drained, then queues this one. eot_level=1 parks the line high afterwards.
  rmt_transmit_config_t tx_cfg{};
  tx_cfg.flags.eot_level = 1;
  esp_err_t err = rmt_transmit(this->tx_ch_, this->copy_encoder_, this->sym_buf_, n * sizeof(rmt_symbol_word_t),
                               &tx_cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
    return;
  }
  this->tx_in_flight_ = true;

#ifdef USE_UART_DEBUGGER
  for (size_t i = 0; i < len; i++)
    this->debug_callback_.call(UART_DIRECTION_TX, data[i]);
#endif
}

uart::UARTFlushResult RmtTxUARTComponent::flush() {
  if (!this->tx_in_flight_)
    return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  // A worst-case 266-byte frame at 38400 baud is ~69ms on the wire; 2000ms is a
  // generous bound that still catches a truly stuck peripheral.
  esp_err_t err = rmt_tx_wait_all_done(this->tx_ch_, 2000);
  this->tx_in_flight_ = false;
  if (err == ESP_OK)
    return uart::UARTFlushResult::UART_FLUSH_RESULT_SUCCESS;
  if (err == ESP_ERR_TIMEOUT)
    return uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT;
  return uart::UARTFlushResult::UART_FLUSH_RESULT_FAILED;
}

void RmtTxUARTComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "uart_rmtx (RMT-TX + HW-RX):");
  ESP_LOGCONFIG(TAG, "  HW UART %u (RX only)", this->get_hw_serial_number());
  LOG_PIN("  RMT TX Pin: ", this->rmt_tx_pin_);
  ESP_LOGCONFIG(TAG,
                "  Baud Rate: %" PRIu32 "\n"
                "  Sub-bits/bit (N): %u\n"
                "  RMT resolution: %" PRIu32 " Hz\n"
                "  RMT mem symbols: %" PRIu32 "\n"
                "  DMA: off (non-DMA ping-pong)",
                this->line_enc_.get_baud_rate(), this->line_enc_.get_sub_bits(), this->resolution_hz_,
                this->rmt_mem_symbols_);
}

}  // namespace esphome::uart_rmtx
#endif  // USE_ESP32
