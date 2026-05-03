#include "infinitesp.h"

namespace esphome {
namespace infinitesp {

// Thermostat registers to poll (table, row)
// Thermostat registers the SAM polls to keep its cache current
static const uint8_t POLL_REGS[][2] = {
    {0x3B, 0x02},  // system state: temps, mode, humidity
    {0x3B, 0x03},  // zone settings: setpoints, fan, hold
};
static const uint8_t POLL_REG_COUNT = 2;

// Slow-poll thermostat registers (0x4xxx tables, polled less frequently)
// These are thermostat-internal config tables, not SAM registers.
static const uint8_t SLOW_POLL_REGS[][2] = {
    {0x40, 0x0A},  // comfort profiles (home/away/sleep/wake/manual setpoints+fan)
    {0x40, 0x12},  // vacation settings (min/max temp, fan)
    {0x46, 0x08},  // WiFi: SSID, password, hostname
    {0x46, 0x09},  // Cloud: host, device IP
    {0x46, 0x0A},  // dealer info: name, brand, URL
};
static const uint8_t SLOW_POLL_REG_COUNT = 5;
static const uint32_t SLOW_POLL_INTERVAL_MS = 31000;  // poll every 31s (prime, avoids beating with other timers)

void InfinitESPComponent::setup() {
  ESP_LOGI("InfinitESP", "InfinitESP v0.1.0 build %s %s", __DATE__, __TIME__);
  ESP_LOGI("InfinitESP", "Address=0x%02X", address_);

  // Initialize NVS-backed preference for cached WiFi credentials
  wifi_pref_ = global_preferences->make_preference<CachedWifi>(
    fnv1a_hash("InfinitESP::wifi_cache"), true);

  CachedWifi cached{};
  if (wifi_pref_.load(&cached) && cached.ssid[0] != '\0') {
    cached_wifi_ssid_ = cached.ssid;
    cached_wifi_password_ = cached.password;
    ESP_LOGI("InfinitESP", "Cached WiFi: SSID=%s", cached.ssid);
  }

  initialize_defaults_();
  rx_buffer_.reserve(FRAME_MAX_SIZE);

  // Push initialized register defaults to all sub-platforms
  // so they don't sit in "unknown" until the first bus reply
  for (auto &kv : device_registers_[address_]) {
    notify_devices_(address_, kv.first);
  }

  // Initialize status LED (if configured via YAML)
#ifdef USE_INFINITESP_STATUS_LED_PIN
  if (status_led_gpio_ != nullptr) {
    status_led_gpio_->setup();
    status_led_gpio_->digital_write(false);
    ESP_LOGI("InfinitESP", "Status LED: pin configured");
  }
#endif
#ifdef USE_INFINITESP_STATUS_LIGHT
  if (status_light_ != nullptr) {
    auto traits = status_light_->get_traits();
    status_light_has_rgb_ = traits.supports_color_capability(light::ColorCapability::RGB);
    ESP_LOGI("InfinitESP", "Status LED: light \"%s\" (RGB=%s)",
             status_light_->get_name().c_str(), status_light_has_rgb_ ? "yes" : "no");
  }
#endif
}

void InfinitESPComponent::loop() {
  uint32_t loop_start = millis();

  // Echo suppression: after TX, drain any echo bytes from the RS485 transceiver.
  // The echo arrives within ~60ms of TX completion (38400 baud + TCP RTT).
  // Large frames (133-byte REPLY) take ~35ms just to transmit at 38400 baud.
  // We drain for 60ms after each TX to avoid corrupted frame parsing.
  // This sacrifices some passively-snooped bus data but prevents CRC failure cascades.
  if (last_tx_done_time_ > 0 && (loop_start - last_tx_done_time_ < 60)) {
    int drained = 0;
    while (available()) {
      uint8_t discard;
      read_byte(&discard);
      drained++;
    }
    if (drained > 0) {
      tx_echo_drain_count_ += drained;
      ESP_LOGD("InfinitESP", "Echo drain: %d bytes (total_drained=%u, since_tx=%ums)",
               drained, tx_echo_drain_count_, loop_start - last_tx_done_time_);
    }
    // Don't process any bus data during echo drain window
    return;
  }

  // Check UART buffer fill level before draining
  int uart_available = available();
  if ((uint32_t) uart_available > diag_uart_hwm_) {
    diag_uart_hwm_ = uart_available;
  }
  // Flag if UART buffer is >75% full (approaching overflow)
  if (uart_available > 768) {  // 75% of 1024
    diag_uart_overflow_events_++;
    ESP_LOGW("InfinitESP", "UART BUFFER HIGH: %d bytes available (HWM=%u) overflow_events=%u",
             uart_available, diag_uart_hwm_, diag_uart_overflow_events_);
  }

  // Read all available bytes from UART
  while (available()) {
    uint8_t byte;
    read_byte(&byte);
    diag_total_rx_bytes_++;
    rx_hex_log_.push_back(byte);
    parse_byte_(byte);
  }
  uint32_t now = millis();

  // Periodic diagnostics every 5 seconds
  if (now - diag_last_stats_time_ > 5000) {
    // Purge timed-out polls (anything older than 5s)
    uint32_t purged = 0;
    for (auto it = pending_polls_.begin(); it != pending_polls_.end();) {
      if (now - it->sent_ms > 5000) {
        ESP_LOGW("InfinitESP", "POLL TIMEOUT: tx_seq=%u dest=%02X reg=%04X sent=%ums ago",
                 it->tx_seq, it->dest, it->reg_key, now - it->sent_ms);
        diag_reply_timeout_++;
        it = pending_polls_.erase(it);
        purged++;
      } else {
        ++it;
      }
    }
    diag_poll_purged_ += purged;

    ESP_LOGI("InfinitESP", "STATS rx_bytes=%u tx_bytes=%u rx_frames=%u tx_frames=%u "
             "crc_fail=%u stale=%u uart_hwm=%u overflow_evts=%u "
             "reply_exp=%u reply_got=%u reply_timeout=%u poll_pending=%u "
             "tx_flush_max=%ums loop_max=%ums inter_frame=%u..%ums echo_drain=%u",
             diag_total_rx_bytes_, diag_total_tx_bytes_, diag_frames_parsed_, diag_tx_seq_,
             diag_crc_fail_, diag_stale_discard_, diag_uart_hwm_, diag_uart_overflow_events_,
             diag_reply_expected_, diag_reply_received_, diag_reply_timeout_,
             (uint32_t) pending_polls_.size(),
             diag_tx_flush_max_ms_, diag_loop_max_ms_,
             diag_inter_frame_min_ms_ == UINT32_MAX ? 0 : diag_inter_frame_min_ms_,
             diag_inter_frame_max_ms_,
             tx_echo_drain_count_);
    diag_last_stats_time_ = now;
    // Reset peak trackers each stats interval
    diag_uart_hwm_ = 0;
    diag_tx_flush_max_ms_ = 0;
    diag_loop_max_ms_ = 0;
    diag_inter_frame_min_ms_ = UINT32_MAX;
    diag_inter_frame_max_ms_ = 0;
  }

  // Flush raw hex log every 200ms of inactivity (DEBUG level to reduce overhead)
  // Use heap-allocated string to avoid stack overflow with large bursts
  if (!rx_hex_log_.empty() && (now - last_rx_time_ > 200)) {
    std::string hex;
    hex.reserve(rx_hex_log_.size() * 3);
    for (size_t i = 0; i < rx_hex_log_.size(); i++) {
      char tmp[4];
      snprintf(tmp, sizeof(tmp), "%02X ", rx_hex_log_[i]);
      hex.append(tmp);
    }
    ESP_LOGV("InfinitESP", "RAW RX (%d bytes): %s", rx_hex_log_.size(), hex.c_str());
    rx_hex_log_.clear();
  }

  // Check bus online status (timeout 30s)
  if (bus_online_ && (now - last_reply_time_ > 30000)) {
    bus_online_ = false;
    ESP_LOGW("InfinitESP", "Bus offline - no replies for 30s");
    notify_devices_(0, 0);
  }

  // One-shot: inject cached WiFi credentials ~15s after boot if WiFi isn't connected yet.
  // This gives the primary (secrets.yaml) credentials time to connect first.
  if (!wifi_injected_ && now > 15000) {
    wifi_injected_ = true;
    inject_cached_wifi_();
  }

  // Update status LED (blink patterns / color changes)
  update_status_led_();

  // Poll thermostat every 3 seconds.
  // Gap: 50ms since last complete frame (frame-boundary, not byte-boundary).
  // The thermostat's own inter-frame gap is 10-30ms, so if 50ms passes
  // with no new frame, the bus is genuinely idle between transactions.
  // This is tighter than the old 200ms byte-gap but still conservative.
  const uint32_t bus_idle_ms = diag_last_frame_time_ ? (now - diag_last_frame_time_) : 1000;
  bool fast_poll_sent = false;
  if ((now - last_poll_time_ > 3000) && bus_idle_ms > 50) {
    poll_thermostat_();
    last_poll_time_ = now;
    fast_poll_sent = true;
  }

  // Slow-poll 0x4xxx thermostat config registers on their own schedule.
  // MUST NOT fire in the same loop iteration as the fast poll — sending
  // two READ frames to the same thermostat back-to-back causes the echo
  // drain to eat one of the replies (observed 44% poll timeout rate).
  if (!fast_poll_sent && bus_online_ &&
      (now - last_slow_poll_time_ >= SLOW_POLL_INTERVAL_MS) && bus_idle_ms > 50) {
    const auto &sreg = SLOW_POLL_REGS[slow_poll_index_ % SLOW_POLL_REG_COUNT];
    uint16_t sreg_key = (sreg[0] << 8) | sreg[1];
    ESP_LOGI("InfinitESP", "SLOW POLL thermostat for %02X%02X", sreg[0], sreg[1]);

    PendingPoll spp;
    spp.sent_ms = millis();
    spp.dest = ADDR_THERMOSTAT;
    spp.reg_key = sreg_key;
    pending_polls_.push_back(spp);
    diag_reply_expected_++;

    std::vector<uint8_t> spayload = {0x00, sreg[0], sreg[1]};
    send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_READ, spayload);
    pending_polls_.back().tx_seq = diag_tx_seq_;

    slow_poll_index_++;
    last_slow_poll_time_ = now;
  }

