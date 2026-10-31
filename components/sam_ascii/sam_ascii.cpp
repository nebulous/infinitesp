#include "sam_ascii.h"
#include "esphome/core/log.h"
#include "esphome/components/infinitesp/infinitesp.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace sam_ascii {

static const char *const TAG = "sam_ascii";

// Mode names indexed by SYSMODE_* values (0=HEAT, 1=COOL, 2=AUTO, 3=EHEAT,
// 4=HEATPUMP, 5=OFF). Fully dense.
static const char *const MODE_NAMES[] = {"HEAT", "COOL", "AUTO", "EHEAT", "HEATPUMP", "OFF"};
static const uint8_t MODE_COUNT = 6;

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

  // Drain diagnostic engine events (SCAN? results, REG?/REG! completions)
  // whether or not input arrived this pass.
  diag_drain_();

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
          " _| || | | | | | | | | | | |_| |___/\\__/ / |     /\\__/ / | | || |  | |\r\n"
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

// Day-of-week from a civil date (Howard Hinnant's days_from_civil).
// Bus encoding: 0=Sunday..6=Saturday; 1970-01-01 was a Thursday (4).
static uint8_t weekday_from_date_(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);            // [0, 399]
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; // [0, 146096]
  const int days = (int) (era * 146097 + (int) doe - 719468);
  return (uint8_t) (((days + 4) % 7 + 7) % 7);
}

