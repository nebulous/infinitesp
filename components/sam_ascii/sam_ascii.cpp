#include "sam_ascii.h"
#include "esphome/core/log.h"
#include "esphome/components/infinitesp/infinitesp.h"

namespace esphome {
namespace sam_ascii {

static const char *const TAG = "sam_ascii";

// Mode names indexed by SYSMODE_* values (0=HEAT, 1=COOL, 2=AUTO, 3=EHEAT, 4=OFF)
static const char *const MODE_NAMES[] = {"HEAT", "COOL", "AUTO", "EHEAT", "OFF"};
static const uint8_t MODE_COUNT = 5;

// Fan mode names indexed by FAN_* values (0=AUTO, 1=LOW, 2=MED, 3=HIGH)
static const char *const FAN_NAMES[] = {"AUTO", "LOW", "MED", "HIGH"};
static const uint8_t FAN_COUNT = 4;

// Weekday names indexed by bus day value.
// 0=SUNDAY through 6=SATURDAY. Confirmed 2026-06-06: Saturday=6.
static const char *const DAY_NAMES[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY",
                                        "THURSDAY", "FRIDAY", "SATURDAY"};

// UTF-8 degree symbol for temperature output
static const char DEGREE_UTF8[] = "\xC2\xB0";

void SamAsciiComponent::setup() {
  ESP_LOGI(TAG, "SAM ASCII protocol server started");
}

void SamAsciiComponent::loop() {
  if (!parent_)
    return;

  const uint32_t now = millis();

  // Reset banner flag when idle so next connection triggers it
  if (banner_sent_ && last_activity_time_ != 0 &&
      (now - last_activity_time_) > CONNECTION_IDLE_MS) {
    banner_sent_ = false;
  }

  // Read available bytes into line buffer
  while (available()) {
    // Detect new connection: bytes arriving after long silence (or first byte ever)
    if (!banner_sent_) {
      if (last_activity_time_ != 0)
        ESP_LOGI(TAG, "New connection detected (idle %.1fs)",
                 (now - last_activity_time_) / 1000.0f);
      write_str(
          " _____       __ _       _ _   _____ ___________   _____  ___  ___  ___\r\n"
          "|_   _|     / _(_)     (_) | |  ___/  ___| ___ \\ /  ___|/ _ \\ |  \\/  |\r\n"
          "  | | _ __ | |_ _ _ __  _| |_| |__ \\ `--.| |_/ / \\ `--./ /_\\ \\| .  . |\r\n"
          "  | || '_ \\|  _| | '_ \\| | __|  __| `--. \\  __/   `--. \\  _  || |\\/| |\r\n"
          " _| || | | | | | | | | | | |_| |___/\__/ / |     /\__/ / | | || |  | |\r\n"
          " \\___/_| |_|_| |_|_| |_|_|\\__\\____/\\____/\\_|     \\____/\\_| |_/\\_|  |_/\r\n");
      flush();
      banner_sent_ = true;
    }
    last_activity_time_ = now;

    uint8_t byte;
    read_byte(&byte);

    // Inter-char timeout: reset buffer if too long between chars
    // Only check when buffer is non-empty (partial command in progress)
    if (!line_buf_.empty() && last_char_time_ != 0 &&
        (now - last_char_time_) > INTER_CHAR_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Inter-char timeout, discarding buffer: '%s'", line_buf_.c_str());
      respond_nak_(line_buf_, "CMD");
      line_buf_.clear();
    }
    last_char_time_ = now;

    // Buffer overflow
    if (line_buf_.size() >= MAX_LINE) {
      ESP_LOGW(TAG, "Line too long, discarding buffer");
      respond_nak_("", "CMD");
      line_buf_.clear();
      continue;
    }

    // End of line
    if (byte == '\n') {
      // Strip trailing \r if present
      if (!line_buf_.empty() && line_buf_.back() == '\r')
        line_buf_.pop_back();

      if (!line_buf_.empty()) {
        process_line_(line_buf_);
      }
      line_buf_.clear();
      continue;
    }

    // Skip bare \r
    if (byte == '\r')
      continue;

    // Strip non-printable bytes (telnet IAC negotiation, binary noise)
    if (byte < 0x20 || byte >= 0x7F)
      continue;

    line_buf_ += (char) byte;
  }
}

// Format a temperature value (stored in bus units, °F or °C) with unit suffix.
// Uses parent's detected/configured unit setting, not the SAM 3B06 register.
std::string SamAsciiComponent::format_temp_(uint8_t temp_bus) {
  using namespace infinitesp;
  char buf[16];
  if (parent_->bus_uses_celsius()) {
    // Bus value is already °C
    snprintf(buf, sizeof(buf), "%d%sC", temp_bus, DEGREE_UTF8);
  } else {
    // Bus value is °F
    snprintf(buf, sizeof(buf), "%d%sF", temp_bus, DEGREE_UTF8);
  }
  return std::string(buf);
}

// Format minutes-since-midnight as 12-hour time with A/P suffix.
// 0 → "12:00 A", 480 → "08:00 A", 720 → "12:00 P", 810 → "01:30 P"
std::string SamAsciiComponent::format_time_(uint16_t minutes) {
  if (minutes > 1439)
    minutes = 0;
  uint8_t hour24 = minutes / 60;
  uint8_t min = minutes % 60;
  uint8_t hour12 = hour24 % 12;
  if (hour12 == 0)
    hour12 = 12;
  const char *ampm = (hour24 < 12) ? "A" : "P";
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d %s", hour12, min, ampm);
  return std::string(buf);
}

void SamAsciiComponent::process_line_(const std::string &line) {
  ESP_LOGD(TAG, "RX: '%s'", line.c_str());

  // Uppercase the input for case-insensitive matching
  std::string cmd = line;
  for (auto &c : cmd)
    c = toupper(c);

  // Build response prefix: full command minus trailing '?'
  std::string prefix = cmd;
  if (!prefix.empty() && prefix.back() == '?')
    prefix.pop_back();

  // HELP anywhere in the command triggers help response (works even with bus offline)
  if (cmd.find("HELP") != std::string::npos) {
    respond_(prefix, "COMMANDS: MODE RT RH HTSP CLSP FAN HOLD OAT TIME DAY ZONE NAME BLIGHT CFGEM REPORT");
    respond_(prefix, "SET: MODE!<mode> Z#HTSP!<temp> Z#CLSP!<temp> Z#FAN!<mode> Z#HOLD!<on|off|minutes>");
    return;
  }

  // REPORT? streams full JSON bus diagnostic (works even when bus offline)
  if (cmd.find("REPORT") != std::string::npos) {
    auto write_cb = [](const uint8_t *data, size_t len, void *ctx) {
      auto *self = static_cast<SamAsciiComponent *>(ctx);
      self->write_array(data, len);
    };
    parent_->stream_bus_report_(write_cb, this);
    flush();
    return;
  }

  // Check bus online
  if (!parent_->is_bus_online()) {
    respond_nak_(prefix, "");
    return;
  }

  // Strip S# prefix (system number, e.g., "S1")
  size_t pos = 0;
  if (cmd.size() >= 2 && cmd[0] == 'S' && cmd[1] >= '1' && cmd[1] <= '9')
    pos = 2;

  std::string body = cmd.substr(pos);

  // Strip trailing '?'
  if (!body.empty() && body.back() == '?')
    body.pop_back();

  // Parse optional zone prefix Z# (1-based, zones 1-8)
  uint8_t zone = 1;   // default zone
  if (body.size() >= 2 && body[0] == 'Z' && body[1] >= '1' && body[1] <= '8') {
    zone = body[1] - '0';
    body = body.substr(2);
  }
  uint8_t idx = zone - 1;  // 0-based index into register arrays

  using namespace infinitesp;

  // Get register data pointers (SAM registers stored under SAM address)
  auto *state = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);     // 3B02
  auto *zones_data = parent_->get_register(parent_->get_sam_address(), REG_SAM_ZONES);      // 3B03
  auto *dealer = parent_->get_register(parent_->get_sam_address(), REG_SAM_DEALER);    // 3B06

