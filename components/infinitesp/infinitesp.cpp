#include "infinitesp.h"
#include "esphome/components/sensor/sensor.h"

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
    {0x42, 0x02},  // fault history (10 entries × 7 bytes)
    {0x46, 0x08},  // WiFi: SSID, password, hostname
    {0x46, 0x09},  // Cloud: host, device IP
    {0x46, 0x0A},  // dealer info: name, brand, URL
};
static const uint8_t SLOW_POLL_REG_COUNT = 6;
static const uint32_t SLOW_POLL_INTERVAL_MS = 31000;  // poll every 31s (prime, avoids beating with other timers)
// Table-name discovery: probe one observed (device, table) 0xNN01 per cycle.
// Conservative cadence to stay off the thermostat's bus schedule.
static const uint32_t DISCOVERY_POLL_INTERVAL_MS = 3500;
static const uint8_t TABLEDEF_ROW = 0x01;  // every table's self-describing register is at row 01

// Write retransmit delay. Each WRITE is re-sent once after this interval to
// ride through sporadic drops. Must stay <= PENDING_SETPOINT_WINDOW_MS/2
// (climate entity overlay) so a retransmit lands inside the newest change's
// overlay window — keeps the last-sent value winning on the bus and the UI
// free of snapback under rapid changes.
static const uint32_t RETRANSMIT_DELAY_MS = 4000;

