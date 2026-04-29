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

// Weekday names indexed by day value (0=SUNDAY .. 6=SATURDAY)
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

  // Read available bytes into line buffer
  while (available()) {
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

// Format a temperature value (stored as °F uint8) with unit suffix.
// If dealer register says Celsius, convert: °C = (°F-32)*5/9.
std::string SamAsciiComponent::format_temp_(uint8_t temp_f) {
  using namespace infinitesp;
  auto *dealer = parent_->get_register(parent_->get_address(), REG_SAM_DEALER);
  char unit = 'F';
  if (dealer && dealer->size() > 7)
    unit = (char) (*dealer)[7];

  char buf[16];
  if (unit == 'C' || unit == 'c') {
    float temp_c = ((float) temp_f - 32.0f) * 5.0f / 9.0f;
    snprintf(buf, sizeof(buf), "%.1f%sC", temp_c, DEGREE_UTF8);
  } else {
    snprintf(buf, sizeof(buf), "%d%sF", temp_f, DEGREE_UTF8);
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
    respond_(prefix, "COMMANDS: MODE RT RH HTSP CLSP FAN HOLD OAT TIME DAY ZONE NAME BLIGHT CFGEM");
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
  auto *state = parent_->get_register(parent_->get_address(), REG_SAM_STATE);     // 3B02
  auto *zones = parent_->get_register(parent_->get_address(), REG_SAM_ZONES);      // 3B03
  auto *dealer = parent_->get_register(parent_->get_address(), REG_SAM_DEALER);    // 3B06

  // ---- System-level commands (no zone) ----

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
    if (!state || state->size() <= REG3B02_WEEKDAY) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t day = (*state)[REG3B02_WEEKDAY];
    respond_(prefix, (day < 7) ? DAY_NAMES[day] : "UNKNOWN");

  } else if (body == "TIME") {
    if (!state || state->size() < REG3B02_MINUTES + 2) {
      respond_nak_(prefix, "");
      return;
    }
    // uint16 LE: minutes since midnight
    uint16_t minutes = (uint16_t) (*state)[REG3B02_MINUTES] |
                       ((uint16_t) (*state)[REG3B02_MINUTES + 1] << 8);
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
    if (!zones || zones->size() <= REG3B03_HEAT_SETPOINTS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*zones)[REG3B03_HEAT_SETPOINTS + idx]));

  } else if (body == "CLSP") {
    if (!zones || zones->size() <= REG3B03_COOL_SETPOINTS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, format_temp_((*zones)[REG3B03_COOL_SETPOINTS + idx]));

  } else if (body == "FAN") {
    if (!zones || zones->size() <= REG3B03_FAN_MODES + idx) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t fan = (*zones)[REG3B03_FAN_MODES + idx];
    respond_(prefix, (fan < FAN_COUNT) ? FAN_NAMES[fan] : "UNKNOWN");

  } else if (body == "HOLD") {
    if (!zones || zones->size() <= REG3B03_ZONES_HOLDING) {
      respond_nak_(prefix, "");
      return;
    }
    uint8_t holding = (*zones)[REG3B03_ZONES_HOLDING];
    respond_(prefix, (holding & (1 << idx)) ? "ON" : "OFF");

  } else if (body == "NAME") {
    if (!zones || zones->size() < REG3B03_ZONE_NAMES + idx * 12 + 12) {
      respond_nak_(prefix, "");
      return;
    }
    std::string name;
    for (size_t i = 0; i < 12; i++) {
      char c = (char) (*zones)[REG3B03_ZONE_NAMES + idx * 12 + i];
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
    if (!dealer || dealer->size() <= 7) {
      respond_nak_(prefix, "");
      return;
    }
    char unit = (char) (*dealer)[7];
    respond_(prefix, std::string(1, unit));

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