  // Track loop duration
  uint32_t loop_ms = millis() - loop_start;
  if (loop_ms > diag_loop_max_ms_) {
    diag_loop_max_ms_ = loop_ms;
    if (loop_ms > 10) {
      ESP_LOGW("InfinitESP", "SLOW LOOP: %ums (uart_avail=%d at entry)", loop_ms, uart_available);
    }
  }
}

// --- Frame Parsing ---

void InfinitESPComponent::parse_byte_(uint8_t byte) {
  uint32_t now = millis();

  // Discard buffer if >100ms gap between bytes (stale data)
  if (!rx_buffer_.empty() && (now - last_rx_time_ > 100)) {
    // Log the stale data for analysis (truncate to 64 bytes)
    char hex_buf[64 * 3 + 1] = {};
    for (size_t i = 0; i < rx_buffer_.size() && i < 64; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", rx_buffer_[i]);
    }
    ESP_LOGW("InfinitESP", "STALE DISCARD seq=%u (%d bytes), gap=%ums data=[%s%s]",
             diag_rx_seq_, rx_buffer_.size(), now - last_rx_time_,
             hex_buf, rx_buffer_.size() > 64 ? "..." : "");
    rx_buffer_.clear();
    diag_stale_discard_++;
  }
  last_rx_time_ = now;

  rx_buffer_.push_back(byte);

  // Need at least header to determine frame size
  if (rx_buffer_.size() < FRAME_HEADER_SIZE + FRAME_CRC_SIZE)
    return;

  uint8_t payload_len = rx_buffer_[4];
  uint16_t expected_size = FRAME_HEADER_SIZE + payload_len + FRAME_CRC_SIZE;

  if (rx_buffer_.size() < expected_size)
    return;

  // Full frame received - validate and dispatch
  diag_rx_seq_++;

  // Track inter-frame timing
  uint32_t frame_now = millis();
  if (diag_last_frame_time_ > 0) {
    uint32_t gap = frame_now - diag_last_frame_time_;
    if (gap < diag_inter_frame_min_ms_) diag_inter_frame_min_ms_ = gap;
    if (gap > diag_inter_frame_max_ms_) diag_inter_frame_max_ms_ = gap;
  }
  diag_last_frame_time_ = frame_now;

  if (validate_frame_()) {
    diag_frames_parsed_++;
    dispatch_frame_();
  } else {
    diag_crc_fail_++;
    char hex_buf[64 * 3 + 1] = {};
    for (size_t i = 0; i < rx_buffer_.size() && i < 64; i++) {
      snprintf(hex_buf + i * 3, 4, "%02X ", rx_buffer_[i]);
    }
    ESP_LOGW("InfinitESP", "CRC FAIL seq=%u (%d bytes): [%s%s]",
             diag_rx_seq_, rx_buffer_.size(), hex_buf, rx_buffer_.size() > 64 ? "..." : "");
  }
  rx_buffer_.clear();
}

bool InfinitESPComponent::validate_frame_() {
  if (rx_buffer_.size() < FRAME_MIN_SIZE)
    return false;

  uint16_t frame_len = rx_buffer_.size();
  uint16_t computed = compute_crc_(rx_buffer_.data(), frame_len - 2);
  uint16_t received = rx_buffer_[frame_len - 2] | (rx_buffer_[frame_len - 1] << 8);
  return computed == received;
}

void InfinitESPComponent::dispatch_frame_() {
  // Parse frame fields
  current_frame_.dst = rx_buffer_[0];
  current_frame_.dst_bus = rx_buffer_[1];
  current_frame_.src = rx_buffer_[2];
  current_frame_.src_bus = rx_buffer_[3];
  current_frame_.length = rx_buffer_[4];
  current_frame_.pid = rx_buffer_[5];
  current_frame_.ext = rx_buffer_[6];
  current_frame_.func = rx_buffer_[7];
  current_frame_.payload.assign(rx_buffer_.begin() + FRAME_HEADER_SIZE,
                                rx_buffer_.end() - FRAME_CRC_SIZE);

  // Log parsed frame at INFO level with sequence number
  const char *func_name = "???";
  switch (current_frame_.func) {
    case FUNC_READ:      func_name = "READ"; break;
    case FUNC_REPLY:     func_name = "REPLY"; break;
    case FUNC_WRITE:     func_name = "WRITE"; break;
    case FUNC_EXCEPTION: func_name = "EXCEPTION"; break;
  }
  // Compact payload hex (limit to 32 bytes for log readability)
  size_t hex_len = current_frame_.payload.size() < 32 ? current_frame_.payload.size() : 32;
  char payload_hex[32 * 3 + 1] = {};
  for (size_t i = 0; i < hex_len; i++) {
    snprintf(payload_hex + i * 3, 4, "%02X ", current_frame_.payload[i]);
  }
  ESP_LOGV("InfinitESP", "RX#%u %02X[%d]->%02X[%d] %s len=%d plen=%d [%s%s],",
           diag_rx_seq_,
           current_frame_.src, current_frame_.src_bus,
           current_frame_.dst, current_frame_.dst_bus,
           func_name, current_frame_.length, current_frame_.payload.size(),
           payload_hex, current_frame_.payload.size() > 32 ? "..." : "");

  // Check if addressed to us
  bool to_us = (current_frame_.dst == address_);

  // Reply matching: if this is a REPLY addressed to us, check against pending polls
  if (current_frame_.func == FUNC_REPLY && to_us) {
    diag_reply_received_++;
    if (current_frame_.payload.size() >= 3) {
      uint16_t reply_reg = (current_frame_.payload[1] << 8) | current_frame_.payload[2];
      // Find matching pending poll (most recent match)
      bool matched = false;
      for (auto it = pending_polls_.rbegin(); it != pending_polls_.rend(); ++it) {
        if (it->dest == current_frame_.src && it->reg_key == reply_reg) {
          uint32_t rtt = millis() - it->sent_ms;
          ESP_LOGD("InfinitESP", "REPLY MATCHED: rx_seq=%u matched tx_seq=%u dest=%02X reg=%04X rtt=%ums",
                   diag_rx_seq_, it->tx_seq, it->dest, it->reg_key, rtt);
          // Erase by converting reverse iterator to forward iterator
          auto fwd = std::prev(it.base());
          pending_polls_.erase(fwd);
          matched = true;
          break;
        }
      }
      if (!matched) {
        ESP_LOGW("InfinitESP", "REPLY UNMATCHED: rx_seq=%u from=%02X reg=%04X (pending=%u)",
                 diag_rx_seq_, current_frame_.src, reply_reg, (uint32_t) pending_polls_.size());
      }
    }
  }

  // Handle replies from thermostat regardless of to_us flag.
  // Our poll responses have dst=our address (to_us=true), but the original
  // snooping path only checked the else branch. Thermostat may also send
  // replies addressed to us or to broadcast.
  if (current_frame_.src == ADDR_THERMOSTAT && current_frame_.func == FUNC_REPLY) {
    handle_reply_();
  }

  if (to_us) {
    switch (current_frame_.func) {
      case FUNC_READ:
        handle_read_request_();
        break;
      case FUNC_WRITE:
        handle_write_request_();
        break;
      default:
        break;
    }
  } else {
    handle_passive_frame_();
  }
}

void InfinitESPComponent::handle_passive_frame_() {
  // Passive snooping: capture REPLY frames from IDU (0x40) and ODU (0x52)
  // that the thermostat polls. We observe but don't initiate these transactions.
  if (current_frame_.func == FUNC_REPLY && current_frame_.payload.size() > 3) {
    uint8_t src_class = current_frame_.src >> 4;
    // Class 4 = Indoor Unit, Class 5 = Outdoor Unit
    if (src_class == 4 || src_class == 5) {
      uint8_t table = current_frame_.payload[1];
      uint8_t row = current_frame_.payload[2];
      uint16_t reg_key = (table << 8) | row;
      std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
      store_register_(current_frame_.src, reg_key, data);

      // Log decoded IDU data
      if (src_class == 4 && reg_key == REG_IDU_STATUS && data.size() >= 5) {
        uint16_t blower_rpm = ((uint16_t) data[1] << 8) | data[2];
        ESP_LOGD("InfinitESP", "IDU 0306: blower_rpm=%u", blower_rpm);
      }
      if (src_class == 4 && reg_key == REG_IDU_CONFIG && data.size() >= 8) {
        uint16_t airflow_cfm = ((uint16_t) data[4] << 8) | data[5];
        bool elec_heat = (data[0] & 0x03) != 0;
        ESP_LOGD("InfinitESP", "IDU 0316: airflow_cfm=%u elec_heat=%d", airflow_cfm, elec_heat);
      }

      // Log decoded ODU data
      if (src_class == 5 && reg_key == REG_ODU_STATUS2 && data.size() >= 1) {
        ESP_LOGD("InfinitESP", "ODU 0303: stage=%u raw=[%02X %02X %02X %02X]",
                 data[0] >> 1, data[0], data.size() > 1 ? data[1] : 0,
                 data.size() > 2 ? data[2] : 0, data.size() > 3 ? data[3] : 0);
      }
      if (src_class == 5 && reg_key == REG_ODU_COMP_SPEED && data.size() >= 2) {
        uint16_t comp_rpm = ((uint16_t) data[0] << 8) | data[1];
        ESP_LOGD("InfinitESP", "ODU 0604: compressor_rpm=%u (%u bytes)", comp_rpm, data.size());
      }
      if (src_class == 5 && reg_key == REG_ODU_DEMAND && data.size() >= 7) {
        ESP_LOGD("InfinitESP", "ODU 0608: demand=%u stage=%u modulation=%u raw=[%02X %02X %02X %02X %02X %02X %02X]",
                 data[3], data[5], data[6],
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
      }
      if (src_class == 5 && reg_key == REG_ODU_SETPOINT && data.size() >= 5) {
        ESP_LOGD("InfinitESP", "ODU 060B: setpoint=%u\xc2\xb0" "F raw=[%02X %02X %02X %02X %02X]",
                 data[4], data[0], data[1], data[2], data[3], data[4]);
      }
      if (src_class == 5 && reg_key == REG_ODU_FLOATS && data.size() >= 25) {
        ESP_LOGI("InfinitESP", "ODU 061f: sh_tgt=%.1f sh_act=%.1f sc_tgt=%.1f sc_act=%.1f dyn=%.1f unk=%.3f",
                 decode_f32_be_(data, 1), decode_f32_be_(data, 5),
                 decode_f32_be_(data, 9), decode_f32_be_(data, 13),
                 decode_f32_be_(data, 17), decode_f32_be_(data, 21));
      }

      // ODU register 0302: temperatures and thresholds (24 bytes = 12 int16 BE / 16)
      // Alternating (threshold, measurement): offsets 0,4,8,12,16,20 = constants;
      // offsets 2,6,10,14,18,22 = dynamic measurements.
      if (src_class == 5 && reg_key == REG_ODU_STATUS1 && data.size() >= 24) {
        ESP_LOGD("InfinitESP", "ODU 0302: outdoor=%.1f coil=%.1f suction=%.1f liquid=%.1f indoor_coil=%.1f discharge=%.1f",
                 decode_int16_f_(data, 2), decode_int16_f_(data, 6),
                 decode_int16_f_(data, 10), decode_int16_f_(data, 14),
                 decode_int16_f_(data, 18), decode_int16_f_(data, 22));
      }

      notify_devices_(current_frame_.src, reg_key);
    }
  }

  // Handle broadcast WRITE frames from thermostat (3B0E activity, 3B02 state/time, etc.)
  if (current_frame_.func == FUNC_WRITE && current_frame_.payload.size() >= 3) {
    uint8_t table = current_frame_.payload[1];
    uint8_t row = current_frame_.payload[2];
    uint16_t reg_key = (table << 8) | row;
    if (reg_key == REG_SAM_ACTIVITY) {
      ESP_LOGD("InfinitESP", "Activity notification from %02X", current_frame_.src);
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
        store_register_(current_frame_.src, reg_key, data);
        notify_devices_(current_frame_.src, reg_key);
      }
    }

    // Broadcast 3B02 state writes from thermostat (contains time, weekday, etc.)
    // Thermostat periodically broadcasts updated 3B02 data to all devices on the bus.
    // Since dst != our address, these land in handle_passive_frame_ instead of handle_reply_.
    // Mirror the data to our address so SAM reads get current time.
    if (reg_key == REG_SAM_STATE && current_frame_.src == ADDR_THERMOSTAT) {
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
        ESP_LOGD("InfinitESP", "Broadcast 3B02 state update (%u bytes)", data.size());
        store_register_(address_, reg_key, data);
        notify_devices_(address_, reg_key);
      }
    }
  }
}