static bool is_leap_year_(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

bool SamAsciiComponent::parse_clock_value_(const std::string &val, uint8_t &weekday, uint16_t &minutes) {
  using namespace infinitesp;
  weekday = InfinitESPComponent::CLOCK_KEEP_WEEKDAY;
  minutes = InfinitESPComponent::CLOCK_KEEP_MINUTES;

  // ISO8601 extension: strict naive local "YYYY-MM-DDTHH:MM" (16 chars;
  // input is already uppercased, so a lowercase 't' separator also lands
  // here). Anything longer ("...Z", "+HH:MM" offsets) is rejected rather
  // than converted - the bus carries local wall time and a silent TZ
  // conversion with the wrong zone would write a wrong clock.
  if (val.size() == 16 && val[4] == '-' && val[7] == '-' && val[10] == 'T' && val[13] == ':') {
    auto digits = [&](size_t from, size_t count) -> bool {
      for (size_t i = from; i < from + count; i++)
        if (val[i] < '0' || val[i] > '9')
          return false;
      return true;
    };
    if (!digits(0, 4) || !digits(5, 2) || !digits(8, 2) || !digits(11, 2) || !digits(14, 2))
      return false;
    auto num = [&](size_t from, size_t count) -> int {
      int v = 0;
      for (size_t i = from; i < from + count; i++)
        v = v * 10 + (val[i] - '0');
      return v;
    };
    int y = num(0, 4);
    int mo = num(5, 2);
    int d = num(8, 2);
    int hh = num(11, 2);
    int mm = num(14, 2);
    static const int mdays[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    // Range-check the month BEFORE indexing mdays (a two-digit month can be
    // up to 99; mdays[mo] would read out of bounds).
    if (mo < 1 || mo > 12 || d < 1 || hh > 23 || mm > 59)
      return false;
    int dmax = (mo == 2 && is_leap_year_(y)) ? 29 : mdays[mo];
    if (d > dmax)
      return false;
    weekday = weekday_from_date_(y, mo, d);
    minutes = (uint16_t) (hh * 60 + mm);
    return true;
  }

  // SAM-native 12-hour "HH:MM" + optional single space + 'A'/'P'
  // (SAM01-04XA Table 1 ex 8: "08:10A" ACKs; ex 9: a missing leading zero
  // NAKs VAL; the optional space before the meridiem is a lenient extension
  // - the SAM's own read format prints "HH:MM A").
  size_t len = val.size();
  size_t meridiem_pos = len - 1;
  if (len < 6 || len > 7)
    return false;
  char mer = val[meridiem_pos];
  if (mer != 'A' && mer != 'P')
    return false;
  if (len == 7 && val[5] != ' ')
    return false;
  if (val[2] != ':')
    return false;
  for (int i : {0, 1, 3, 4})
    if (val[i] < '0' || val[i] > '9')
      return false;
  int hh = (val[0] - '0') * 10 + (val[1] - '0');
  int mm = (val[3] - '0') * 10 + (val[4] - '0');
  if (hh < 1 || hh > 12 || mm > 59)
    return false;
  uint16_t m = (uint16_t) ((hh % 12) * 60 + mm);
  if (mer == 'P')
    m += 720;
  minutes = m;
  return true;
}

void SamAsciiComponent::process_line_(const std::string &line) {
  ESP_LOGD(TAG, "RX: '%s'", line.c_str());

  // Any input line aborts a running diagnostic scan (bus workbench rule):
  // the engine finishes synchronously and the closing events render through
  // diag_drain_() on the next loop pass. Runs before HELP/REPORT so the
  // rule really is "any input".
  if (diag_scan_streaming_ && parent_->diag_scan_active())
    parent_->diag_scan_abort();

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
    respond_(prefix, "ZONE: Z#RT Z#RH Z#HTSP Z#CLSP Z#FAN Z#HOLD Z#UNOCC Z#RHTG Z#OVR Z#OTMR Z#NAME");
    respond_(prefix, "ACCESSORY: FILTRLVL UVLVL HUMLVL VENTLVL FILTRRMD UVRMD HUMRMD VENTRMD");
    respond_(prefix, "VACATION: VACAT VACDAYS VACHOURS VACMINT VACMAXT VACMINH VACMAXH VACFAN  CONFIG: CFGDEAD CFGCPH CFGPER CFGPGM DEALER DEALERPH");
    respond_(prefix, "SET: MODE!<mode> Z#HTSP!<temp> Z#CLSP!<temp> Z#FAN!<mode> Z#HOLD!<on|off|minutes> VACDAYS!<days> VACHOURS!<hours> VACMINT!<temp> VACMAXT!<temp> VACFAN!<mode>");
    respond_(prefix, "CLOCK: TIME!<HH:MM A/P | YYYY-MM-DDTHH:MM | NOW> DAY!<0-6>  (NOW = ESPHome time source; one 3B02 write; no auto-sync)");
    respond_(prefix, "BUS: SCAN?[ ALL|addr] REG?[ addr] reg | table row REG![ addr] reg hexrow  (frames from bus address 0x93; REG! needs raw_writes)");
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

  // ---- Bus workbench: SCAN? / REG? / REG! (diagnostic seat 0x93) ----
  // Runs before the bus-online gate: the diagnostics seat initiates its own
  // traffic, so it works in passive installs too, where bus_online_ never
  // flips (no replies are ever addressed to the emulated seats).
  {
    std::string wb = cmd;
    size_t wb_pos = 0;
    if (wb.size() >= 2 && wb[0] == 'S' && wb[1] >= '1' && wb[1] <= '9')
      wb_pos = 2;
    wb = wb.substr(wb_pos);
    wb.erase(std::remove(wb.begin(), wb.end(), '?'), wb.end());
    if (wb.rfind("SCAN", 0) == 0 || wb.rfind("REG", 0) == 0) {
      handle_workbench_(wb);
      return;
    }
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
  auto *accessories = parent_->get_register(parent_->get_sam_address(), REG_SAM_ACCESSORIES);  // 3B05
  auto *dealer = parent_->get_register(parent_->get_sam_address(), REG_SAM_DEALER);    // 3B06
  // (3B04 vacation is not a cached register — see vacation_* getters below.)

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
      // EHEAT/HEATPUMP are experimental and off by default: they do not select
      // the heat source, and the HEATPUMP write has shut down a Next Gen
      // tstat mid-call (issue #18). set_system_mode() gates the same modes
      // for the select path.
      if ((new_mode == SYSMODE_EHEAT || new_mode == SYSMODE_HEATPUMP) &&
          !parent_->heat_source_modes_enabled()) {
        ESP_LOGW(TAG, "MODE %s refused: experimental heat-source modes disabled "
                      "(hub experimental_heat_source_modes)", write_val.c_str());
        respond_nak_(prefix, "VAL");
        return;
      }
      parent_->set_system_mode(new_mode);
      respond_(prefix, "ACK");
      return;
    }

    if (write_cmd == "ZONE") {
      int zone = atoi(write_val.c_str());
      // System-level: sets 3B02 byte 28 regardless of any S<N>/zone prefix
      // in the command. Adoption is generation-dependent (works on UIZ,
      // ACKed-but-ignored by newer touch units); the ACK reflects transport,
      // not adoption.
      if (zone < 1 || zone > 8) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_displayed_zone((uint8_t) zone);
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
      ESP_LOGI(TAG, "ASCII Z%uHOLD!%s", zone, write_val.c_str());
      if (write_val == "ON") {
        parent_->set_zone_hold(zone, InfinitESPComponent::HOLD_PERMANENT, "ascii");
      } else if (write_val == "OFF") {
        parent_->set_zone_hold(zone, 0, "ascii");
      } else {
        int minutes = atoi(write_val.c_str());
        if (minutes <= 0) { respond_nak_(prefix, "VAL"); return; }
        parent_->set_zone_hold(zone, (uint16_t) minutes, "ascii");
      }
      respond_(prefix, "ACK");
      return;
    }

    // ---- Vacation writes (3B04) ----
    if (write_cmd == "VACDAYS") {
      int days = atoi(write_val.c_str());
      if (days < 0 || days > 365) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_vacation_days((uint16_t) days);
      respond_(prefix, "ACK");
      return;
    }
    // Native 1-h resolution (issue #33). InfinitESP extension — the vendor
    // SAM01 grammar has no hours verb; rides the same setter as the number
    // entity. 8760 = the 365-day UI/SAM ceiling.
    if (write_cmd == "VACHOURS") {
      int hours = atoi(write_val.c_str());
      if (hours < 0 || hours > 8760) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_vacation_hours((uint16_t) hours);
      respond_(prefix, "ACK");
      return;
    }
    if (write_cmd == "VACMINT" || write_cmd == "VACMAXT") {
      int t = atoi(write_val.c_str());
      int lo = parent_->bus_uses_celsius() ? 4 : 40;
      int hi = parent_->bus_uses_celsius() ? 37 : 99;
      if (t < lo || t > hi) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_vacation_temp(write_cmd == "VACMINT", (uint8_t) t);
      respond_(prefix, "ACK");
      return;
    }
    if (write_cmd == "VACMINH" || write_cmd == "VACMAXH") {
      int h = atoi(write_val.c_str());
      if (h < 0 || h > 100) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_vacation_humidity(write_cmd == "VACMINH", (uint8_t) h);
      respond_(prefix, "ACK");
      return;
    }
    if (write_cmd == "VACFAN") {
      uint8_t fan = 0xFF;
      for (uint8_t i = 0; i < FAN_COUNT; i++) {
        if (write_val == FAN_NAMES[i]) { fan = i; break; }
      }
      if (fan == 0xFF) { respond_nak_(prefix, "VAL"); return; }
      parent_->set_vacation_fan(fan);
      respond_(prefix, "ACK");
      return;
    }

    // ---- Clock writes (3B02 time ride-along, issue #45 phase 1) ----
    // TIME!/DAY! mimic the physical SAM01-04XA verbs (Table 1 ex 6-9, 22):
    // TIME! takes 12-hour "HH:MM A/P" with leading zeros; DAY! takes a bare
    // digit 0-6 (0=Sunday). The ISO8601 datetime form of TIME! is an
    // InfinitESP extension that derives weekday + minutes and sets both in
    // one write. One 3B02 write per command: every applied clock write
    // resets the UIZ-family seconds counter, so the surface is deliberately
    // manual-only (no auto-sync; Q11).
    if (write_cmd == "TIME" || write_cmd == "DAY") {
      // Without a filled 3B02 mirror there is nothing to carry and no write
      // can be built - plain NAK, mirroring the read-side SYNC gate.
      if (!parent_->has_real_state()) { respond_nak_(prefix, ""); return; }
      if (write_cmd == "DAY") {
        // Strict single digit (Table 1 ex 22: values outside 0-6 NAK VAL;
        // bare atoi would silently read "SUNDAY" as 0).
        if (write_val.size() != 1 || write_val[0] < '0' || write_val[0] > '6') {
          respond_nak_(prefix, "VAL");
          return;
        }
        parent_->set_displayed_clock((uint8_t) (write_val[0] - '0'),
                                     InfinitESPComponent::CLOCK_KEEP_MINUTES);
      } else {
        // TIME!NOW — InfinitESP extension (clock phase 2/3): the shared
        // manual-sync core (same path as the infinitesp.sync_clock action
        // and the Sync Thermostat Clock button). Local wall time from the
        // hub's ESPHome time source; refuses (plain NAK + log) when no
        // source is configured, ESP time is invalid, mirror unfilled, or
        // SAM emulation is off — a garbage clock is never written. Single
        // 0x180 write: weekday+minutes together.
        if (write_val == "NOW") {
          std::string detail;
          if (!parent_->sync_clock_from_source(detail)) {
            respond_nak_(prefix, "");
            return;
          }
        } else {
          uint8_t weekday;
          uint16_t minutes;
          if (!parse_clock_value_(write_val, weekday, minutes)) {
            respond_nak_(prefix, "VAL");
            return;
          }
          parent_->set_displayed_clock(weekday, minutes);
        }
      }
      // ACK reflects transport (frame queued), not thermostat adoption -
      // the Touch generation sleep-queues 3B02 writes until the wall UI
      // wakes (same convention as ZONE!).
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

  // ---- Zone read commands (3B02 / 3B03) ----
  // 3B02 byte 21 zones_unoccupied bitmask: bit (zone-1) set = unoccupied.
  } else if (body == "UNOCC") {
    if (!state || state->size() <= REG3B02_UNOCCUPIED) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, ((*state)[REG3B02_UNOCCUPIED] & (1 << idx)) ? "ON" : "OFF");

  // 3B03 byte 28+idx humidity_setpoint (humidification target, %).
  } else if (body == "RHTG") {
    if (!zones_data || zones_data->size() <= REG3B03_HUMIDITY_SETPOINTS + idx) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, std::to_string((*zones_data)[REG3B03_HUMIDITY_SETPOINTS + idx]) + "%");

  // Override state: a finite ("hold until") timer is active for this zone.
  // get_zone_hold_duration() returns HOLD_PERMANENT for permanent holds, so a
  // finite value >0 means a timed override is running. (Spec: Touch OVR == the
  // "hold until" timer.)
  } else if (body == "OVR") {
    uint16_t dur = parent_->get_zone_hold_duration(zone);
    respond_(prefix, (dur > 0 && dur != InfinitESPComponent::HOLD_PERMANENT) ? "ON" : "OFF");

  // Override timer remaining as HH:MM. hold_duration is the set duration in
  // minutes (the thermostat, not InfinitESP, counts it down), so this reports
  // the configured duration rather than a live decrementing value.
  } else if (body == "OTMR") {
    uint16_t dur = parent_->get_zone_hold_duration(zone);
    if (dur == 0 || dur == InfinitESPComponent::HOLD_PERMANENT) {
      respond_(prefix, "00:00");
    } else {
      char buf[8];
      snprintf(buf, sizeof(buf), "%02d:%02d", dur / 60, dur % 60);
      respond_(prefix, buf);
    }

  } else if (body == "BLIGHT") {
    if (!dealer || dealer->empty()) {
      respond_nak_(prefix, "");
      return;
    }
    respond_(prefix, (*dealer)[0] ? "ON" : "OFF");

  } else if (body == "CFGEM") {
    respond_(prefix, parent_->bus_uses_celsius() ? "C" : "F");

  // ---- Accessory life & reminders (3B05) ----
  // Values are consumption % (0=new, 100=replace); reminder bytes are 0/1.
  } else if (body == "FILTRLVL" || body == "UVLVL" || body == "HUMLVL" || body == "VENTLVL") {
    uint8_t off = (body == "FILTRLVL") ? REG3B05_FILTER : (body == "UVLVL") ? REG3B05_UV :
                  (body == "HUMLVL") ? REG3B05_HUMIDIFIER : REG3B05_VENTILATOR;
    if (!accessories || accessories->size() <= off) { respond_nak_(prefix, ""); return; }
    respond_(prefix, std::to_string((*accessories)[off]) + "%");
  } else if (body == "FILTRRMD" || body == "UVRMD" || body == "HUMRMD" || body == "VENTRMD") {
    uint8_t off = (body == "FILTRRMD") ? REG3B05_FILTER_RMD : (body == "UVRMD") ? REG3B05_UV_RMD :
                  (body == "HUMRMD") ? REG3B05_HUMIDIFIER_RMD : REG3B05_VENTILATOR_RMD;
    if (!accessories || accessories->size() <= off) { respond_nak_(prefix, ""); return; }
    respond_(prefix, (*accessories)[off] ? "ON" : "OFF");

  // ---- Vacation ----
  // Config lives in InfinitESP vacation_* members and is pushed to the
  // thermostat as SAM.0x3B04 change-frames (see REG3B04_FLAG_* in the header).
  // VACAT is derived from hours (>0 = active); VACDAYS is the rounded-up
  // whole-days view, VACHOURS the native resolution.
  } else if (body == "VACAT") {
    respond_(prefix, parent_->get_vacation_days() > 0 ? "ON" : "OFF");
  } else if (body == "VACDAYS") {
    char buf[8];
    snprintf(buf, sizeof(buf), "%03u", parent_->get_vacation_days());
    respond_(prefix, buf);
  } else if (body == "VACHOURS") {
    char buf[8];
    snprintf(buf, sizeof(buf), "%04u", parent_->get_vacation_hours());
    respond_(prefix, buf);
  } else if (body == "VACMINT" || body == "VACMAXT") {
    uint8_t t = (body == "VACMINT") ? parent_->get_vacation_min_temp() : parent_->get_vacation_max_temp();
    respond_(prefix, format_temp_(t));
  } else if (body == "VACMINH" || body == "VACMAXH") {
    uint8_t h = (body == "VACMINH") ? parent_->get_vacation_min_humidity() : parent_->get_vacation_max_humidity();
    char buf[8];
    snprintf(buf, sizeof(buf), "%03d%%", h);
    respond_(prefix, buf);
  } else if (body == "VACFAN") {
    uint8_t fan = parent_->get_vacation_fan();
    respond_(prefix, (fan < FAN_COUNT) ? FAN_NAMES[fan] : "UNKNOWN");

  // ---- Configuration (3B06) ----
  // CFGAUTO is not served: no auto_mode field is confirmed in the current 3B06
  // guess (byte 1 is the live-confirmed metric_units flag, leaving no room for
  // an auto_mode byte there).
  } else if (body == "CFGDEAD" || body == "CFGCPH" || body == "CFGPER") {
    uint8_t off = (body == "CFGDEAD") ? REG3B06_DEADBAND : (body == "CFGCPH") ?
                  REG3B06_CYCLES_PER_HOUR : REG3B06_SCHEDULE_PERIODS;
    if (!dealer || dealer->size() <= off) { respond_nak_(prefix, ""); return; }
    respond_(prefix, std::to_string((*dealer)[off]));
  } else if (body == "CFGPGM") {
    if (!dealer || dealer->size() <= REG3B06_PROGRAMS_ENABLED) { respond_nak_(prefix, ""); return; }
    respond_(prefix, (*dealer)[REG3B06_PROGRAMS_ENABLED] ? "ON" : "OFF");
  } else if (body == "DEALER" || body == "DEALERPH") {
    uint8_t off = (body == "DEALER") ? REG3B06_DEALER_NAME : REG3B06_DEALER_PHONE;
    if (!dealer || dealer->size() < off + 20) { respond_nak_(prefix, ""); return; }
    std::string s;
    for (size_t i = 0; i < 20; i++) {
      char c = (char) (*dealer)[off + i];
      if (c == 0) break;
      s += c;
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    respond_(prefix, s);

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

// --- Bus workbench (SCAN? / REG? / REG!) ---

namespace {
bool hex_nibble_(char c, uint8_t &out) {
  if (c >= '0' && c <= '9') { out = c - '0'; return true; }
  if (c >= 'A' && c <= 'F') { out = c - 'A' + 10; return true; }
  return false;  // input is already uppercased
}
bool hex_byte_(const std::string &s, uint8_t &out) {
  if (s.size() != 2) return false;
  uint8_t hi, lo;
  if (!hex_nibble_(s[0], hi) || !hex_nibble_(s[1], lo)) return false;
  out = (hi << 4) | lo;
  return true;
}
bool hex_word_(const std::string &s, uint16_t &out) {
  if (s.size() != 4) return false;
  uint8_t hi, lo;
  if (!hex_byte_(s.substr(0, 2), hi) || !hex_byte_(s.substr(2, 2), lo)) return false;
  out = ((uint16_t) hi << 8) | lo;
  return true;
}
std::vector<std::string> split_tokens_(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ') {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
}  // namespace

std::string SamAsciiComponent::diag_hex_(const uint8_t *data, size_t len) {
  static const char *HEX = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out += HEX[data[i] >> 4];
    out += HEX[data[i] & 0x0F];
  }
  return out;
}

bool SamAsciiComponent::diag_parse_reg_args_(const std::vector<std::string> &tokens, size_t from,
                                             uint8_t &addr, uint16_t &reg, size_t &consumed) {
  // Accepted shapes (thermostat default when no address): [reg4],
  // [table2 row2], [addr2 reg4], [addr2 table2 row2].
  size_t n = tokens.size() - from;
  uint16_t v4 = 0;
  uint8_t v2a = 0, v2b = 0, v2c = 0;
  if (n == 1 && hex_word_(tokens[from], v4)) {
    addr = 0x20; reg = v4; consumed = 1; return true;
  }
  if (n == 2 && hex_byte_(tokens[from], v2a) && hex_byte_(tokens[from + 1], v2b)) {
    addr = 0x20; reg = ((uint16_t) v2a << 8) | v2b; consumed = 2; return true;
  }
  if (n == 2 && hex_byte_(tokens[from], v2a) && hex_word_(tokens[from + 1], v4)) {
    addr = v2a; reg = v4; consumed = 2; return true;
  }
  if (n == 3 && hex_byte_(tokens[from], v2a) && hex_byte_(tokens[from + 1], v2b) &&
      hex_byte_(tokens[from + 2], v2c)) {
    addr = v2a; reg = ((uint16_t) v2b << 8) | v2c; consumed = 3; return true;
  }
  return false;
}

void SamAsciiComponent::handle_workbench_(const std::string &body) {
  // SCAN form: SCAN | SCAN ALL | SCAN <addr2>
  if (body.rfind("SCAN", 0) == 0) {
    auto tokens = split_tokens_(body);
    if (tokens.empty() || tokens[0] != "SCAN") { respond_nak_("SCAN", "CMD"); return; }
    if (diag_read_pending_ || diag_write_pending_) { respond_nak_("SCAN", "BUSY"); return; }
    bool started = false;
    if (tokens.size() == 1 || (tokens.size() == 2 && tokens[1] == "ALL")) {
      started = parent_->diag_scan_start_all();
    } else if (tokens.size() == 2) {
      uint8_t addr = 0;
      if (hex_byte_(tokens[1], addr))
        started = parent_->diag_scan_start(addr);
    }
    if (!started) { respond_nak_("SCAN", "VAL"); return; }
    diag_scan_streaming_ = true;
    diag_scan_refused_.clear();
    diag_scan_timeouts_.clear();
    respond_("SCAN", "STARTED");
    return;
  }

  // REG forms. Read: REG [addr] reg | table row. Write (vendor bang
  // grammar, value after '!'): REG![addr] reg|table row <hexrow> - the
  // register arguments sit between REG and the final hex-row token, whether
  // the bang is attached ("REG!400A AABB") or the head carries the address
  // ("REG 20 400A!AABB").
  bool is_write = body.find('!') != std::string::npos;
  std::string flattened = body;
  std::replace(flattened.begin(), flattened.end(), '!', ' ');
  auto tokens = split_tokens_(flattened);
  if (tokens.empty() || tokens[0] != "REG") { respond_nak_("REG", "CMD"); return; }
  if (tokens.size() < 2) { respond_nak_("REG", "VAL"); return; }
  std::string hexrow_tok = is_write ? tokens.back() : "";
  size_t arg_end = is_write ? tokens.size() - 1 : tokens.size();  // exclusive
  uint8_t addr = 0;
  uint16_t reg = 0;
  size_t consumed = 0;
  {
    // Parse against just the argument span (diag_parse_reg_args_ reads to
    // the end of `tokens`, so hand it the span).
    std::vector<std::string> span(tokens.begin(), tokens.begin() + arg_end);
    if (!diag_parse_reg_args_(span, 1, addr, reg, consumed) || consumed != arg_end - 1) {
      respond_nak_("REG", "VAL");
      return;
    }
  }
  char rb[8], rh[8];
  snprintf(rb, sizeof(rb), "%02X", addr);
  snprintf(rh, sizeof(rh), "%04X", reg);

  if (!is_write) {
    // REG? read
    if (parent_->diag_reg_is_redacted(reg)) { respond_nak_("REG", "REDACTED"); return; }
    if (!parent_->diag_read_request(addr, reg)) { respond_nak_("REG", "BUSY"); return; }
    diag_read_pending_ = true;
    diag_read_reg_ = reg;
    diag_readback_ = false;
    respond_("REG", std::string("SENT ") + rb + " " + rh + " AS 93");
    return;
  }

  // REG! write: hexrow is the final token
  const std::string &hexrow = hexrow_tok;
  if (hexrow.empty() || hexrow.size() % 2 != 0 || hexrow.size() > 600) {
    respond_nak_("REG", "VAL");
    return;
  }
  std::vector<uint8_t> row(hexrow.size() / 2);
  for (size_t i = 0; i < row.size(); i++) {
    if (!hex_byte_(hexrow.substr(i * 2, 2), row[i])) { respond_nak_("REG", "VAL"); return; }
  }
  bool unknown_size = false;
  const char *refused = nullptr;
  if (!parent_->diag_write_request(addr, reg, row, unknown_size, &refused)) {
    respond_nak_("REG", refused ? refused : "VAL");
    if (refused && std::string(refused) == "LOCKED")
      respond_("REG", "ENABLE WITH sam_ascii raw_writes: true");
    return;
  }
  diag_write_pending_ = true;
  diag_write_reg_ = reg;
  diag_write_dest_ = addr;
  char lenb[8];
  snprintf(lenb, sizeof(lenb), "%uB", (unsigned) row.size());
  respond_("REG", std::string("WRITE ") + rb + " " + rh + " " + lenb +
                    (unknown_size ? " ROWSIZE UNKNOWN" : "") + " QUEUED");
}

void SamAsciiComponent::diag_render_scan_event_(const infinitesp::InfinitESPComponent::DiagScanEvent &ev) {
  using E = infinitesp::InfinitESPComponent::DiagScanEvent;
  // Sized for a HIT line with a large tabledef payload in DATA (147-byte
  // 4609 tabledef renders ~330 chars of hex; snprintf truncates safely
  // beyond that rather than overflowing).
  char line[768];
  switch (ev.type) {
    case E::DEVICE_START:
      diag_scan_refused_.clear();
      diag_scan_timeouts_.clear();
      snprintf(line, sizeof(line), "SCAN %02X BEGIN\r\n", ev.addr);
      break;
    case E::TABLE_RESULT: {
      char t[4];
      snprintf(t, sizeof(t), "%02X", ev.table);
      if (ev.status == E::HIT) {
        // Tabledef layout (PROTOCOL, discovery parser): name at [2..9],
        // alloc BE16 at [10..11], row count at [12]. Non-printable names
        // render as raw hex (the table-3E live-data-at-row-01 case).
        const auto &d = ev.data;
        std::string name;
        bool printable = d.size() >= 10;
        if (printable) {
          name.assign(d.begin() + 2, d.begin() + 10);
          while (!name.empty() && (name.back() == 0 || name.back() == ' ')) name.pop_back();
          for (char c : name)
            if ((uint8_t) c < 0x20 || (uint8_t) c > 0x7E) { printable = false; break; }
        }
        unsigned alloc = (d.size() >= 12) ? (unsigned) ((d[10] << 8) | d[11]) : 0;
        unsigned rows = (d.size() >= 13) ? (unsigned) d[12] : 0;
        if (printable && !name.empty()) {
          snprintf(line, sizeof(line), "SCAN %02X %02X HIT NAME=%s ALLOC=%u ROWS=%u LEN=%u DATA=%s\r\n",
                   ev.addr, ev.table, name.c_str(), alloc, rows, (unsigned) d.size(),
                   diag_hex_(d.data(), d.size()).c_str());
        } else {
          snprintf(line, sizeof(line), "SCAN %02X %02X HIT RAW LEN=%u DATA=%s\r\n",
                   ev.addr, ev.table, (unsigned) d.size(), diag_hex_(d.data(), d.size()).c_str());
        }
      } else {
        // Refusals and timeouts aggregate into per-device table lists
        // rendered at DEVICE_DONE (one line per probed table would bury the
        // hits; the lists are the negative inventory).
        std::string &agg = (ev.status == E::REFUSED) ? diag_scan_refused_ : diag_scan_timeouts_;
        if (!agg.empty()) agg += ",";
        agg += t;
        return;
      }
      break;
    }
    case E::DEVICE_DONE: {
      std::string out;
      char head[16];
      snprintf(head, sizeof(head), "SCAN %02X ", ev.addr);
      if (!diag_scan_refused_.empty())
        out += head + std::string("REFUSED_TABLES=") + diag_scan_refused_ + "\r\n";
      if (!diag_scan_timeouts_.empty())
        out += head + std::string("TIMEOUT_TABLES=") + diag_scan_timeouts_ + "\r\n";
      snprintf(line, sizeof(line), "SCAN %02X DONE HITS=%u REFUSED=%u TIMEOUTS=%u%s\r\n",
               ev.addr, ev.hits, ev.refused, ev.timeouts, ev.absent ? " ABSENT" : "");
      out += line;
      write_str(out.c_str());
      flush();
      return;
    }
    case E::SCAN_DONE:
      diag_scan_streaming_ = false;
      snprintf(line, sizeof(line), "SCAN DONE DEVICES=%u%s\r\n", ev.devices,
               ev.aborted ? " ABORTED" : "");
      break;
  }
  write_str(line);
  flush();
}

void SamAsciiComponent::diag_drain_() {
  using infinitesp::InfinitESPComponent;
  InfinitESPComponent::DiagScanEvent ev;
  while (parent_->diag_scan_pop(ev))
    diag_render_scan_event_(ev);

  if (diag_write_pending_ && parent_->diag_write_ready()) {
    auto r = parent_->diag_write_result();
    diag_write_pending_ = false;
    char line[64];
    snprintf(line, sizeof(line), "REG %04X: %s (%u ATTEMPTS)\r\n", diag_write_reg_,
             r.acked ? "ACK" : "NOACK", r.attempts);
    write_str(line);
    flush();
    // READBACK: re-read the register so an ACK-and-ignore control shows its
    // unchanged value inline (the #61 lesson: ACK means nothing, adoption
    // is what matters).
    if (parent_->diag_read_request(diag_write_dest_, diag_write_reg_)) {
      diag_read_pending_ = true;
      diag_read_reg_ = diag_write_reg_;
      diag_readback_ = true;
    }
  }

  if (diag_read_pending_ && parent_->diag_read_ready()) {
    auto r = parent_->diag_read_result();
    diag_read_pending_ = false;
    char rh[8];
    snprintf(rh, sizeof(rh), "%04X", diag_read_reg_);
    std::string line;
    if (r.refused)
      line = "REG " + std::string(rh) + ": REFUSED " + std::to_string(r.code);
    else if (!r.ok)
      line = "REG " + std::string(rh) + ": TIMEOUT";
    else
      line = "REG " + std::string(rh) + ": " + (diag_readback_ ? "READBACK " : "") +
             diag_hex_(r.data.data(), r.data.size());
    line += "\r\n";
    write_str(line.c_str());
    flush();
    diag_readback_ = false;
  }
}

}  // namespace sam_ascii
}  // namespace esphome