void InfinitESPComponent::setup() {
  ESP_LOGI("InfinitESP", "InfinitESP v0.1.0 build %s %s", __DATE__, __TIME__);
  ESP_LOGI("InfinitESP", "SAM Address=0x%02X", sam_address_);
  if (zc_enabled())
    ESP_LOGI("InfinitESP", "Zone Controller emulation at 0x%02X", zc_address_);

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
  if (sam_enabled()) {
    for (auto &kv : device_registers_[sam_address_]) {
      notify_entities_(sam_address_, kv.first);
    }
  }

  // Initialize status LED (if configured via YAML)
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  if (flow_control_pin_ != nullptr) {
    flow_control_pin_->setup();
    flow_control_pin_->digital_write(false);
    ESP_LOGI("InfinitESP", "TX enable pin: configured");
  }
#endif

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

  // NOTE: no echo suppression needed. RS485 auto-direction transceivers disable
  // RX during TX, so the UART RX buffer is empty after transmission — there is no
  // echo to drain. A previous 60ms drain window was removed because it was
  // discarding legitimate bus traffic (replies from other devices that arrived
  // within 60ms of our TX), causing ~50% poll timeouts and cascading CRC failures
  // from partial frames that straddled the drain boundary.

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

  // Read all available bytes from primary UART (ABCD bus)
  while (available()) {
    uint8_t byte;
    read_byte(&byte);
    diag_total_rx_bytes_++;
    if (rx_hex_log_.size() < 512)
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
             "tx_flush_max=%ums loop_max=%ums inter_frame=%u..%ums",
             diag_total_rx_bytes_, diag_total_tx_bytes_, diag_frames_parsed_, diag_tx_seq_,
             diag_crc_fail_, diag_stale_discard_, diag_uart_hwm_, diag_uart_overflow_events_,
             diag_reply_expected_, diag_reply_received_, diag_reply_timeout_,
             (uint32_t) pending_polls_.size(),
             diag_tx_flush_max_ms_, diag_loop_max_ms_,
             diag_inter_frame_min_ms_ == UINT32_MAX ? 0 : diag_inter_frame_min_ms_,
             diag_inter_frame_max_ms_);
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
    notify_entities_(0, 0);
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

  // Drain due write retransmit (one per iteration, bus-idle gated). Suppresses
  // the fast/slow polls this iteration to avoid back-to-back TX to the thermostat.
  bool retransmit_sent = false;
  if (!pending_retransmits_.empty() && bus_idle_ms > 50 &&
      (int32_t) (pending_retransmits_.front().fire_ms - now) <= 0) {
    auto &r = pending_retransmits_.front();
    uint16_t rk = r.payload.size() >= 3 ? (uint16_t) ((r.payload[1] << 8) | r.payload[2]) : 0;
    ESP_LOGI("InfinitESP", "Retransmit WRITE %04X (+%ums, %u queued)",
             rk, RETRANSMIT_DELAY_MS, (uint32_t) pending_retransmits_.size());
    send_frame_(r.dst, r.dst_bus, r.func, r.payload);
    pending_retransmits_.pop_front();
    retransmit_sent = true;
  }

  bool fast_poll_sent = false;
  if (sam_enabled() && !retransmit_sent && (now - last_poll_time_ > 3000) && bus_idle_ms > 50) {
    poll_thermostat_();
    last_poll_time_ = now;
    fast_poll_sent = true;
  }

  // Slow-poll 0x4xxx thermostat config registers on their own schedule.
  // MUST NOT fire in the same loop iteration as the fast poll — sending
  // two READ frames to the same thermostat back-to-back causes the echo
  // drain to eat one of the replies (observed 44% poll timeout rate).
  if (sam_enabled() && !fast_poll_sent && !retransmit_sent && bus_online_ &&
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

  // Table-name discovery: probe one observed device's 0xNN01 register from
  // ADDR_FAKESAM (0x93) when 0x93 is free (not our SAM address). One query
  // per cycle, never in the same iteration as a thermostat poll.
  if (sam_address_ != ADDR_FAKESAM && bus_online_ &&
      !fast_poll_sent && !retransmit_sent &&
      (now - last_discovery_poll_ms_ >= DISCOVERY_POLL_INTERVAL_MS) && bus_idle_ms > 50) {
    poll_discovery_();
    last_discovery_poll_ms_ = now;
  }

  // Metric-units poll (AUTO mode, NOT emulating the SAM): the thermostat
  // doesn't push 3B06 without a SAM, so poll its 3B05 as FakeSAM. Once on
  // first RX activity, then every 5 min in case the user changes the unit.
  // When emulating the SAM, the tstat's 3B06 push is the source (no poll).
  // NOTE: gated on last_rx_time_ (frames being seen), NOT bus_online_ — that
  // flag only flips for replies addressed to us, which never happens in a
  // pure-passive (no-emulation) config.
  bool bus_active = (now - last_rx_time_) < 5000;
  if (temperature_unit_ == TemperatureUnit::AUTO && !sam_enabled() &&
      sam_address_ != ADDR_FAKESAM && bus_active &&
      !fast_poll_sent && !retransmit_sent && bus_idle_ms > 50 &&
      (!metric_units_known_ || (now - last_unit_poll_ms_) > 300000) &&
      (now - last_unit_poll_ms_) > 5000) {
    poll_metric_units_();
    last_unit_poll_ms_ = now;
  }

  // ZC sensor staleness fallback: check every 10s
  if (zc_enabled() && (now - last_zc_sensor_check_ > 10000)) {
    last_zc_sensor_check_ = now;
    check_zc_sensor_fallback_();
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

  // Check if addressed to us (SAM or optional zone controller)
  bool to_us = (sam_enabled() && current_frame_.dst == sam_address_);
  bool to_zc = is_emu_zc_addr_(current_frame_.dst);

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
  // replies addressed to us or to broadcast. Discovery replies (dst=0x93)
  // are handled separately below, so exclude them here.
  if (current_frame_.src == ADDR_THERMOSTAT && current_frame_.func == FUNC_REPLY &&
      current_frame_.dst != ADDR_FAKESAM) {
    handle_reply_();
  }

  if (current_frame_.func == FUNC_REPLY && current_frame_.dst == ADDR_FAKESAM) {
    handle_discovery_reply_();
  } else if (to_us || to_zc) {
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

  // Log all RX frames to traffic capture
  if (current_frame_.payload.size() >= 3) {
    log_traffic_(current_frame_.src, current_frame_.dst, current_frame_.func,
                (current_frame_.payload[1] << 8) | current_frame_.payload[2],
                current_frame_.payload);
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

      // Log decoded IDU data via accessors (single source of truth for offsets)
      if (src_class == 4 && reg_key == REG_IDU_STATUS) {
        float blower_rpm = idu_blower_rpm_(data);
        if (!std::isnan(blower_rpm))
          ESP_LOGD("InfinitESP", "IDU 0306: blower_rpm=%u", (unsigned) blower_rpm);
      }
      if (src_class == 4 && reg_key == REG_IDU_CONFIG) {
        float airflow_cfm = idu_airflow_cfm_(data);
        if (!std::isnan(airflow_cfm))
          ESP_LOGD("InfinitESP", "IDU 0316: airflow_cfm=%u elec_heat=%d",
                   (unsigned) airflow_cfm, (int) idu_electric_heat_(data));
      }

      // Log decoded ODU data
      if (src_class == 5 && reg_key == REG_ODU_STATUS2 && data.size() >= 1) {
        ESP_LOGD("InfinitESP", "ODU 0303: stage=%u raw=[%02X %02X %02X %02X]",
                 data[0] >> 1, data[0], data.size() > 1 ? data[1] : 0,
                 data.size() > 2 ? data[2] : 0, data.size() > 3 ? data[3] : 0);
      }
      if (src_class == 5 && reg_key == REG_ODU_COMP_SPEED) {
        float target = odu_compressor_target_rpm_(data);
        float actual = odu_compressor_actual_rpm_(data);
        if (!std::isnan(target) && !std::isnan(actual))
          ESP_LOGD("InfinitESP", "ODU 0604: target_rpm=%u actual_rpm=%u (%u bytes)",
                   (unsigned) target, (unsigned) actual, data.size());
      }
      if (src_class == 5 && reg_key == REG_ODU_DEMAND && data.size() >= 7) {
        ESP_LOGD("InfinitESP", "ODU 0608: compressor_frequency=%.1f Hz expansion_valve=%.0f%% raw=[%02X %02X %02X %02X %02X %02X %02X]",
                 odu_compressor_frequency_(data),
                 odu_expansion_valve_(data),
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
      }
      if (src_class == 5 && reg_key == REG_ODU_STAGE_INFO && data.size() >= 1) {
        ESP_LOGD("InfinitESP", "ODU 060e: stage=%u raw=[%02X]",
                 (unsigned) odu_stage_(data), data[0]);
      }

      if (src_class == 5 && reg_key == REG_ODU_FLOATS && data.size() >= 25) {
        ESP_LOGI("InfinitESP", "ODU 061f: sh_tgt=%.1f sh_act=%.1f sc_tgt=%.1f sc_act=%.1f dyn=%.1f unk=%.3f",
                 odu_float_(data, 1), odu_float_(data, 2),
                 odu_float_(data, 3), odu_float_(data, 4),
                 odu_float_(data, 5), odu_float_(data, 6));
      }

      // ODU register 0302: temperatures and thresholds (24 bytes = 12 int16 BE / 16)
      // Alternating (threshold, measurement): offsets 0,4,8,12,16,20 = constants;
      // offsets 2,6,10,14,18,22 = dynamic measurements (accessor idx 0..5).
      if (src_class == 5 && reg_key == REG_ODU_STATUS1 && data.size() >= 24) {
        ESP_LOGD("InfinitESP", "ODU 0302: outdoor=%.1f coil=%.1f suction=%.1f liquid=%.1f indoor_amb=%.1f discharge=%.1f",
                 odu_status1_meas_f_(data, 0), odu_status1_meas_f_(data, 1),
                 odu_status1_meas_f_(data, 2), odu_status1_meas_f_(data, 3),
                 odu_status1_meas_f_(data, 4), odu_status1_meas_f_(data, 5));
      }

      notify_entities_(current_frame_.src, reg_key);
    }

    // Zone Controller (class 6, 0x60/0x61) replies — e.g. zone status (0302).
    // Passive monitoring only: when we emulate the ZC, dispatch routes its
    // traffic to handle_read/write_request_ and our own TX never echoes back
    // (auto-direction RS485 disables RX during TX), so this branch only fires
    // for real physical ZCs. Multi-ZC: capture each controller under its real
    // source address (0x60 serves zones 1-4, 0x61 serves zones 5-8) so the
    // per-zone cover/climate/sensor entities read the correct register.
    if (!zc_enabled() && src_class == 6) {
      uint8_t table = current_frame_.payload[1];
      uint8_t row = current_frame_.payload[2];
      uint16_t zc_key = (table << 8) | row;
      std::vector<uint8_t> zc_data(current_frame_.payload.begin() + 3,
                                    current_frame_.payload.end());
      uint8_t zc_src = current_frame_.src;  // 0x60 or 0x61
      store_register_(zc_src, zc_key, zc_data);
      // Issue #9 diagnostic: dump the FULL payload (all bytes) for registers we
      // care about, so we can see where (if anywhere) the secondary controller
      // (0x61) hides its damper data. The 4-byte prints masked bytes 4-7.
      if (zc_key == REG_ZC_ZONE_CONFIG || zc_key == REG_ZC_DAMPER_CMD ||
          zc_key == REG_DEVICE_INFO || zc_key == 0x030D) {
        char hex[3 * 16 + 1];
        hex[0] = '\0';
        for (size_t i = 0; i < zc_data.size() && i < 16; i++) {
          char b[4];
          snprintf(b, sizeof(b), "%02X ", zc_data[i]);
          strlcat(hex, b, sizeof(hex));
        }
        const char *label =
            zc_key == REG_ZC_ZONE_CONFIG ? "damper state (0319)" :
            zc_key == REG_ZC_DAMPER_CMD   ? "damper cmd (0308)"    :
            zc_key == REG_DEVICE_INFO      ? "device info (0104)"   :
                                             "030D";
        ESP_LOGD("InfinitESP", "ZC %02X %s [%u bytes]: %s",
                 zc_src, label, zc_data.size(), hex);
        // Parse 0104 model/serial for the real (passively captured) controller
        // so we can confirm what device actually answers at 0x61.
        if (zc_key == REG_DEVICE_INFO && zc_data.size() >= 120) {
          std::string model(zc_data.begin() + 64, zc_data.begin() + 84);
          std::string serial(zc_data.begin() + 96, zc_data.begin() + 120);
          ESP_LOGI("InfinitESP", "ZC %02X 0104 model: '%s' serial: '%s'",
                   zc_src, model.c_str(), serial.c_str());
        }
      } else {
        ESP_LOGD("InfinitESP", "ZC %02X %04X reply captured (%u bytes)",
                 zc_src, zc_key, zc_data.size());
      }
      notify_entities_(zc_src, zc_key);
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
        notify_entities_(current_frame_.src, reg_key);
      }
    }

    // ZC damper command (0308) written by the thermostat to a real physical
    // zone controller (0x60 or 0x61). When we emulate the ZC this frame is
    // routed to handle_write_request_ and never reaches here; this branch only
    // fires for a physical ZC. 0308 is a SYSTEM-WIDE 8-byte payload (one byte
    // per system zone 1-8) written IDENTICALLY to BOTH controllers: 0x60 acts
    // on bytes 0-3, 0x61 on bytes 4-7. Store the FULL payload under each
    // controller's real address so a zone-N cover reads byte N-1 from its
    // serving controller. (issue #9: the prior 4-byte slice made zones 5-8
    // alias zones 1-4.)
    if (!zc_enabled() && (current_frame_.dst >> 4) == 6 &&
        reg_key == REG_ZC_DAMPER_CMD) {
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> damper(current_frame_.payload.begin() + 3,
                                     current_frame_.payload.end());
        uint8_t zc_dst = current_frame_.dst;  // 0x60 or 0x61
        store_register_(zc_dst, REG_ZC_DAMPER_CMD, damper);
        // Diagnostic: dump the full 0308 payload so the 8-byte system-wide
        // layout (bytes 4-7 = zones 5-8) is visible in logs.
        {
          char hex[3 * 32 + 1];
          hex[0] = '\0';
          const auto &pl = current_frame_.payload;
          for (size_t i = 0; i < pl.size() && i < 32; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%02X ", pl[i]);
            strlcat(hex, b, sizeof(hex));
          }
          ESP_LOGD("InfinitESP", "ZC %02X damper cmd (0308) [%u bytes]: %s",
                   zc_dst, pl.size(), hex);
        }
        notify_entities_(zc_dst, REG_ZC_DAMPER_CMD);
        notify_entities_(zc_dst, REG_ZC_ZONE_CONFIG);
      }
    }

    // Broadcast 3B02 state writes from thermostat (contains time, weekday, etc.)
    // Thermostat periodically broadcasts updated 3B02 data to all devices on the bus.
    // Since dst != our address, these land in handle_passive_frame_ instead of handle_reply_.
    // Mirror the data to SAM address so climate/sensor entities get current values.
    if (sam_enabled() && reg_key == REG_SAM_STATE && current_frame_.src == ADDR_THERMOSTAT) {
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
        ESP_LOGD("InfinitESP", "Broadcast 3B02 state update (%u bytes)", data.size());
        mirror_to_sam_(reg_key, data);
        notify_entities_(sam_address_, reg_key);
      }
    }

    // Capture thermostat→ODU writes. The ODU never replies to these (write-only
    // registers like 060b setpoint and 0605 commanded stage), so passive write
    // capture is the only source. Stored under dst (ODU address) so ODU sensors
    // match on bus_class 5. Write rows (0x0605/060b/0610/0612/061a/061d/061e)
    // do not collide with reply rows (0x0602/0604/0608/060a/060e/061f/0625).
    if (current_frame_.dst >> 4 == 5 && current_frame_.src == ADDR_THERMOSTAT &&
        current_frame_.payload.size() > 3) {
      std::vector<uint8_t> odu_data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
      store_register_(current_frame_.dst, reg_key, odu_data);
      if (reg_key == REG_ODU_CMD_STAGE && odu_data.size() >= 4)
        ESP_LOGD("InfinitESP", "ODU 0605 write: commanded_stage=%.1f", (double) odu_commanded_stage_(odu_data));
      notify_entities_(current_frame_.dst, reg_key);
    }
  }
}

// --- Frame Transmission ---

uint16_t InfinitESPComponent::compute_crc_(const uint8_t *data, uint16_t len) const {
  return crc16(data, len, 0x0000, 0xA001);
}

// --- Shared frame transmission ---

void InfinitESPComponent::transmit_frame_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus, uint8_t func,
                                          const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> frame;
  frame.reserve(FRAME_HEADER_SIZE + payload.size() + FRAME_CRC_SIZE);

  frame.push_back(dst);
  frame.push_back(dst_bus);
  frame.push_back(src);
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

  // Log TX frames to traffic capture
  if (payload.size() >= 3) {
    log_traffic_(src, dst, func, (payload[1] << 8) | payload[2], payload);
  }

  uint32_t flush_start = millis();
  diag_in_tx_ = true;
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  if (flow_control_pin_ != nullptr)
    flow_control_pin_->digital_write(true);