// --- Frame Transmission ---

uint16_t InfinitESPComponent::compute_crc_(const uint8_t *data, uint16_t len) const {
  return crc16(data, len, 0x0000, 0xA001);
}

// --- Shared frame transmission ---

void InfinitESPComponent::transmit_frame_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus, uint8_t func,
                                          const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> frame;
  frame.reserve(FRAME_HEADER_SIZE + payload.size() + FRAME_CRC_SIZE);

  frame.push_back(dst);
  frame.push_back(dst_bus);
  frame.push_back(address_);
  frame.push_back(src_bus);
  frame.push_back(payload.size());
  frame.push_back(0x00);  // pid
  frame.push_back(0x00);  // ext
  frame.push_back(func);
  frame.insert(frame.end(), payload.begin(), payload.end());

  uint16_t crc = compute_crc_(frame.data(), frame.size());
  frame.push_back(crc & 0xFF);
  frame.push_back((crc >> 8) & 0xFF);

  diag_tx_seq_++;
  diag_total_tx_bytes_ += frame.size();

  uint32_t flush_start = millis();
  diag_in_tx_ = true;
  write_array(frame);
  flush();
  diag_in_tx_ = false;
  uint32_t flush_ms = millis() - flush_start;
  if (flush_ms > diag_tx_flush_max_ms_)
    diag_tx_flush_max_ms_ = flush_ms;
  if (flush_ms > 5)
    ESP_LOGW("InfinitESP", "SLOW FLUSH: %ums for %u bytes func=%02X to %02X",
             flush_ms, frame.size(), func, dst);

  last_tx_done_time_ = millis();
}