  // ---- Write commands (! separator) ----
  size_t bang = body.find('!');
  if (bang != std::string::npos) {
    std::string write_cmd = body.substr(0, bang);
    std::string write_val = body.substr(bang + 1);

    if (write_cmd == "MODE") {
      uint8_t new_mode = 0xFF;
      for (uint8_t i = 0; i < MODE_COUNT; i++) {
        if (write_val == MODE_NAMES[i]) { new_mode = i; break; }
      }
      if (new_mode == 0xFF) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_system_mode(new_mode);
      respond_(prefix, "ACK");
      return;
    }

    if (write_cmd == "HTSP") {
      int temp = atoi(write_val.c_str());
      if (temp < 40 || temp > 99) { respond_nak_(prefix, "VAL"); return; }
      uint8_t cool_sp = (zones_data && zones_data->size() > REG3B03_COOL_SETPOINTS + idx)
                            ? (*zones_data)[REG3B03_COOL_SETPOINTS + idx] : 0;
      parent_->set_zone_setpoint(zone, (uint8_t) temp, cool_sp);
      respond_(prefix, "ACK");
      return;
    }

    if (write_cmd == "CLSP") {
      int temp = atoi(write_val.c_str());
      if (temp < 40 || temp > 99) { respond_nak_(prefix, "VAL"); return; }
      uint8_t heat_sp = (zones_data && zones_data->size() > REG3B03_HEAT_SETPOINTS + idx)
                            ? (*zones_data)[REG3B03_HEAT_SETPOINTS + idx] : 0;
      parent_->set_zone_setpoint(zone, heat_sp, (uint8_t) temp);
      respond_(prefix, "ACK");
      return;
    }

    if (write_cmd == "FAN") {
      uint8_t new_fan = 0xFF;
      for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (write_val == FAN_NAMES[i]) { new_fan = i; break; }
      }
      if (new_fan == 0xFF) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_zone_fan(zone, new_fan);
      respond_(prefix, "ACK");
      return;
    }