#endif
  write_array(frame);
  flush();
#ifdef USE_INFINITESP_FLOW_CONTROL_PIN
  if (flow_control_pin_ != nullptr)
    flow_control_pin_->digital_write(false);
#endif
  diag_in_tx_ = false;
  uint32_t flush_ms = millis() - flush_start;
  if (flush_ms > diag_tx_flush_max_ms_)
    diag_tx_flush_max_ms_ = flush_ms;
  if (flush_ms > 5)
    ESP_LOGW("InfinitESP", "SLOW FLUSH: %ums for %u bytes func=%02X to %02X",
             flush_ms, frame.size(), func, dst);
}

void InfinitESPComponent::send_frame_(uint8_t dst, uint8_t dst_bus, uint8_t func,
                                      const std::vector<uint8_t> &payload) {
  ESP_LOGV("InfinitESP", "TX#%u %02X->%02X func=%02X len=%d uart_avail=%d",
           diag_tx_seq_ + 1, sam_address_, dst, func, payload.size(), available());
  transmit_frame_(dst, dst_bus, sam_address_, 0x01, func, payload);
}

void InfinitESPComponent::send_write_frame_(uint8_t dst, uint8_t dst_bus,
                                             const std::vector<uint8_t> &payload) {
  send_frame_(dst, dst_bus, FUNC_WRITE, payload);
  pending_retransmits_.push_back({dst, dst_bus, FUNC_WRITE, payload, millis() + RETRANSMIT_DELAY_MS});
}

void InfinitESPComponent::send_reply_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus,
                                      const std::vector<uint8_t> &payload) {
  ESP_LOGV("InfinitESP", "TX#%u REPLY %02X->%02X uart_avail=%d",
           diag_tx_seq_ + 1, src, dst, available());
  transmit_frame_(dst, dst_bus, src, src_bus, FUNC_REPLY, payload);
}

void InfinitESPComponent::send_exception_(uint8_t dst, uint8_t dst_bus, uint8_t src, uint8_t src_bus,
                                           uint8_t table, uint8_t row, uint8_t code) {
  std::vector<uint8_t> payload = {0x00, table, row, code};
  transmit_frame_(dst, dst_bus, src, src_bus, FUNC_EXCEPTION, payload);
}

// --- Frame Handlers ---

void InfinitESPComponent::handle_read_request_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;
  uint8_t dest = current_frame_.dst;  // who they're talking to (SAM or ZC)
  bool is_zc = is_emu_zc_addr_(dest);

  ESP_LOGD("InfinitESP", "%s READ %04X from %02X",
           is_zc ? "ZC" : "SAM", reg_key, current_frame_.src);

  const std::vector<uint8_t> *reg_data = get_register(dest, reg_key);
  if (reg_data != nullptr) {
    // Build reply payload: [0x00, table, row] + register data
    std::vector<uint8_t> reply_payload = {0x00, table, row};
    reply_payload.insert(reply_payload.end(), reg_data->begin(), reg_data->end());

    // Debug: log model/serial fields from 0104 to verify register data before TX
    if (reg_key == REG_DEVICE_INFO && reg_data->size() >= 96) {
      ESP_LOGI("InfinitESP", "%s 0104 model: %.*s", is_zc ? "ZC" : "SAM", 20, (const char *) &(*reg_data)[64]);
      ESP_LOGI("InfinitESP", "%s 0104 serial: %.*s", is_zc ? "ZC" : "SAM", 24, (const char *) &(*reg_data)[96]);
    }

    send_reply_(current_frame_.src, current_frame_.src_bus, dest, current_frame_.dst_bus, reply_payload);
  } else {
    ESP_LOGI("InfinitESP", "%s READ unknown register %04X — returning EXCEPTION",
             is_zc ? "ZC" : "SAM", reg_key);
    send_exception_(current_frame_.src, current_frame_.src_bus, dest, current_frame_.dst_bus, table, row, 0x04);
  }
}


void InfinitESPComponent::handle_write_request_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;
  uint8_t dest = current_frame_.dst;  // who they're writing to (SAM or ZC)
  bool is_zc = is_emu_zc_addr_(dest);

  // ZC-specific write handling
  if (is_zc) {
    ESP_LOGI("InfinitESP", "ZC WRITE %04X from %02X (%d bytes)",
             reg_key, current_frame_.src, current_frame_.payload.size() - 3);

    // 0308: damper position command. SYSTEM-WIDE 8-byte payload (one byte per
    // system zone 1-8); store the full payload. Mirror to 0319 for the
    // thermostat's duct-eval read (emulated-ZC path only).
    if (reg_key == REG_ZC_DAMPER_CMD) {
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> damper(current_frame_.payload.begin() + 3,
                                     current_frame_.payload.end());
        store_register_(dest, REG_ZC_DAMPER_CMD, damper);

        // Mirror damper positions to 0319 (no delay)
        mirror_damper_to_0319_(dest, damper);

        {
          char hex[3 * 16 + 1];
          hex[0] = '\0';
          for (size_t i = 0; i < damper.size() && i < 16; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%02X ", damper[i]);
            strlcat(hex, b, sizeof(hex));
          }
          ESP_LOGD("InfinitESP", "ZC damper [%u bytes]: %s -> 0319 mirrored",
                   damper.size(), hex);
        }

        notify_entities_(dest, REG_ZC_DAMPER_CMD);
        notify_entities_(dest, REG_ZC_ZONE_CONFIG);
      }
      // 1-byte ACK (matches real ZC behavior)
      send_reply_(current_frame_.src, current_frame_.src_bus, dest, current_frame_.dst_bus, {0x00});
      return;
    }

    // 3404: heartbeat — 1-byte ACK
    if (reg_key == REG_ZC_HEARTBEAT) {
      if (current_frame_.payload.size() > 3) {
        std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
        store_register_(dest, reg_key, data);
      }
      send_reply_(current_frame_.src, current_frame_.src_bus, dest, current_frame_.dst_bus, {0x00});
      notify_entities_(dest, reg_key);
      return;
    }

    // Other ZC writes: fall through to generic handling below
  } else {
    ESP_LOGD("InfinitESP", "WRITE to register %04X from %02X (%d bytes)", reg_key, current_frame_.src,
             current_frame_.payload.size() - 3);
  }

  // Generic write handling (SAM writes and unhandled ZC writes)
  if (current_frame_.payload.size() > 3) {
    std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
    // Protect device identity from being overwritten
    if (reg_key != REG_DEVICE_INFO) {
      store_register_(dest, reg_key, data);
    }
  }

  // Send ACK reply (echo the original payload back)
  send_reply_(current_frame_.src, current_frame_.src_bus, dest, current_frame_.dst_bus, current_frame_.payload);

  // Notify sub-platforms
  notify_entities_(dest, reg_key);
}