void InfinitESPComponent::send_frame_(uint8_t dst, uint8_t dst_bus, uint8_t func,
                                      const std::vector<uint8_t> &payload) {
  ESP_LOGV("InfinitESP", "TX#%u %02X->%02X func=%02X len=%d uart_avail=%d",
           diag_tx_seq_ + 1, address_, dst, func, payload.size(), available());
  transmit_frame_(dst, dst_bus, 0x01, func, payload);
}

void InfinitESPComponent::send_reply_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus,
                                      const std::vector<uint8_t> &payload) {
  ESP_LOGV("InfinitESP", "TX#%u REPLY to 0x%02X uart_avail=%d",
           diag_tx_seq_ + 1, dst, available());
  transmit_frame_(dst, dst_bus, src_bus, FUNC_REPLY, payload);
}

void InfinitESPComponent::send_exception_(uint8_t dst, uint8_t dst_bus, uint8_t src_bus,
                                           uint8_t table, uint8_t row, uint8_t code) {
  std::vector<uint8_t> payload = {0x00, table, row, code};
  transmit_frame_(dst, dst_bus, src_bus, FUNC_EXCEPTION, payload);
}

// --- Frame Handlers ---

void InfinitESPComponent::handle_read_request_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;

  ESP_LOGD("InfinitESP", "READ request for register %04X from %02X", reg_key, current_frame_.src);

  const std::vector<uint8_t> *reg_data = get_register(address_, reg_key);
  if (reg_data != nullptr) {
    // Build reply payload: [0x00, table, row] + register data
    std::vector<uint8_t> reply_payload = {0x00, table, row};
    reply_payload.insert(reply_payload.end(), reg_data->begin(), reg_data->end());

    // Debug: log model/serial fields from 0104 to verify register data before TX
    if (reg_key == REG_DEVICE_INFO && reg_data->size() >= 96) {
      ESP_LOGI("InfinitESP", "0104 model: %.*s", 20, (const char *) &(*reg_data)[64]);
      ESP_LOGI("InfinitESP", "0104 serial: %.*s", 24, (const char *) &(*reg_data)[96]);
    }

    send_reply_(current_frame_.src, current_frame_.src_bus, current_frame_.dst_bus, reply_payload);
  } else {
    send_exception_(current_frame_.src, current_frame_.src_bus, current_frame_.dst_bus, table, row, 0x04);
  }
}