    if (write_cmd == "HOLD") {
      if (write_val == "ON") {
        parent_->set_zone_hold(zone, InfinitESPComponent::HOLD_PERMANENT);
      } else if (write_val == "OFF") {
        parent_->set_zone_hold(zone, 0);
      } else {
        int minutes = atoi(write_val.c_str());
        if (minutes <= 0) { respond_nak_(prefix, "VAL"); return; }
        parent_->set_zone_hold(zone, (uint16_t) minutes);
      }
      respond_(prefix, "ACK");
      return;
    }

    respond_nak_(prefix, "CMD");
    return;
  }

  // ---- System-level read commands (no zone) ----

  if (body == "MODE") {
    if (!state || state->size() <= REG3B02_STAGMODE) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t stagmode = (*state)[REG3B02_STAGMODE];
    uint8_t mode = stagmode & 0x0F;
    uint8_t stage = (stagmode >> 4) & 0x0F;
    std::string value = (mode < MODE_COUNT) ? MODE_NAMES[mode] : "UNKNOWN";
    if (stage > 0)
      value += std::to_string(stage);
    respond_(prefix, value);

  } else if (body == "OAT") {
    if (!state || state->size() <= REG3B02_OUTDOOR_TEMP) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*state)[REG3B02_OUTDOOR_TEMP]));

  } else if (body == "DAY") {
    if (!parent_->has_real_state()) {
      respond_(prefix, "SYNC");
      return;
    }
    if (!state || state->size() <= REG3B02_WEEKDAY) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t day = (*state)[REG3B02_WEEKDAY];
    respond_(prefix, (day < 7) ? DAY_NAMES[day] : "UNKNOWN");

  } else if (body == "TIME") {
    if (!parent_->has_real_state()) {
      respond_(prefix, "SYNC");
      return;
    }
    if (!state || state->size() < REG3B02_MINUTES + 2) {
      respond_nak_(prefix, "");
      return;
    }
    // uint16 BE: minutes since midnight
    uint16_t minutes = ((uint16_t) (*state)[REG3B02_MINUTES] << 8) |
                       (uint16_t) (*state)[REG3B02_MINUTES + 1];
    respond_(prefix, format_time_(minutes));

  } else if (body == "ZONE") {
    if (!state || state->size() <= REG3B02_DISPLAYED_ZONE) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, std::to_string((*state)[REG3B02_DISPLAYED_ZONE]));

  // ---- Zone-level commands ----

  } else if (body == "RT") {
    if (!state || state->size() <= REG3B02_TEMPS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*state)[REG3B02_TEMPS + idx]));

  } else if (body == "RH") {
    if (!state || state->size() <= REG3B02_HUMIDITY + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, std::to_string((*state)[REG3B02_HUMIDITY + idx]) + "%");

  } else if (body == "HTSP") {
    if (!zones_data || zones_data->size() <= REG3B03_HEAT_SETPOINTS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*zones_data)[REG3B03_HEAT_SETPOINTS + idx]));

  } else if (body == "CLSP") {
    if (!zones_data || zones_data->size() <= REG3B03_COOL_SETPOINTS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*zones_data)[REG3B03_COOL_SETPOINTS + idx]));

  } else if (body == "FAN") {
    if (!zones_data || zones_data->size() <= REG3B03_FAN_MODES + idx) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t fan = (*zones_data)[REG3B03_FAN_MODES + idx];
    respond_(prefix, (fan < FAN_COUNT) ? FAN_NAMES[fan] : "UNKNOWN");

  } else if (body == "HOLD") {
    if (!zones_data || zones_data->size() < REG3B03_HOLD_DURATIONS + idx * 2 + 2) {
      respond_nak_(prefix, "");
      return;
    }
    uint16_t hold_dur = ((uint16_t) (*zones_data)[REG3B03_HOLD_DURATIONS + idx * 2] << 8) |
                        (*zones_data)[REG3B03_HOLD_DURATIONS + idx * 2 + 1];
    if (hold_dur == 0) {
      respond_(prefix, "OFF");
    } else if (hold_dur >= InfinitESPComponent::HOLD_PERMANENT) {
      respond_(prefix, "PERMANENT");
    } else {
      std::string end = parent_->format_hold_end(hold_dur);
      if (!end.empty())
        respond_(prefix, "ON until " + end);
      else
        respond_(prefix, "ON (" + std::to_string(hold_dur) + " min)");
    }

  } else if (body == "NAME") {
    if (!zones_data || zones_data->size() < REG3B03_ZONE_NAMES + idx * 12 + 12) {
      respond_nak_(prefix, "");
      return;
    }
    std::string name;
    for (size_t i = 0; i < 12; i++) {
      char c = (char) (*zones_data)[REG3B03_ZONE_NAMES + idx * 12 + i];
      if (c == 0)
        break;
      name += c;
    }
    respond_(prefix, name.empty() ? ("Zone " + std::to_string(zone)) : name);

  } else if (body == "BLIGHT") {
    if (!dealer || dealer->empty()) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, (*dealer)[0] ? "ON" : "OFF");

  } else if (body == "CFGEM") {
    respond_(prefix, parent_->bus_uses_celsius() ? "C" : "F");

  } else {
    // Unknown command
    respond_nak_(prefix, "CMD");
  }
}

void SamAsciiComponent::respond_(const std::string &prefix, const std::string &value) {
  std::string response = prefix + ": " + value + "\r\n";
  ESP_LOGD(TAG, "TX: '%s'", response.c_str());
  write_str(response.c_str());
  flush();
}

void SamAsciiComponent::respond_nak_(const std::string &prefix, const std::string &reason) {
  std::string response;
  if (!prefix.empty())
    response = prefix + ": ";
  if (reason.empty())
    response += "NAK";
  else
    response += "NAK " + reason;
  response += "\r\n";
  ESP_LOGD(TAG, "TX: '%s'", response.c_str());
  write_str(response.c_str());
  flush();
}

}  // namespace sam_ascii
}  // namespace esphome