void InfinitESPComponent::handle_reply_() {
  if (current_frame_.payload.size() < 3)
    return;

  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;

  ESP_LOGD("InfinitESP", "REPLY register %04X from %02X (%d bytes)", reg_key,
           current_frame_.src, current_frame_.payload.size() - 3);

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
      mirror_to_sam_(reg_key, data);
      // Log time fields from 3B02 for debugging
      if (reg_key == REG_SAM_STATE && data.size() >= REG3B02_MINUTES + 2) {
        uint16_t minutes = ((uint16_t) data[REG3B02_MINUTES] << 8) | (uint16_t) data[REG3B02_MINUTES + 1];
        ESP_LOGD("InfinitESP", "3B02 reply: weekday=%u minutes=%u (%02u:%02u)",
                 data[REG3B02_WEEKDAY], minutes, minutes / 60, minutes % 60);
      }

      // Heuristic bus unit detection (AUTO mode only)
      // On every 3B02 update, check active zone temps.
      // If any active zone temp byte <= 50, bus is in °C.
      // No plausible HVAC zone is >50°C (122°F), so this is reliable.
      if (reg_key == REG_SAM_STATE && temperature_unit_ == TemperatureUnit::AUTO &&
          data.size() >= REG3B02_TEMPS + 8) {
        uint8_t active = data[REG3B02_ACTIVE_ZONES];
        bool prev = bus_celsius_detected_;
        for (uint8_t i = 0; i < 8; i++) {
          if (active & (1 << i)) {
            uint8_t temp = data[REG3B02_TEMPS + i];
            // 0xFF = no sensor / offline; 0x00 = unpopulated (common during
            // thermostat reboot before the tstat has filled active-zone slots).
            // Skip both — neither is a real reading. Treating 0 as real would
            // trip the <=50 -> °C branch and corrupt stale-sensor fallbacks
            // (which convert zone-1 ambient via bus_uses_celsius()).
            if (temp != 0xFF && temp != 0x00) {
              bus_celsius_detected_ = (temp <= 50);
              bus_unit_detected_ = true;
              break;
            }
          }
        }
        if (bus_unit_detected_ && bus_celsius_detected_ != prev) {
          ESP_LOGI("InfinitESP", "Bus unit detected: %s (heuristic)",
                   bus_celsius_detected_ ? "°C" : "°F");
        }
      }

      // Authoritative metric-units flag from the thermostat's 3B06 push.
      // The tstat pushes 3B06 to our emulated SAM (dst=0x92); reply stores it
      // under src (0x20). data[1]: 0=English/°F, 1=Metric/°C (verified 2026-06-26).
      // SAM-emulated path only — when not emulating we poll 3B05 instead.
      if (sam_enabled() && reg_key == REG_SAM_DEALER &&
          current_frame_.src == ADDR_THERMOSTAT &&
          temperature_unit_ == TemperatureUnit::AUTO) {
        handle_metric_units_reply_(current_frame_.src, reg_key, data);
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
        const char *unit = bus_uses_celsius() ? "\xc2\xb0" "C" : "\xc2\xb0" "F";
        float ht_disp = comfort_byte_to_celsius(htsp);
        float cl_disp = comfort_byte_to_celsius(clsp);
        ESP_LOGI("InfinitESP", "COMFORT %s: heat=%.1f%s cool=%.1f%s fan=%d rclg=%d rhtg=%d hum_vent=0x%02X unk=[%02X %02X]",
                 names[i], ht_disp, unit, cl_disp, unit, fan, rclg, rhtg, data[base + 4], data[base + 5], data[base + 6]);
      }
    }

    if (reg_key == REG_TSTAT_VACATION && data.size() >= 7) {
      ESP_LOGI("InfinitESP", "VACATION: min_temp=%d max_temp=%d fan=%d (%s)", data[0], data[1], data[2],
               bus_uses_celsius() ? "°C" : "°F");
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

    notify_entities_(current_frame_.src, reg_key);
  }

  // If bus just came online, explicitly notify bus status sensors
  if (was_offline) {
    notify_entities_(0, 0);
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

void InfinitESPComponent::poll_discovery_() {
  // Probe one observed (device, table) for its 0xNN01 table definition, sent
  // from ADDR_FAKESAM (0x93). Picks the queryable pair missing a cached name
  // with the oldest last-query time (so a dropped reply is retried on the next
  // full sweep rather than never). Skips our own emulated addresses.
  std::pair<uint8_t, uint8_t> best{0, 0};
  bool found = false;
  uint32_t best_ts = UINT32_MAX;
  for (const auto &akv : device_registers_) {
    uint8_t addr = akv.first;
    if (addr == sam_address_ || is_emu_zc_addr_(addr) || addr == ADDR_FAKESAM)
      continue;  // our own emulated roles, or the phantom itself
    for (const auto &rkv : akv.second) {
      uint8_t table = rkv.first >> 8;
      if (table == 0)
        continue;  // reg keys are (table<<8|row); table 0 is invalid
      auto key = std::make_pair(addr, table);
      if (table_names_.count(key))
        continue;  // already learned
      auto it = discovery_query_ms_.find(key);
      uint32_t ts = (it == discovery_query_ms_.end()) ? 0 : it->second;
      if (ts < best_ts) {
        best_ts = ts;
        best = key;
        found = true;
      }
    }
  }
  if (!found)
    return;
  std::vector<uint8_t> payload = {0x00, best.second, TABLEDEF_ROW};
  transmit_frame_(best.first, 0x01, ADDR_FAKESAM, 0x01, FUNC_READ, payload);
  discovery_query_ms_[best] = millis();
  ESP_LOGD("InfinitESP", "DISCOVERY probe %02X for table %02X%02X (as %02X)",
           best.first, best.second, TABLEDEF_ROW, ADDR_FAKESAM);
}

void InfinitESPComponent::handle_discovery_reply_() {
  // Reply to a FakeSAM (0x93) probe. Two probe types are routed here:
  //  (1) 0xNN01 tabledef probes (poll_discovery_) — parse + cache the table name.
  //  (2) 3B05 metric-units poll (poll_metric_units_, sam-not-emulated only) —
  //      read data[1] (0=English/°F, 1=Metric/°C) as the authoritative unit flag.
  if (current_frame_.payload.size() < 3)
    return;
  uint8_t table = current_frame_.payload[1];
  uint8_t row = current_frame_.payload[2];
  uint16_t reg_key = (table << 8) | row;
  if (current_frame_.payload.size() <= 3)
    return;
  std::vector<uint8_t> data(current_frame_.payload.begin() + 3, current_frame_.payload.end());
  store_register_(current_frame_.src, reg_key, data);

  // Metric-units poll reply (3B05, sam not emulated)
  if (reg_key == REG_SAM_ACCESSORIES && temperature_unit_ == TemperatureUnit::AUTO) {
    handle_metric_units_reply_(current_frame_.src, reg_key, data);
    return;
  }

  // Name lives at [2..9] of the tabledef register (row 0x01).
  if (row == TABLEDEF_ROW && data.size() >= 10) {
    std::string name(data.begin() + 2, data.begin() + 10);
    while (!name.empty() && (name.back() == '\0' || name.back() == ' '))
      name.pop_back();
    table_names_[{current_frame_.src, table}] = name;
    ESP_LOGI("InfinitESP", "DISCOVERY: %02X table %02X = '%s' (alloc=%u, rows=%u)",
             current_frame_.src, table, name.c_str(),
             data.size() >= 12 ? (unsigned) ((data[10] << 8) | data[11]) : 0,
             data.size() >= 13 ? (unsigned) data[12] : 0);
  }
}

void InfinitESPComponent::handle_metric_units_reply_(uint8_t device_addr, uint16_t reg_key,
                                                       const std::vector<uint8_t> &data) {
  // Authoritative F/C flag from 3B05 (sam not emulated, polled) or 3B06
  // (sam emulated, thermostat push). data[1]: 0=English/°F, 1=Metric/°C.
  // Verified live 2026-06-26 across 3 unit transitions on an Infinity Touch.
  if (data.size() < 2)
    return;
  bool metric = (data[1] != 0x00);
  bool first_read = !metric_units_known_;
  if (first_read || metric != metric_units_) {
    metric_units_ = metric;
    metric_units_known_ = true;
    ESP_LOGI("InfinitESP", "Metric units %s: %s (from %04X data[1]=0x%02X)",
             first_read ? "detected" : "updated",
             metric ? "°C" : "°F", reg_key, data[1]);
  }
}

void InfinitESPComponent::poll_metric_units_() {
  // When NOT emulating the SAM, the thermostat doesn't push 3B06 to us, so poll
  // its 3B05 (accessories — intrinsic config, independent of SAM presence) as
  // FakeSAM (0x93). Reply routes to handle_discovery_reply_ → handle_metric_units_reply_.
  // Polled on startup and when the cached value is stale (>5 min).
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x05};
  transmit_frame_(ADDR_THERMOSTAT, 0x01, ADDR_FAKESAM, 0x01, FUNC_READ, payload);
  ESP_LOGD("InfinitESP", "Polling thermostat 3B05 for metric-units (as %02X)", ADDR_FAKESAM);
}

// --- Register Management ---

void InfinitESPComponent::store_register_(uint8_t addr, uint16_t key, const std::vector<uint8_t> &data) {
  device_registers_[addr][key] = data;
}