void InfinitESPComponent::handle_write_request_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;

  ESP_LOGD("InfinitESP", "WRITE to register %04X from %02X (%d bytes)", reg_key, current_frame_.src,
           current_frame_.payload.size() - 3);

  // Store register data (skip the 3-byte header) under our own address
  if (current_frame_.payload.size() > 3) {
    std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
    // Protect our device identity from being overwritten
    if (reg_key != REG_DEVICE_INFO) {
      store_register_(address_, reg_key, data);
    }
  }

  // Send ACK reply (echo the original payload back)
  send_reply_(current_frame_.src, current_frame_.src_bus, current_frame_.dst_bus, current_frame_.payload);

  // Notify sub-platforms
  notify_devices_(address_, reg_key);
}

void InfinitESPComponent::handle_reply_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;

  ESP_LOGD("InfinitESP", "REPLY register %04X from thermostat (%d bytes)", reg_key,
           current_frame_.payload.size() - 3);

  // Mark bus online before notifying so binary sensors read correct state
  bool was_offline = !bus_online_;
  bus_online_ = true;
  last_reply_time_ = millis();

  // Store under the source device's address and notify
  if (current_frame_.payload.size() > 3) {
    std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
    store_register_(current_frame_.src, reg_key, data);

    // Mirror state/zones registers to SAM's own address so READ requests
    // to the SAM serve current values instead of stale defaults
    if (reg_key == REG_SAM_STATE || reg_key == REG_SAM_ZONES) {
      store_register_(address_, reg_key, data);
      // Log time fields from 3B02 for debugging
      if (reg_key == REG_SAM_STATE && data.size() >= REG3B02_MINUTES + 2) {
        uint16_t minutes = ((uint16_t) data[REG3B02_MINUTES] << 8) | (uint16_t) data[REG3B02_MINUTES + 1];
        ESP_LOGD("InfinitESP", "3B02 reply: weekday=%u minutes=%u (%02u:%02u)",
                 data[REG3B02_WEEKDAY], minutes, minutes / 60, minutes % 60);
      }
    }

    // Log parsed 0x4xxx register contents
    if (reg_key == REG_TSTAT_COMFORT && data.size() >= 35) {
      // Comfort profile: 5 activities x 7 bytes each (heat, cool, fan, 4x unknown)
      const char *names[] = {"home", "away", "sleep", "wake", "manual"};
      for (int i = 0; i < 5; i++) {
        uint8_t base = i * 7;
        uint8_t htsp = data[base + 0];
        uint8_t clsp = data[base + 1];
        uint8_t fan = data[base + 2];
        uint8_t rhtg = data[base + 3] >> 4;
        uint8_t rclg = data[base + 3] & 0x0F;
        ESP_LOGI("InfinitESP", "COMFORT %s: heat=%d°F cool=%d°F fan=%d rclg=%d rhtg=%d hum_vent=0x%02X unk=[%02X %02X]",
                 names[i], htsp, clsp, fan, rclg, rhtg, data[base + 4], data[base + 5], data[base + 6]);
      }
    }

    if (reg_key == REG_TSTAT_VACATION && data.size() >= 7) {
      ESP_LOGI("InfinitESP", "VACATION: min_temp=%d°F max_temp=%d°F fan=%d", data[0], data[1], data[2]);
    }

    if (reg_key == REG_TSTAT_WIFI && data.size() >= 71) {
      std::string mac = extract_cstr(data, 4);
      std::string ssid = extract_cstr(data, 24);
      std::string password = extract_cstr(data, 70);
      std::string hostname = extract_cstr(data, 139);
      ESP_LOGI("InfinitESP", "WIFI: mac=%s ssid=%s hostname=%s", mac.c_str(), ssid.c_str(), hostname.c_str());
      cache_wifi_credentials_(ssid, password);
    }

    if (reg_key == REG_TSTAT_DEALER && data.size() >= 70) {
      std::string name = extract_cstr(data, 0);
      std::string brand = extract_cstr(data, 50);
      std::string url = extract_cstr(data, 70);
      ESP_LOGI("InfinitESP", "DEALER: name=%s brand=%s url=%s", name.c_str(), brand.c_str(), url.c_str());
    }

    notify_devices_(current_frame_.src, reg_key);
  }

  // If bus just came online, explicitly notify bus status sensors
  if (was_offline) {
    notify_devices_(0, 0);
  }
}

// --- Polling ---

void InfinitESPComponent::poll_thermostat_() {
  const auto &reg = POLL_REGS[poll_index_ % POLL_REG_COUNT];
  uint16_t reg_key = (reg[0] << 8) | reg[1];
  std::vector<uint8_t> payload = {0x00, reg[0], reg[1]};
  ESP_LOGI("InfinitESP", "POLL thermostat for %02X%02X (pending=%u)", reg[0], reg[1], (uint32_t) pending_polls_.size());

  // Register the pending poll before sending (so reply matching works)
  PendingPoll pp;
  pp.tx_seq = diag_tx_seq_ + 1;  // will be assigned in send_frame_
  pp.sent_ms = millis();
  pp.dest = ADDR_THERMOSTAT;
  pp.reg_key = reg_key;
  pending_polls_.push_back(pp);
  diag_reply_expected_++;

  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_READ, payload);

  // Update the tx_seq since send_frame_ just incremented it
  pending_polls_.back().tx_seq = diag_tx_seq_;
  poll_index_++;
}

// --- Register Management ---

void InfinitESPComponent::store_register_(uint8_t addr, uint16_t key, const std::vector<uint8_t> &data) {
  device_registers_[addr][key] = data;
}

void InfinitESPComponent::notify_devices_(uint8_t device_addr, uint16_t register_key) {
  for (auto *device : devices_) {
    device->on_register_update(device_addr, register_key);
  }
}

const std::vector<uint8_t> *InfinitESPComponent::get_register(uint8_t addr, uint16_t key) const {
  auto dev_it = device_registers_.find(addr);
  if (dev_it == device_registers_.end())
    return nullptr;
  auto reg_it = dev_it->second.find(key);
  if (reg_it != dev_it->second.end())
    return &reg_it->second;
  return nullptr;
}

uint8_t InfinitESPComponent::get_zone_count() const {
  auto *state = get_register(address_, REG_SAM_STATE);
  if (state && !state->empty())
    return state->at(0);  // active_zones bitmask
  return 0;
}

