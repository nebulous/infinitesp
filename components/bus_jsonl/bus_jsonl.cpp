#include "bus_jsonl.h"

namespace esphome {
namespace bus_jsonl {

void BusJsonlComponent::setup() {
  ESP_LOGI(TAG, "JSONL bus stream ready (queue %u lines / %u bytes, %s timestamps)",
           (unsigned) MAX_QUEUED_LINES, (unsigned) MAX_QUEUED_BYTES,
           epoch_provider_ ? "ISO8601" : "boot-ms");
}

// Re-anchor the epoch/millis correlation whenever the RTC's whole second
// advances. Between rolls, millis() carries the precision (the RTC anchors
// phase only), and an RTC step (NTP correction) is absorbed at the next
// frame. Sanity floor of 2020 keeps unsynced/1970 clocks out.
void BusJsonlComponent::rebase_epoch_() {
  if (epoch_provider_ == nullptr)
    return;
  time_t s = epoch_provider_();
  if (s > (time_t) 1577836800 && s != last_sec_) {
    last_sec_ = s;
    base_epoch_ms_ = (int64_t) s * 1000;
    base_boot_ms_ = millis();
  }
}

void BusJsonlComponent::offer_frame(uint32_t ms, uint8_t src, uint8_t dst, uint8_t func,
                                     uint16_t reg, const uint8_t *data, size_t len) {
  rebase_epoch_();

  char buf[LINE_BUF_MAX];
  int n;
  if (last_sec_ > 0) {
    // int32 sign-extension: ms is stamped by the hub microseconds before
    // rebase_epoch_ latches base_boot_ms_, so the difference can be -1/-2 ms;
    // a uint32 cast would wrap that to +2^32 (observed as +49.7-day jumps).
    int64_t epoch_ms = base_epoch_ms_ + (int64_t) (int32_t) (ms - base_boot_ms_);
    time_t secs = (time_t) (epoch_ms / 1000);
    struct tm tm;
    gmtime_r(&secs, &tm);
    n = snprintf(buf, sizeof(buf),
                 "{\"ts\":\"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ\","
                 "\"src\":\"%02X\",\"dst\":\"%02X\",\"func\":\"%02X\",\"reg\":\"%04X\",\"data\":",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (int) (epoch_ms % 1000),
                 src, dst, func, reg);
  } else {
    // No (or unsynced) time source: boot-relative milliseconds.
    n = snprintf(buf, sizeof(buf),
                 "{\"ms\":%u,\"src\":\"%02X\",\"dst\":\"%02X\",\"func\":\"%02X\",\"reg\":\"%04X\",\"data\":",
                 (unsigned) ms, src, dst, func, reg);
  }
  if (n < 0 || (size_t) n >= sizeof(buf))
    return;
  size_t pos = (size_t) n;

  // Pointer, not length, carries the null/empty distinction: data != nullptr
  // renders a string ("" for frames with no data section, i.e. reads);
  // data == nullptr renders null, reserved for the oversize anomaly so
  // null has unique meaning in a capture.
  if (data != nullptr && len > 0) {
    // +1 NUL, 2 quotes, and the closing '}' must fit alongside the hex.
    if (pos + len * 2 + 5 >= sizeof(buf)) {
      // Oversize payload: keep the frame line, drop the payload (null).
      data = nullptr;
      len = 0;
    }
  }

  if (data != nullptr) {
    buf[pos++] = '"';
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
      buf[pos++] = HEX[data[i] >> 4];
      buf[pos++] = HEX[data[i] & 0x0F];
    }
    buf[pos++] = '"';
  } else {
    buf[pos++] = 'n';
    buf[pos++] = 'u';
    buf[pos++] = 'l';
    buf[pos++] = 'l';
  }
  buf[pos++] = '}';
  // '\n' terminates the JSONL line; plain LF (not CRLF) for jsonl tooling.

  // Make room: drop oldest lines until the new one fits both caps.
  while (!queue_.empty() &&
         (queue_.size() >= MAX_QUEUED_LINES || queued_bytes_ + pos > MAX_QUEUED_BYTES)) {
    queued_bytes_ -= queue_.front().size();
    queue_.pop_front();
    dropped_lines_++;
  }
  queue_.emplace_back(buf, pos);
  queued_bytes_ += pos;

  if (dropped_lines_ > 0 && millis() - last_drop_log_ms_ > 60000) {
    ESP_LOGW(TAG, "queue overflow: %u lines dropped total (drain slower than dispatch)",
             (unsigned) dropped_lines_);
    last_drop_log_ms_ = millis();
  }
}

void BusJsonlComponent::loop() {
  // Output-only: consume and discard client input so the server's RX ring
  // drains instead of churning drop-oldest. One throttled log line per
  // minute explains the silence to anyone expecting a command prompt
  // (the SAM ASCII port on 23 is the interactive one).
  if (available() > 0) {
    uint8_t sink[64];
    while (available() > 0) {
      size_t n = std::min(available(), sizeof(sink));
      read_array(sink, n);
    }
    if (millis() - last_input_log_ms_ > 60000) {
      ESP_LOGI(TAG, "stream is output-only; ignoring client input");
      last_input_log_ms_ = millis();
    }
  }

  if (queue_.empty())
    return;
  const std::string &line = queue_.front();
  write_array(reinterpret_cast<const uint8_t *> (line.data()), line.size());
  static const uint8_t NEWLINE[] = {'\n'};
  write_array(NEWLINE, 1);
  queued_bytes_ -= line.size();
  queue_.pop_front();
}

}  // namespace bus_jsonl
}  // namespace esphome