void InfinitESPComponent::notify_entities_(uint8_t device_addr, uint16_t register_key) {
  uint8_t src_class = device_addr >> 4;
  for (auto *entity : entities_) {
    uint8_t dc = entity->get_bus_class();
    if (dc != 0 && src_class != 0 && dc != src_class)
      continue;
    entity->on_register_update(device_addr, register_key);
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
  auto *state = get_register(sam_address_, REG_SAM_STATE);
  if (state && !state->empty())
    return state->at(0);  // active_zones bitmask
  return 0;
}

uint16_t InfinitESPComponent::get_zone_hold_duration(uint8_t zone) const {
  auto *data = get_register(sam_address_, REG_SAM_ZONES);
  uint8_t idx = zone - 1;
  if (!data || data->size() < REG3B03_HOLD_DURATIONS + idx * 2 + 2)
    return 0;
  bool is_holding = (data->at(REG3B03_ZONES_HOLDING) & (1 << idx)) != 0;
  uint16_t dur = ((uint16_t) data->at(REG3B03_HOLD_DURATIONS + idx * 2) << 8) |
                 data->at(REG3B03_HOLD_DURATIONS + idx * 2 + 1);
  // Carrier protocol: zones_holding bit + duration<=1 = permanent hold
  if (is_holding && dur <= 1)
    return HOLD_PERMANENT;
  return dur;
}

std::string InfinitESPComponent::format_hold_end(uint16_t hold_minutes) const {
  auto *state = get_register(sam_address_, REG_SAM_STATE);
  if (!state || state->size() < REG3B02_MINUTES + 2)
    return "";  // bus clock not available yet
  uint16_t now_min = ((uint16_t) state->at(REG3B02_MINUTES) << 8) |
                     state->at(REG3B02_MINUTES + 1);
  uint16_t end_min = now_min + hold_minutes;
  if (end_min >= 1440) end_min -= 1440;
  uint8_t hr24 = end_min / 60;
  uint8_t mn = end_min % 60;
  uint8_t hr12 = hr24 % 12;
  if (hr12 == 0) hr12 = 12;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d %s", hr12, mn, hr24 < 12 ? "AM" : "PM");
  return std::string(buf);
}

void InfinitESPComponent::mirror_to_sam_(uint16_t reg_key, const std::vector<uint8_t> &data) {
  store_register_(sam_address_, reg_key, data);
  if (reg_key == REG_SAM_STATE || reg_key == REG_SAM_ZONES)
    sam_state_received_ = true;
}

void InfinitESPComponent::mirror_damper_to_0319_(uint8_t addr, const std::vector<uint8_t> &damper) {
  // 0308 carries 4 damper bytes; 0319 mirrors them into an 8-byte state field
  // (bytes 0-3 = positions, bytes 4-7 = 0xFF). Shared by the emulated-ZC write
  // path (handle_write_request_) and the passive physical-ZC capture.
  std::vector<uint8_t> state_0319(8);
  for (uint8_t i = 0; i < 4 && i < damper.size(); i++)
    state_0319[i] = damper[i];
  for (uint8_t i = 4; i < 8; i++)
    state_0319[i] = 0xFF;
  store_register_(addr, REG_ZC_ZONE_CONFIG, state_0319);
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
  if (!sam_enabled()) return;
  auto *zones_data = get_register(sam_address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
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
  mirror_to_sam_(REG_SAM_ZONES, data);

  // Build write payload: [0x00, 0x3B, 0x03] + [zone_idx, 0x00, flags] + data[3:]
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, idx, 0x00, flags};
  payload.insert(payload.end(), write_data.begin() + 3, write_data.end());

  send_write_frame_(ADDR_THERMOSTAT, 0x01, payload);
  ESP_LOGI("InfinitESP", "Set zone %d: heat=%d cool=%d flags=0x%02X az=0x00", zone, heat_sp, cool_sp, flags);
  // Log first 10 bytes of payload for debugging
  ESP_LOGI("InfinitESP", "  payload: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
           payload.size() > 0 ? payload[0] : 0, payload.size() > 1 ? payload[1] : 0,
           payload.size() > 2 ? payload[2] : 0, payload.size() > 3 ? payload[3] : 0,
           payload.size() > 4 ? payload[4] : 0, payload.size() > 5 ? payload[5] : 0,
           payload.size() > 6 ? payload[6] : 0, payload.size() > 7 ? payload[7] : 0,
           payload.size() > 8 ? payload[8] : 0, payload.size() > 9 ? payload[9] : 0);
}

void InfinitESPComponent::set_zone_fan(uint8_t zone, uint8_t fan_mode) {
  if (!sam_enabled()) return;
  auto *zones_data = get_register(sam_address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
    return;

  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  data[REG3B03_FAN_MODES + idx] = fan_mode;

  // Update local cache
  data[REG3B03_CHANGE_FLAGS] = 0;  // clear change flags
  mirror_to_sam_(REG_SAM_ZONES, data);

  // Build write payload — target specific zone
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, idx, 0x00, CHANGE_FAN};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_write_frame_(ADDR_THERMOSTAT, 0x01, payload);
  ESP_LOGI("InfinitESP", "Set zone %d fan=%d", zone, fan_mode);
}

void InfinitESPComponent::set_zone_hold(uint8_t zone, uint16_t duration_minutes) {
  if (!sam_enabled()) return;
  auto *zones_data = get_register(sam_address_, REG_SAM_ZONES);
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
  mirror_to_sam_(REG_SAM_ZONES, data);

  // Use CHANGE_HOLD flag (0x02) + CHANGE_OVERRIDE flag (0x80)
  uint8_t flags = CHANGE_HOLD | CHANGE_OVERRIDE;
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, idx, 0x00, flags};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_write_frame_(ADDR_THERMOSTAT, 0x01, payload);
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
  uint8_t htsp_raw = (*comfort)[base + 0];
  uint8_t clsp_raw = (*comfort)[base + 1];
  uint8_t fan = (*comfort)[base + 2];

  // Convert comfort bytes to the 3B03 setpoint encoding via the shared helpers
  // (comfort_byte_to_celsius → celsius_to_setpoint) — single source of truth for
  // the comfort→setpoint transform. Byte-identical to the previous inline branch.
  // NOTE: comfort_byte_to_celsius() is itself tracked as buggy in °C mode (ROADMAP
  // bug #1: 400A is always °F, not C*2); routing through it here means a future
  // fix to that helper propagates to apply_activity too.
  uint8_t htsp_bus = celsius_to_setpoint(comfort_byte_to_celsius(htsp_raw));
  uint8_t clsp_bus = celsius_to_setpoint(comfort_byte_to_celsius(clsp_raw));

  const char *names[] = {"home", "away", "sleep", "wake", "manual"};
  ESP_LOGI("InfinitESP", "Apply activity %s to zone %d: heat=%d->%d cool=%d->%d fan=%d hold=%d min (%s)",
           activity_index < 5 ? names[activity_index] : "?", zone,
           htsp_raw, htsp_bus, clsp_raw, clsp_bus, fan, hold_duration,
           bus_uses_celsius() ? "°C" : "°F");

  // Update the 3B03 zones data with the activity's setpoints and fan
  auto *zones_data = get_register(sam_address_, REG_SAM_ZONES);
  if (!zones_data || zones_data->size() < REG3B03_SIZE)
    return;

  std::vector<uint8_t> data = *zones_data;
  uint8_t idx = zone - 1;

  data[REG3B03_FAN_MODES + idx] = fan;
  data[REG3B03_HEAT_SETPOINTS + idx] = htsp_bus;
  data[REG3B03_COOL_SETPOINTS + idx] = clsp_bus;

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
  mirror_to_sam_(REG_SAM_ZONES, data);

  // Write with all change flags set (fan + hold + heat + cool + override)
  uint8_t flags = CHANGE_FAN | CHANGE_HOLD | CHANGE_HEAT | CHANGE_COOL | CHANGE_OVERRIDE;
  std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, idx, 0x00, flags};
  payload.insert(payload.end(), data.begin() + 3, data.end());

  send_write_frame_(ADDR_THERMOSTAT, 0x01, payload);
  ESP_LOGI("InfinitESP", "Applied activity %s zone %d flags=0x%02X",
           activity_index < 5 ? names[activity_index] : "?", zone, flags);
}

void InfinitESPComponent::set_system_mode(uint8_t mode) {
  if (!sam_enabled()) return;
  auto *state_data = get_register(sam_address_, REG_SAM_STATE);
  if (!state_data || state_data->size() < REG3B02_SIZE) {
    ESP_LOGW("InfinitESP", "Set system mode=%d FAILED: no cached 3B02 data", mode);
    return;
  }

  std::vector<uint8_t> data = *state_data;
  uint8_t old_stagmode = data[22];

  // stagmode at offset 22: high nibble=stage, low nibble=mode
  data[22] = (data[22] & 0xF0) | (mode & 0x0F);

  // Update local 3B02 cache so we can serve it when thermostat READs us
  mirror_to_sam_(REG_SAM_STATE, data);

  ESP_LOGI("InfinitESP", "Set system mode=%d (stagmode 0x%02X->0x%02X)", mode, old_stagmode, data[22]);

  // Two-pronged approach for reliable mode changes:
  // 1. 3B03 notification with system_mode flag (notify-pull pattern)
  // 2. Direct 3B02 write with change flags (push pattern)
  // Both are needed — the 3B03 notification primes the thermostat to accept
  // the mode change, and the 3B02 write delivers the actual data.

  // 3B03 notification with system_mode flag
  auto *zones_data = get_register(sam_address_, REG_SAM_ZONES);
  if (zones_data && zones_data->size() >= 11) {
    std::vector<uint8_t> payload = {0x00, 0x3B, 0x03, 0x00, 0x00, CHANGE_MODE};
    payload.insert(payload.end(), zones_data->begin() + 3, zones_data->end());
    send_write_frame_(ADDR_THERMOSTAT, 0x01, payload);
  }

  // Direct 3B02 write with updated stagmode
  {
    std::vector<uint8_t> payload_3b02 = {0x00, 0x3B, 0x02, 0x00, 0x00, CHANGE_MODE};
    payload_3b02.insert(payload_3b02.end(), data.begin() + 3, data.end());
    send_write_frame_(ADDR_THERMOSTAT, 0x01, payload_3b02);
  }
}

// --- Default Register Initialization ---

// Pad/zero-fill a fixed-width ASCII field into a register buffer. Carrier's
// register layout uses NUL (0x00) padding (not spaces) after the string.
// File-local; shared by the SAM and ZC device-info seeds in initialize_defaults_().
static void pad_str(std::vector<uint8_t> &buf, const char *str, size_t width) {
  size_t slen = strlen(str);
  for (size_t i = 0; i < width; i++)
    buf.push_back(i < slen ? (uint8_t) str[i] : 0x00);
}