void InfinitESPComponent::poll_register(uint8_t table, uint8_t row) {
  uint16_t reg_key = (table << 8) | row;
  ESP_LOGI("InfinitESP", "Manual poll: register %02X%02X", table, row);

  PendingPoll pp;
  pp.sent_ms = millis();
  pp.dest = ADDR_THERMOSTAT;
  pp.reg_key = reg_key;
  pending_polls_.push_back(pp);
  diag_reply_expected_++;

  std::vector<uint8_t> payload = {0x00, table, row};
  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_READ, payload);
  pending_polls_.back().tx_seq = diag_tx_seq_;
}

// --- Domain Methods ---

void InfinitESPComponent::set_zone_setpoint(uint8_t zone, uint8_t heat_sp, uint8_t cool_sp) {
  auto *zones_data = get_register(address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < 27)
    return;

  // Copy current zones data
  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  uint8_t flags = 0;
  if (heat_sp > 0) {
    data[REG3B03_HEAT_SETPOINTS + idx] = heat_sp;
    flags |= CHANGE_HEAT;
  }
  if (cool_sp > 0) {
    data[REG3B03_COOL_SETPOINTS + idx] = cool_sp;
    flags |= CHANGE_COOL;
  }

  // Write header: [0x00, 0x00, flags]
  // Build payload for 3B03 write to thermostat
  std::vector<uint8_t> write_data = data;
  // For writes, first 3 bytes are [active_zones=0, reserved=0, change_flags]
  // But data already has the read format; we need to set the write header

  // Actually, the write format replaces the first 3 bytes with [0x00, 0x00, flags]
  // We keep the rest of the data as-is (setpoints, fan modes, names, etc.)

  // Update local cache first
  data[1] = 0;  // reserved stays 0
  data[REG3B03_CHANGE_FLAGS] = 0;  // clear change flags in cache
  store_register_(address_, REG_SAM_ZONES, data);

  // Build write payload: [0x00, 0x3B, 0x03] + [0x00, 0x00, flags] + data[3:]
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, flags};
  payload.insert(payload.end(), write_data.begin() + 3, write_data.end());

  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload);
  ESP_LOGI("InfinitESP", "Set zone %d: heat=%d cool=%d flags=0x%02X", zone, heat_sp, cool_sp, flags);
}

void InfinitESPComponent::set_zone_fan(uint8_t zone, uint8_t fan_mode) {
  auto *zones_data = get_register(address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
    return;

  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  data[REG3B03_FAN_MODES + idx] = fan_mode;

  // Update local cache
  data[REG3B03_CHANGE_FLAGS] = 0;  // clear change flags
  store_register_(address_, REG_SAM_ZONES, data);

  // Build write payload
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, CHANGE_FAN};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload);
  ESP_LOGI("InfinitESP", "Set zone %d fan=%d", zone, fan_mode);
}

void InfinitESPComponent::set_zone_hold(uint8_t zone, uint16_t duration_minutes) {
  auto *zones_data = get_register(address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
    return;

  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  uint8_t hold_offset = REG3B03_HOLD_DURATIONS + (idx * 2);
  // Write as big-endian (UBInt16 in Infinitude parser)
  data[hold_offset] = (duration_minutes >> 8) & 0xFF;
  data[hold_offset + 1] = duration_minutes & 0xFF;

  // Set/clear zones_holding bitmask
  if (duration_minutes > 0) {
    data[REG3B03_ZONES_HOLDING] |= (1 << idx);
  } else {
    data[REG3B03_ZONES_HOLDING] &= ~(1 << idx);
  }

  // Update local cache
  data[REG3B03_CHANGE_FLAGS] = 0;
  store_register_(address_, REG_SAM_ZONES, data);

  // Use CHANGE_HOLD flag (0x02) + CHANGE_OVERRIDE flag (0x80)
  uint8_t flags = CHANGE_HOLD | CHANGE_OVERRIDE;
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, flags};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload);
  ESP_LOGI("InfinitESP", "Set zone %d hold=%d min (flags=0x%02X)", zone, duration_minutes, flags);
}

void InfinitESPComponent::apply_activity(uint8_t zone, uint8_t activity_index, uint16_t hold_duration) {
  // Look up comfort profile from 400A register data (stored under thermostat address)
  auto *comfort = get_register(ADDR_THERMOSTAT, REG_TSTAT_COMFORT);
  if (!comfort || comfort->size() < (activity_index + 1) * COMFORT_ENTRY_SIZE) {
    ESP_LOGW("InfinitESP", "apply_activity: no comfort profile data for activity %d", activity_index);
    return;
  }

  uint8_t base = activity_index * COMFORT_ENTRY_SIZE;
  uint8_t htsp = (*comfort)[base + 0];
  uint8_t clsp = (*comfort)[base + 1];
  uint8_t fan = (*comfort)[base + 2];

  const char *names[] = {"home", "away", "sleep", "wake", "manual"};
  ESP_LOGI("InfinitESP", "Apply activity %s to zone %d: heat=%d°F cool=%d°F fan=%d hold=%d min",
           activity_index < 5 ? names[activity_index] : "?", zone, htsp, clsp, fan, hold_duration);

  // Update the 3B03 zones data with the activity's setpoints and fan
  auto *zones_data = get_register(address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
    return;

  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  data[REG3B03_FAN_MODES + idx] = fan;
  data[REG3B03_HEAT_SETPOINTS + idx] = htsp;
  data[REG3B03_COOL_SETPOINTS + idx] = clsp;

  // Set hold duration
  uint8_t hold_offset = REG3B03_HOLD_DURATIONS + (idx * 2);
  data[hold_offset] = (hold_duration >> 8) & 0xFF;
  data[hold_offset + 1] = hold_duration & 0xFF;
  if (hold_duration > 0) {
    data[REG3B03_ZONES_HOLDING] |= (1 << idx);
  } else {
    data[REG3B03_ZONES_HOLDING] &= ~(1 << idx);
  }

  // Update local cache
  data[REG3B03_CHANGE_FLAGS] = 0;
  store_register_(address_, REG_SAM_ZONES, data);

  // Write with all change flags set (fan + hold + heat + cool + override)
  uint8_t flags = CHANGE_FAN | CHANGE_HOLD | CHANGE_HEAT | CHANGE_COOL | CHANGE_OVERRIDE;
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, flags};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload);
  ESP_LOGI("InfinitESP", "Applied activity %s zone %d flags=0x%02X",
           activity_index < 5 ? names[activity_index] : "?", zone, flags);
}

void InfinitESPComponent::set_system_mode(uint8_t mode) {
  auto *state_data = get_register(address_, REG_SAM_STATE);
  if (!state_data || state_data->size() < 29) {
    ESP_LOGW("InfinitESP", "Set system mode=%d FAILED: no cached 3B02 data", mode);
    return;
  }

  std::vector<uint8_t> data = *state_data;
  uint8_t old_stagmode = data[22];

  // stagmode at offset 22: high nibble=stage, low nibble=mode
  data[22] = (data[22] & 0xF0) | (mode & 0x0F);

  // Update local 3B02 cache so we can serve it when thermostat READs us
  store_register_(address_, REG_SAM_STATE, data);

  ESP_LOGI("InfinitESP", "Set system mode=%d (stagmode 0x%02X->0x%02X)", mode, old_stagmode, data[22]);

  // Two-pronged approach for reliable mode changes:
  // 1. 3B03 notification with system_mode flag (notify-pull pattern)
  // 2. Direct 3B02 write with change flags (push pattern)
  // Both are needed — the 3B03 notification primes the thermostat to accept
  // the mode change, and the 3B02 write delivers the actual data.

  // 3B03 notification with system_mode flag
  auto *zones_data = get_register(address_, REG_SAM_ZONES);
  if (zones_data && zones_data->size() >= 11) {
    std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, CHANGE_MODE};
    payload.insert(payload.end(), zones_data->begin() + 3, zones_data->end());
    send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload);
  }

  // Direct 3B02 write with updated stagmode
  {
    std::vector<uint8_t> payload_3b02 = {0x00, 0x3B, 0x02, 0x00, 0x00, CHANGE_MODE};
    payload_3b02.insert(payload_3b02.end(), data.begin() + 3, data.end());
    send_frame_(ADDR_THERMOSTAT, 0x01, FUNC_WRITE, payload_3b02);
  }
}

// --- Default Register Initialization ---

void InfinitESPComponent::initialize_defaults_() {
  // Register 0104 - Device info (120 bytes)
  {
    auto pad_str = [](std::vector<uint8_t> &buf, const char *str, size_t width) {
      size_t slen = strlen(str);
      for (size_t i = 0; i < width; i++)
        buf.push_back(i < slen ? (uint8_t) str[i] : 0x00);
    };
    std::vector<uint8_t> data;
    data.reserve(120);
    pad_str(data, "SYSTEM ACCESS MODULE", 24);    // device
    pad_str(data, "", 24);                        // location
    pad_str(data, __DATE__, 16);                  // software (auto build date)
    pad_str(data, "InfinitESP--SAM", 20);         // model
    pad_str(data, "", 12);                        // reference
    pad_str(data, "ESP32SAM01", 24);              // serial
    store_register_(address_, REG_DEVICE_INFO, data);
  }

  // Register 030D - SAM status (7 bytes)
  {
    std::vector<uint8_t> data = {0x3D, 0x3E, 0x3F, 0, 0, 0, 0};
    store_register_(address_, REG_SAM_STATUS, data);
  }

  // Register 3B02 - System state (29 bytes)
  {
    std::vector<uint8_t> data(REG3B02_SIZE, 0);
    data[REG3B02_ACTIVE_ZONES] = 0x01;                          // zone 1
    for (int i = 0; i < 8; i++) data[REG3B02_TEMPS + i] = 70;   // 70°F
    for (int i = 0; i < 8; i++) data[REG3B02_HUMIDITY + i] = 50; // 50%
    data[REG3B02_OUTDOOR_TEMP] = 70;                             // 70°F
    data[REG3B02_STAGMODE] = 0x04;                               // stage=0, mode=off
    data[REG3B02_WEEKDAY] = 0x01;                                // Monday
    data[REG3B02_MINUTES] = 0x01;                                // 480 (8:00am) BE high byte
    data[REG3B02_MINUTES + 1] = 0xE0;                            // BE low byte
    data[REG3B02_DISPLAYED_ZONE] = 0x01;
    store_register_(address_, REG_SAM_STATE, data);
  }

  // Register 3B03 - Zone settings (150 bytes)
  {
    std::vector<uint8_t> data(REG3B03_SIZE, 0);
    data[REG3B03_ACTIVE_ZONES] = 0x01;                                     // zone 1
    data[REG3B03_CHANGE_FLAGS] = 0x00;
    // fan_mode[8]: all auto (0)
    for (int i = 0; i < 8; i++) data[REG3B03_HEAT_SETPOINTS + i] = 68;    // 68°F
    for (int i = 0; i < 8; i++) data[REG3B03_COOL_SETPOINTS + i] = 76;    // 76°F
    for (int i = 0; i < 8; i++) data[REG3B03_HUMIDITY_SETPOINTS + i] = 50; // 50%
    // hold_duration[8]: all 0
    const char *zone_names[] = {"Zone 1", "Zone 2", "Zone 3", "Zone 4",
                                "Zone 5", "Zone 6", "Zone 7", "Zone 8"};
    for (int z = 0; z < 8; z++) {
      uint16_t name_offset = REG3B03_ZONE_NAMES + (z * 12);
      strncpy(reinterpret_cast<char *>(&data[name_offset]), zone_names[z], 12);
    }
    store_register_(address_, REG_SAM_ZONES, data);
  }

  // Register 3B04 - Vacation settings (11 bytes)
  {
    std::vector<uint8_t> data(11, 0);
    data[0] = 0;    // active: off
    data[1] = 0;    // hours low
    data[2] = 0;    // hours high
    data[3] = 60;   // min_temp: 60F
    data[4] = 85;   // max_temp: 85F
    data[5] = 0;    // min_humidity
    data[6] = 100;  // max_humidity: 100%
    data[7] = 0;    // fan_mode: auto
    store_register_(address_, REG_SAM_VACATION, data);
  }

  // Register 3B05 - Accessories (11 bytes)
  {
    std::vector<uint8_t> data(11, 0);
    data[3] = 0;   // filter_consumption
    data[4] = 0;   // uv_consumption
    data[5] = 0;   // humidifier_consumption
    data[6] = 0;   // ventilator_consumption
    data[7] = 0;   // filter_reminders: off
    data[8] = 0;   // uv_reminders: off
    data[9] = 0;   // humidifier_reminders: off
    data[10] = 0;  // ventilator_reminders: off
    store_register_(address_, REG_SAM_ACCESSORIES, data);
  }

  // Register 3B06 - Dealer info (52 bytes)
  {
    std::vector<uint8_t> data(52, 0);
    data[0] = 8;    // backlight
    data[1] = 1;    // auto_mode
    data[2] = 0;    // unknown1
    data[3] = 3;    // deadband
    data[4] = 4;    // cycles_per_hour
    data[5] = 4;    // schedule_periods
    data[6] = 1;    // programs_enabled
    data[7] = 0x46; // temp_units: 'F'
    data[8] = 0xFF; // unknown2
    data[9] = 1;    // unknown_padding[0]
    // dealer_name at offset 12: 20 bytes
    strncpy(reinterpret_cast<char *>(&data[12]), "InfinitESP", 20);
    // dealer_phone at offset 32: 20 bytes (all zeros)
    store_register_(address_, REG_SAM_DEALER, data);
  }

  // Register 3B0E - Activity flag (1 byte)
  {
    std::vector<uint8_t> data = {0};
    store_register_(address_, REG_SAM_ACTIVITY, data);
  }

  ESP_LOGI("InfinitESP", "Initialized %d SAM registers at address 0x%02X",
           device_registers_[address_].size(), address_);
}