void InfinitESPComponent::initialize_defaults_() {
  // --- SAM registers ---
  if (sam_enabled()) {
    // Register 0104 - Device info (120 bytes)
    {
      std::vector<uint8_t> data;
      data.reserve(120);
      pad_str(data, "SYSTEM ACCESS MODULE", 24);    // device
      pad_str(data, "", 24);                        // location
      pad_str(data, __DATE__, 16);                  // software (auto build date)
      pad_str(data, "InfinitESP--SAM", 20);         // model
      pad_str(data, "", 12);                        // reference
      pad_str(data, "1726ESP32SAM01", 24);          // serial (week 17, 2026 = InfinitESP first working climate)
      store_register_(sam_address_, REG_DEVICE_INFO, data);
    }

    // Register 030D - SAM status (7 bytes)
    {
      std::vector<uint8_t> data = {0x3D, 0x3E, 0x3F, 0, 0, 0, 0};
      store_register_(sam_address_, REG_SAM_STATUS, data);
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
      store_register_(sam_address_, REG_SAM_STATE, data);
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
      store_register_(sam_address_, REG_SAM_ZONES, data);
    }

    // Register 3B04 - Vacation settings (11 bytes)
    // Seed values follow Infinitude's current CarBus::SAM 3B04 guess (our own RE,
    // not a Carrier source): byte 1 metric_units, min/max temp at 5/6, humidity
    // at 7/8, fan at 9. Only byte 1 is live-confirmed; the field-to-offset
    // mapping for the rest is unverified. The prior seed wrote min/max temp at
    // 3/4 (an older guess) — this just brings the seed in line with the current
    // guess so VACMINT/VACMAXT reads aren't obviously wrong.
    {
      std::vector<uint8_t> data(REG3B04_SIZE, 0);
      data[REG3B04_ACTIVE] = 0;         // vacation off
      data[REG3B04_METRIC_UNITS] = 0;   // English (matches 3B06 default)
      data[REG3B04_MIN_TEMP] = 60;      // °F
      data[REG3B04_MAX_TEMP] = 85;      // °F
      data[REG3B04_MIN_HUMIDITY] = 0;   // NONE
      data[REG3B04_MAX_HUMIDITY] = 100; // NONE
      data[REG3B04_FAN_MODE] = 0;       // auto
      store_register_(sam_address_, REG_SAM_VACATION, data);
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
      store_register_(sam_address_, REG_SAM_ACCESSORIES, data);
    }

    // Register 3B06 - Dealer info (52 bytes)
    // Seed values follow Infinitude's current CarBus::SAM 3B06 guess (our own RE,
    // not a Carrier source). Only the byte-1 metric_units flag is live-confirmed;
    // the config fields (deadband/cph/periods/programs) and dealer_name/phone
    // offsets are guesses. byte 7 was previously guessed 'temp_units' but is
    // observed 0xFF on Touch (the F/C ASCII codes live only on the RS-232 port,
    // not in this register). bus_uses_celsius() reads the runtime metric_units_
    // member (set from the thermostat's 3B06 push), not this seed byte, so the
    // seed here only affects the cached default register.
    {
      std::vector<uint8_t> data(REG3B06_SIZE, 0);
      data[REG3B06_BACKLIGHT] = 8;           // level 8
      data[REG3B06_METRIC_UNITS] = 0;        // English (°F)
      data[REG3B06_DEADBAND] = 3;
      data[REG3B06_CYCLES_PER_HOUR] = 4;
      data[REG3B06_SCHEDULE_PERIODS] = 4;
      data[REG3B06_PROGRAMS_ENABLED] = 1;
      data[7] = 0xFF;                        // unknown2 (Touch observes 0xFF)
      data[8] = 0xFF;                        // unknown3
      data[9] = 1;                           // programs_enabled_2
      data[10] = 0;                          // metric_units mirror (English)
      strncpy(reinterpret_cast<char *>(&data[REG3B06_DEALER_NAME]), "InfinitESP", 20);
      // dealer_phone at offset 32: all zeros
      store_register_(sam_address_, REG_SAM_DEALER, data);
    }

    // Register 3B0E - Activity flag (1 byte)
    {
      std::vector<uint8_t> data = {0};
      store_register_(sam_address_, REG_SAM_ACTIVITY, data);
    }

    ESP_LOGI("InfinitESP", "Initialized %d SAM registers at address 0x%02X",
    device_registers_[sam_address_].size(), sam_address_);
  } else {
    ESP_LOGI("InfinitESP", "SAM emulation disabled (sam_address=0)");
  }

  // --- Zone Controller registers ---
  // Two controllers are emulated when zc_enabled(): the primary at zc_address_
  // (0x60, system zones 1-4) and a secondary at zc_address_+1 (0x61, zones 5-8),
  // matching a real two-controller Carrier damper system (issue #9). Each is a
  // full SYSTXCC4ZC01 with a distinct serial. Based on SYSTXCC4ZC01 captures.
  //
  // The secondary (0x61) is only emulated when a zone >4 has a temperature
  // sensor wired (zc_secondary_enabled_()). An empty 0x61 would still answer
  // the thermostat's 3405 presence probe and get commissioned, forcing an
  // 8-zone install even when the system only has zones 1-4 active.
  if (zc_enabled()) {
    auto seed_zc = [this](uint8_t zc_addr, const char *serial) {
      // Register 0104 - Device info (120 bytes)
      {
        std::vector<uint8_t> data;
        data.reserve(120);
        pad_str(data, "INFINITESP ZONE CTRL", 24);  // device
        pad_str(data, "", 24);                       // location
        pad_str(data, __DATE__, 16);                 // software
        pad_str(data, "SYSTXCC4ZC01", 20);           // model (real model so thermostat recognizes it)
        pad_str(data, "INFD-ZC-01", 12);              // reference
        pad_str(data, serial, 24);                    // serial (week 17, 2026; unique per controller)
        store_register_(zc_addr, REG_DEVICE_INFO, data);
      }

      // Register 0302 - Zone sensor readings (24 bytes, TLV format)
      // Six entries of [tag, id, val_hi, val_lo] in id order:
      //   z1(0x01) z2(0x02) z3(0x03) z4(0x04) LAT(0x14) HPT(0x1C).
      // tag 0x01 = present, 0x04 = not installed. °F = uint16_BE / 16.
      // Each controller reports its OWN four local zones (system zones N..N+3).
      // Local zone 1 is thermostat-direct (not installed); 2-4 report 73°F until
      // an external temperature_sensor overrides them. LAT/HPT report
      // not-installed (InfinitESP has no thermistors on those ports).
      {
        // 73°F → 73×16 = 1168 = 0x0490
        std::vector<uint8_t> data = {
          0x04, 0x01, 0x00, 0x00,               // local zone 1: not installed (thermostat-direct)
          0x01, 0x02, 0x04, 0x90,               // local zone 2: 73°F
          0x01, 0x03, 0x04, 0x90,               // local zone 3: 73°F
          0x01, 0x04, 0x04, 0x90,               // local zone 4: 73°F
          0x04, 0x14, 0x00, 0x00,               // LAT (leaving air temp): not installed
          0x04, 0x1C, 0x00, 0x00,               // HPT: not installed
        };
        store_register_(zc_addr, REG_ZC_ZONE_STATUS, data);
      }

      // Register 0319 - Damper state feedback (8 bytes)
      // Start with all zones detected; mirrors 0308 writes at runtime.
      {
        std::vector<uint8_t> data = {0x0F, 0x0F, 0x0F, 0x0F, 0xFF, 0xFF, 0xFF, 0xFF};
        store_register_(zc_addr, REG_ZC_ZONE_CONFIG, data);
      }

      // Register 030D - Unknown (7 bytes, always zeros)
      // Shared register number with SAM 030D but different device address
      {
        std::vector<uint8_t> data(7, 0);
        store_register_(zc_addr, REG_SAM_STATUS, data);
      }

      // Register 3404 - Heartbeat flag (1 byte)
      {
        std::vector<uint8_t> data = {0x00};
        store_register_(zc_addr, REG_ZC_HEARTBEAT, data);
      }

      // Register 3405 - Presence probe (discovery register, 3 bytes)
      {
        std::vector<uint8_t> data = {0x00, 0x00, 0x00};
        store_register_(zc_addr, REG_ZC_PRESENCE, data);
      }

      // Register 0310 - Cycle counters (12 bytes, 3 key-value entries)
      // Values from real SYSTXCC4ZC01 capture
      {
        std::vector<uint8_t> data = {
          0x38, 0x00, 0x00, 0x01,
          0x39, 0x00, 0x00, 0x01,
          0x2B, 0x00, 0x00, 0x7E,
        };
        store_register_(zc_addr, REG_ZC_CYCLES, data);
      }

      // Register 0311 - Runtime hours (12 bytes, 3 key-value entries)
      // Values from real SYSTXCC4ZC01 capture
      {
        std::vector<uint8_t> data = {
          0x3A, 0x00, 0x00, 0x00,
          0x3B, 0x00, 0x00, 0x00,
          0x2C, 0x00, 0x7E, 0xED,
        };
        store_register_(zc_addr, REG_ZC_RUNTIME, data);
      }

      ESP_LOGI("InfinitESP", "Initialized %d ZC registers at address 0x%02X",
               device_registers_[zc_addr].size(), zc_addr);
    };
    seed_zc(zc_address_, "1726ESP32ZC01");       // 0x60 — system zones 1-4
    // Secondary 0x61 only when a zone >4 is configured; otherwise it would be
    // discovered and commission zones 5-8 that have no sensors.
    if (zc_secondary_enabled_())
      seed_zc((uint8_t) (zc_address_ + 1), "1726ESP32ZC02");  // 0x61 — system zones 5-8
    else
      ESP_LOGI("InfinitESP", "No zones >4 configured — secondary ZC at 0x%02X not emulated",
               (uint8_t) (zc_address_ + 1));
  }
}

bool InfinitESPComponent::bus_uses_celsius() const {
  switch (temperature_unit_) {
    case TemperatureUnit::CELSIUS:   return true;
    case TemperatureUnit::FAHRENHEIT: return false;
    case TemperatureUnit::AUTO:
      // Prefer the authoritative metric-units flag from 3B06 (sam emulated) or
      // 3B05 (sam not emulated) — data[1]: 0=English/°F, 1=Metric/°C. Falls back
      // to the zone-temp heuristic only before the first authoritative read.
      if (metric_units_known_)
        return metric_units_;
      return bus_celsius_detected_;
  }
  return false;  // unreachable
}

float InfinitESPComponent::bus_temp_to_celsius(float bus_value) const {
  if (bus_uses_celsius())
    return bus_value;  // already °C
  return (bus_value - 32.0f) * (5.0f / 9.0f);  // °F → °C
}

float InfinitESPComponent::celsius_to_bus_temp(float celsius) const {
  if (bus_uses_celsius())
    return celsius;  // bus wants °C
  return celsius * (9.0f / 5.0f) + 32.0f;  // °C → °F
}

float InfinitESPComponent::comfort_byte_to_celsius(uint8_t raw) const {
  if (bus_uses_celsius())
    return (float) raw / 2.0f;  // half-degree °C
  // °F mode: raw is whole °F, convert to °C
  return ((float) raw - 32.0f) * (5.0f / 9.0f);
}

uint8_t InfinitESPComponent::celsius_to_comfort_byte(float celsius) const {
  if (bus_uses_celsius())
    return (uint8_t) roundf(celsius * 2.0f);  // °C → half-degree
  return (uint8_t) roundf(celsius * (9.0f / 5.0f) + 32.0f);  // °C → °F
}

float InfinitESPComponent::setpoint_to_celsius(uint8_t raw) const {
  if (bus_uses_celsius())
    return (float) raw;  // whole °C
  return ((float) raw - 32.0f) * (5.0f / 9.0f);  // °F → °C
}