void InfinitESPComponent::cache_wifi_credentials_(const std::string &ssid, const std::string &password) {
  if (ssid.empty())
    return;

  // Only write to NVS if credentials have changed
  if (ssid == cached_wifi_ssid_ && password == cached_wifi_password_) {
    ESP_LOGD("InfinitESP", "WiFi cache unchanged (SSID=%s)", ssid.c_str());
    return;
  }

  cached_wifi_ssid_ = ssid;
  cached_wifi_password_ = password;
  wifi_cache_dirty_ = true;

  CachedWifi cached{};
  strncpy(cached.ssid, ssid.c_str(), sizeof(cached.ssid) - 1);
  strncpy(cached.password, password.c_str(), sizeof(cached.password) - 1);

  if (wifi_pref_.save(&cached)) {
    global_preferences->sync();
    ESP_LOGI("InfinitESP", "Cached WiFi credentials: SSID=%s (saved to NVS)", ssid.c_str());
  } else {
    ESP_LOGW("InfinitESP", "Failed to save WiFi credentials to NVS");
  }
}

void InfinitESPComponent::inject_cached_wifi_() {
  if (cached_wifi_ssid_.empty())
    return;

#ifdef USE_WIFI
  auto *wifi = wifi::global_wifi_component;
  if (wifi == nullptr)
    return;

  if (wifi->is_connected()) {
    ESP_LOGD("InfinitESP", "WiFi already connected, skipping cached credential injection");
    return;
  }

  ESP_LOGI("InfinitESP", "Injecting cached WiFi credentials: SSID=%s (WiFi not connected)",
           cached_wifi_ssid_.c_str());
  wifi->save_wifi_sta(cached_wifi_ssid_, cached_wifi_password_);
#else
  ESP_LOGD("InfinitESP", "WiFi component not available, cannot inject cached credentials");
#endif
}

void InfinitESPComponent::update_status_led_() {
  // Skip if no status LED is configured at all
#if !defined(USE_INFINITESP_STATUS_LED_PIN) && !defined(USE_INFINITESP_STATUS_LIGHT)
  return;
#else
  // Determine subsystem states
  bool wifi_connected = false;
#ifdef USE_WIFI
  wifi_connected = wifi::global_wifi_component != nullptr &&
                   wifi::global_wifi_component->is_connected();
#endif

  // Sequential: bus first, then wifi. LED shows which step we're stuck on.
  StatusLedState new_state = StatusLedState::BUS_NOT_READY;
  if (bus_online_ && wifi_connected)
    new_state = StatusLedState::ALL_GOOD;
  else if (bus_online_)
    new_state = StatusLedState::WIFI_NOT_READY;
  // else: bus not ready, regardless of wifi state

  uint32_t now = millis();

  // --- Pin mode: simple digital on/off ---
#ifdef USE_INFINITESP_STATUS_LED_PIN
  if (status_led_gpio_ != nullptr) {
    bool target = false;

    switch (new_state) {
      case StatusLedState::ALL_GOOD:
        target = true;  // solid on
        break;
      case StatusLedState::WIFI_NOT_READY:
        // Fast blink: 250ms period
        target = ((now / 250) % 2) == 0;
        break;
      case StatusLedState::BUS_NOT_READY:
        // Slow blink: 1s period
        target = ((now / 1000) % 2) == 0;
        break;
    }

    if (target != status_led_physical_) {
      status_led_physical_ = target;
      status_led_gpio_->digital_write(target);
    }
  }
#endif

  // --- Light mode: ESPHome light API with RGB support ---
#ifdef USE_INFINITESP_STATUS_LIGHT
  if (status_light_ != nullptr) {
    bool state_changed = (new_state != status_led_state_);

    // For solid states, throttle to 500ms. For blink states, update every 100ms.
    if (state_changed || (new_state == StatusLedState::BUS_NOT_READY)) {
      if (!state_changed && (now - status_light_last_update_ < 100))
        return;
    } else {
      if (now - status_light_last_update_ < 500)
        return;
    }

    switch (new_state) {
      case StatusLedState::ALL_GOOD:
        // Solid green — everything connected
        if (state_changed) {
          if (status_light_has_rgb_) {
            status_light_->turn_on()
                .set_color_mode(light::ColorMode::RGB)
                .set_rgb(0.0f, 1.0f, 0.0f)
                .set_brightness(0.32f)
                .set_transition_length_if_supported(500)
                .perform();
          } else {
            status_light_->turn_on()
                .set_brightness(0.32f)
                .set_transition_length_if_supported(500)
                .perform();
          }
        }
        break;

      case StatusLedState::WIFI_NOT_READY:
        // Blue blink — bus up, wifi not connected
        if (status_light_has_rgb_) {
          bool phase = ((now / 500) % 2) == 0;
          if (phase) {
            status_light_->turn_on()
                .set_color_mode(light::ColorMode::RGB)
                .set_rgb(0.0f, 0.4f, 1.0f)
                .set_brightness(0.32f)
                .set_transition_length_if_supported(200)
                .perform();
          } else {
            status_light_->turn_off()
                .set_transition_length_if_supported(200)
                .perform();
          }
        } else {
          bool phase = ((now / 250) % 2) == 0;
          if (phase) {
            status_light_->turn_on().set_brightness(0.32f).perform();
          } else {
            status_light_->turn_off().perform();
          }
        }
        break;

      case StatusLedState::BUS_NOT_READY:
        // Yellow blink — bus not fully online yet
        if (status_light_has_rgb_) {
          bool phase = ((now / 1000) % 2) == 0;
          if (phase) {
            status_light_->turn_on()
                .set_color_mode(light::ColorMode::RGB)
                .set_rgb(1.0f, 0.8f, 0.0f)
                .set_brightness(0.32f)
                .set_transition_length_if_supported(300)
                .perform();
          } else {
            status_light_->turn_off()
                .set_transition_length_if_supported(300)
                .perform();
          }
        } else {
          bool phase = ((now / 1000) % 2) == 0;
          if (phase) {
            status_light_->turn_on().set_brightness(0.32f).perform();
          } else {
            status_light_->turn_off().perform();
          }
        }
        break;
    }

    status_led_state_ = new_state;
    status_light_last_update_ = now;
  }
#endif

  status_led_state_ = new_state;
#endif  // has at least one status LED mode
}

}  // namespace infinitesp
}  // namespace esphome