uint8_t InfinitESPComponent::celsius_to_setpoint(float celsius) const {
  if (bus_uses_celsius())
    return (uint8_t) roundf(celsius);  // whole °C
  return (uint8_t) roundf(celsius * (9.0f / 5.0f) + 32.0f);  // °C → °F
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

// --- Bus Traffic Capture ---

void InfinitESPComponent::log_traffic_(uint8_t src, uint8_t dst, uint8_t func, uint16_t reg_key,
                                         const std::vector<uint8_t> &payload) {
  TrafficKey key{src, dst, func, reg_key};
  auto &entry = traffic_log_[key];
  entry.count++;
  entry.last_payload = payload;
  entry.last_seen_ms = millis();

  // Capture write frames for protocol analysis
  if (func == FUNC_WRITE) {
    write_captures_.push_back({src, dst, reg_key, payload});
    if (write_captures_.size() > WRITE_CAPTURE_MAX)
      write_captures_.pop_front();
  }
}

void InfinitESPComponent::stream_bus_report_(void (*write_fn)(const uint8_t *, size_t, void *), void *ctx) {
  // Stream a JSON diagnostic report via a write callback (UART/TCP socket).
  // Uses only a 256-byte stack buffer — no heap allocation regardless of report size.
  // Full register hex dumps, no truncation.
  auto emit = [&](const char *s) { write_fn((const uint8_t *)s, strlen(s), ctx); };
  char buf[256];
  int n;

  // Report metadata
  n = snprintf(buf, sizeof(buf),
    "{\"fw\":\"InfinitESP 0.1\",\"up\":%u,\"bus\":\"%s\","
    "\"sam\":\"%02X\",\"zc\":\"%02X\","
    "\"temp_unit\":\"%s\",\"temp_cfg\":\"%s\","
    "\"rx\":%u,\"tx\":%u,\"crc\":%u,\"exp\":%u,\"got\":%u,\"to\":%u,"
    "\"hwm\":%u,\"ovf\":%u,\"prg\":%u,\"pp\":%u",
    (unsigned)(millis()/1000), bus_online_?"on":"off",
    sam_address_, zc_address_,
    bus_uses_celsius() ? "C" : "F",
    temperature_unit_ == TemperatureUnit::AUTO ? "auto" : temperature_unit_ == TemperatureUnit::CELSIUS ? "C" : "F",
    (unsigned)diag_frames_parsed_, (unsigned)diag_tx_seq_,
    (unsigned)diag_crc_fail_, (unsigned)diag_reply_expected_,
    (unsigned)diag_reply_received_, (unsigned)diag_reply_timeout_,
    (unsigned)diag_uart_hwm_, (unsigned)diag_uart_overflow_events_,
    (unsigned)diag_poll_purged_, (unsigned)pending_polls_.size());
  write_fn((const uint8_t *)buf, n, ctx);

  // Devices
  emit(",\"dev\":[");
  bool first = true;
  for (auto &akv : device_registers_) {
    auto it = akv.second.find(REG_DEVICE_INFO);
    if (it == akv.second.end()) continue;
    auto &d = it->second;
    char name[25] = {}, model[21] = {}, serial[25] = {};
    memcpy(name, d.data(), std::min((size_t)24, d.size()));
    if (d.size() > 64) memcpy(model, d.data()+64, std::min((size_t)20, d.size()-64));
    if (d.size() > 96) memcpy(serial, d.data()+96, std::min((size_t)24, d.size()-96));
    for (int i=23; i>=0 && name[i]==' '; i--) name[i]=0;
    for (int i=19; i>=0 && model[i]==' '; i--) model[i]=0;
    for (int i=23; i>=0 && serial[i]==' '; i--) serial[i]=0;
    n = snprintf(buf, sizeof(buf), "%s{\"address\":\"%02X\",\"name\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\"}",
             first?"":",", akv.first, name, model, serial);
    write_fn((const uint8_t *)buf, n, ctx);
    first = false;
  }
  emit("]");

  // Traffic summary
  emit(",\"traffic\":[");
  first = true;
  for (auto &kv : traffic_log_) {
    auto &key = kv.first;
    auto &entry = kv.second;
    const char *fn = key.func==FUNC_READ?"R":key.func==FUNC_REPLY?"P":key.func==FUNC_WRITE?"W":"E";
    n = snprintf(buf, sizeof(buf), "%s\"%02X>%02X %s %04X x%u\"",
             first?"":",", key.src, key.dst, fn, key.reg_key, (unsigned)entry.count);
    write_fn((const uint8_t *)buf, n, ctx);
    first = false;
  }
  emit("]");

  // Learned table names (from 0xNN01 probes as ADDR_FAKESAM)
  emit(",\"tables\":[");
  first = true;
  for (const auto &kv : table_names_) {
    n = snprintf(buf, sizeof(buf), "%s{\"address\":\"%02X\",\"table\":\"%02X\",\"name\":\"%s\"}",
             first ? "" : ",", kv.first.first, kv.first.second, kv.second.c_str());
    write_fn((const uint8_t *) buf, n, ctx);
    first = false;
  }
  emit("]");
  emit(",\"writes\":[");
  first = true;
  for (auto &wc : write_captures_) {
    n = snprintf(buf, sizeof(buf), "%s{\"src\":\"%02X\",\"dst\":\"%02X\",\"reg\":\"%04X\",\"hex\":\"",
             first?"":",", wc.src, wc.dst, wc.reg_key);
    write_fn((const uint8_t *)buf, n, ctx);
    for (auto b : wc.payload) {
      n = snprintf(buf, sizeof(buf), "%02X", b);
      write_fn((const uint8_t *)buf, n, ctx);
    }
    emit("\"}");
    first = false;
  }
  emit("]");

  // Register dump — full hex, no truncation
  emit(",\"regs\":[");
  first = true;
  for (auto &akv : device_registers_) {
    for (auto &rkv : akv.second) {
      if (rkv.first == REG_TSTAT_WIFI) continue;  // skip WiFi creds
      n = snprintf(buf, sizeof(buf),
               "%s{\"address\":\"%02X\",\"register\":\"%04X\",\"length\":%u,\"data\":\"",
               first?"":",", akv.first, rkv.first, (unsigned)rkv.second.size());
      write_fn((const uint8_t *)buf, n, ctx);
      // Full hex dump
      for (size_t i = 0; i < rkv.second.size(); i++) {
        char hex[3];
        hex[0] = "0123456789ABCDEF"[rkv.second[i] >> 4];
        hex[1] = "0123456789ABCDEF"[rkv.second[i] & 0x0F];
        write_fn((const uint8_t *)hex, 2, ctx);
      }
      emit("\"}");
      first = false;
    }
  }
  emit("]}\r\n");
}

// --- ZC Zone Temperature Management ---

void InfinitESPComponent::set_zc_temperature_sensor(uint8_t zone, sensor::Sensor *s) {
  if (zone < 2 || zone > 8) return;
  zc_zones_[zone].temp_sensor = s;
  s->add_on_state_callback([this, zone](float value) {
    this->on_zc_sensor_update_(zone, value);
  });
}

void InfinitESPComponent::set_zc_staleness_timeout(uint8_t zone, uint32_t timeout_ms) {
  if (zone < 2 || zone > 8) return;
  zc_zones_[zone].staleness_timeout_ms = timeout_ms;
}

bool InfinitESPComponent::zc_unit_is_fahrenheit_(const ZCZoneConfig &slot) const {
  switch (slot.sensor_unit) {
    case 1: return false;  // explicit °C
    case 2: return true;   // explicit °F
    default: return !bus_uses_celsius();  // inherit from bus
  }
}

void InfinitESPComponent::register_zc_thermistor_(ZCZoneConfig &slot, uint8_t tlv_id, sensor::Sensor *s) {
  slot.temp_sensor = s;
  s->add_on_state_callback([this, &slot, tlv_id](float value) {
    this->on_zc_thermistor_update_(slot, tlv_id, value);
  });
}

void InfinitESPComponent::set_zc_lat_sensor(sensor::Sensor *s) {
  register_zc_thermistor_(zc_lat_, ZC_ID_LAT, s);
}

void InfinitESPComponent::set_zc_hpt_sensor(sensor::Sensor *s) {
  register_zc_thermistor_(zc_hpt_, ZC_ID_HPT, s);
}

void InfinitESPComponent::on_zc_thermistor_update_(ZCZoneConfig &slot, uint8_t tlv_id, float value) {
  if (std::isnan(value))
    return;

  // Convert to °F: respect sensor_unit setting, default inherits from bus
  bool is_f = zc_unit_is_fahrenheit_(slot);
  float temp_f = is_f ? value : (value * 9.0f / 5.0f + 32.0f);

  // Wide sanity band (-40..250 °F) covers any real HVAC thermistor (supply air
  // can exceed the 40-99°F indoor band used for zones). Out of band almost
  // always means a sensor_unit misconfiguration (e.g. a °F sensor treated as °C
  // lands at 300°F+). Per the thermistor contract, an untrustworthy reading is
  // treated as unavailable: revert the entry to not-installed immediately.
  if (temp_f < ZC_THERMISTOR_MIN_F || temp_f > ZC_THERMISTOR_MAX_F) {
    ESP_LOGE("InfinitESP", "ZC %02X out of range: %.1f°F (from %.2f%s). Reverting to "
             "not-installed. Check sensor_unit config.",
             tlv_id, temp_f, value, is_f ? "F" : "C");
    slot.last_sensor_value = NAN;
    write_zc_temp_entry_(zc_address_, tlv_id, 0.0f, false);
    return;
  }

  slot.last_sensor_value = value;
  slot.last_sensor_update_ms = millis();
  ESP_LOGD("InfinitESP", "ZC %02X sensor: %.2f°%s → %.2f°F",
           tlv_id, value, is_f ? "F" : "C", temp_f);
  write_zc_temp_entry_(zc_address_, tlv_id, temp_f, true);
}

void InfinitESPComponent::write_zc_temp_entry_(uint8_t zc_addr, uint8_t tlv_id, float temp_f, bool present) {
  if (!zc_enabled())
    return;

  auto *data = get_register(zc_addr, REG_ZC_ZONE_STATUS);
  if (!data || data->size() != 24)
    return;

  // uint16 BE: °F × 16 when present; 0x0000 when not-installed
  uint16_t raw = present ? (uint16_t)(temp_f * ZC_TEMP_SCALE + 0.5f) : 0x0000;
  uint8_t tag = present ? ZC_0302_TAG_PRESENT : 0x04;
  uint8_t hi = (raw >> 8) & 0xFF;
  uint8_t lo = raw & 0xFF;

  // Scan the six TLV entries [tag, id, hi, lo] for our id, write in place.
  for (uint8_t e = 0; e + 3 < 24; e += 4) {
    if ((*data)[e + 1] != tlv_id)
      continue;
    if ((*data)[e] == tag && (*data)[e + 2] == hi && (*data)[e + 3] == lo)
      return;  // no change
    std::vector<uint8_t> new_data = *data;
    new_data[e] = tag;
    new_data[e + 2] = hi;
    new_data[e + 3] = lo;
    store_register_(zc_addr, REG_ZC_ZONE_STATUS, new_data);
    notify_entities_(zc_addr, REG_ZC_ZONE_STATUS);
    return;
  }
}

void InfinitESPComponent::on_zc_sensor_update_(uint8_t zone, float value) {
  if (std::isnan(value) || zone < 2 || zone > 8) return;
  auto &zc = zc_zones_[zone];

  // Convert to °F: respect sensor_unit setting, default inherits from bus
  bool is_f = zc_sensor_is_fahrenheit_(zone);
  float temp_f = is_f ? value : (value * 9.0f / 5.0f + 32.0f);

  // Range check (40-99°F indoor band). A value outside this band almost always
  // means a sensor_unit misconfiguration (e.g. a °F sensor treated as °C lands
  // at 104°F+; a °C sensor treated as °F lands below 40°F). Reject the value,
  // mark the sensor unavailable, and let the staleness path fall back to
  // zone-1 ambient until a plausible reading arrives. Self-heals on the next
  // in-range reading.
  if (temp_f < 40.0f || temp_f > 99.0f) {
    ESP_LOGE("InfinitESP", "ZC zone %u out of range: %.1f°F (from %.2f%s). Rejecting and falling "
             "back to zone-1 ambient. Check sensor_unit config.",
             zone, temp_f, value, is_f ? "F" : "C");
    zc.last_sensor_value = NAN;
    return;
  }

  zc.last_sensor_value = value;
  zc.last_sensor_update_ms = millis();
  ESP_LOGD("InfinitESP", "ZC zone %u sensor: %.2f°%s → %.2f°F → 0x%04X",
           zone, value, is_f ? "F" : "C", temp_f,
           (uint16_t)(temp_f * ZC_TEMP_SCALE + 0.5f));
  update_zc_zone_temp_(zone, temp_f);
}

// Zone-number wrapper around write_zc_temp_entry_(): writes the TLV entry for
// system zone N (present or not-installed) to the controller that serves it.
// Used by the sensor-fallback loop, which drives installed/not-installed per
// zone. update_zc_zone_temp_() is the present-only convenience for callbacks.
void InfinitESPComponent::write_zc_zone_temp_entry_(uint8_t zone, float temp_f, bool present) {
  if (zone < 2 || zone > 8) return;
  write_zc_temp_entry_(zc_addr_for_zone_(zone), zc_local_id_for_zone_(zone), temp_f, present);
}

void InfinitESPComponent::update_zc_zone_temp_(uint8_t zone, float temp_f) {
  if (zone < 2 || zone > 8) return;
  // System zone N → its controller's local id (1-4). zone N ≠ TLV id N for
  // zones 5-8, which live on the secondary controller (0x61) as local 1-4.
  write_zc_temp_entry_(zc_addr_for_zone_(zone), zc_local_id_for_zone_(zone), temp_f, true);
}

void InfinitESPComponent::check_zc_sensor_fallback_() {
  if (!zc_enabled()) return;

  // Need zone 1 temp for fallback — only available when SAM registers are populated
  auto *state = get_register(sam_address_, REG_SAM_STATE);
  if (!state || state->size() <= REG3B02_TEMPS) return;

  // Zone 1 temp in bus units (°F or °C depending on setting)
  float z1_bus = (float) (*state)[REG3B02_TEMPS];
  float z1_f = bus_uses_celsius() ? (z1_bus * 9.0f / 5.0f + 32.0f) : z1_bus;

  uint32_t now = millis();

  for (uint8_t zone = 2; zone <= 8; zone++) {
    auto &zc = zc_zones_[zone];

    bool has_fresh_sensor = (zc.temp_sensor != nullptr) &&
                            !std::isnan(zc.last_sensor_value) &&
                            ((now - zc.last_sensor_update_ms) < zc.staleness_timeout_ms);

    if (zc.temp_sensor == nullptr) {
      // No external sensor wired: report not-installed (tag 0x04), matching a
      // real ZC whose thermistor port has no sensor. This lets the thermostat
      // see phantom zones (e.g. unused slots on the secondary 0x61 board) as
      // absent rather than as a zone with a stuck temperature.
      write_zc_zone_temp_entry_(zone, 0.0f, false);
      continue;
    }

    float temp_f;
    if (has_fresh_sensor) {
      // Use external sensor: convert to °F
      bool is_f = zc_sensor_is_fahrenheit_(zone);
      temp_f = is_f ? zc.last_sensor_value : (zc.last_sensor_value * 9.0f / 5.0f + 32.0f);
    } else {
      // Fallback: use zone 1 current temperature
      temp_f = z1_f;
    }

    update_zc_zone_temp_(zone, temp_f);
  }

  // Thermistor ports (LAT 0x14, HPT 0x1C): only managed when a sensor is
  // configured. When the reading is fresh, the sensor callback already wrote a
  // present entry — nothing to do here. When stale (or never arrived), revert
  // the entry to not-installed so the thermostat stops seeing it. Unlike zones,
  // there is no zone-1-ambient fallback for supply-air temperature.
  struct Therm { ZCZoneConfig *slot; uint8_t id; };
  for (auto t : {Therm{&zc_lat_, ZC_ID_LAT}, Therm{&zc_hpt_, ZC_ID_HPT}}) {
    if (t.slot->temp_sensor == nullptr)
      continue;  // unconfigured: leave the seed (not-installed) untouched
    bool fresh = !std::isnan(t.slot->last_sensor_value) &&
                 ((now - t.slot->last_sensor_update_ms) < t.slot->staleness_timeout_ms);
    if (!fresh)
      write_zc_temp_entry_(zc_address_, t.id, 0.0f, false);
  }
}

}  // namespace infinitesp
}  // namespace esphome
