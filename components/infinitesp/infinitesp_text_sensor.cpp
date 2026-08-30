#include "infinitesp_text_sensor.h"
#include <cctype>

namespace esphome {
namespace infinitesp {

// Fault source byte is the device bus address; the high nibble is the device
// class (0x2x thermostat/UI, 0x4x IDU/furnace, 0x5x ODU). Matches Infinitude
// (0x20=UI, 0x40=furnace, 0x52=AC) and the device-class convention, robust to
// other instances (0x21 stat, 0x53 ODU). Confirmed against live 4202 data
// where every entry is src=0x20 (thermostat).
static const char *fault_source_name(uint8_t source) {
  switch (source >> 4) {
    case CLASS_THERMOSTAT:   return "UI";   // thermostat
    case CLASS_INDOOR_UNIT:  return "IDU";  // indoor unit / furnace
    case CLASS_OUTDOOR_UNIT: return "ODU";  // outdoor unit
    default:  return "?";
  }
}

void InfinitESPTextSensor::on_register_update(uint8_t device_addr, uint16_t register_key) {
  // Hold state display: "until HH:MM PM", "Permanent", or "Schedule"
  if (sensor_type_ == "hold_state") {
    if (register_key != REG_SAM_ZONES)
      return;
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_ZONES);
    if (!data || data->size() < REG3B03_HOLD_DURATIONS + zone_ * 2)
      return;

    uint8_t idx = zone_ - 1;
    if (!(data->at(REG3B03_ACTIVE_ZONES) & (1 << idx)))
      return;

    uint16_t hold_dur = parent_->get_zone_hold_duration(zone_);

    if (hold_dur == 0) {
      publish_state("Schedule");
    } else if (hold_dur >= InfinitESPComponent::HOLD_PERMANENT) {
      publish_state("Hold - Permanent");
    } else {
      std::string end = parent_->format_hold_end(hold_dur);
      if (!end.empty())
        publish_state("Hold until " + end);
      else
        publish_state("Hold " + std::to_string(hold_dur) + " min");
    }
    return;
  }

  // Zone name from SAM 3B03 register
  if (sensor_type_ == "zone_name") {
    if (register_key != REG_SAM_ZONES)
      return;

    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_ZONES);
    if (!data || data->size() < REG3B03_SIZE)
      return;

    uint8_t idx = zone_ - 1;
    if (!(data->at(REG3B03_ACTIVE_ZONES) & (1 << idx)))
      return;

    uint16_t name_offset = REG3B03_ZONE_NAMES + (idx * 12);
    std::string name;
    for (int i = 0; i < 12; i++) {
      char c = (char) data->at(name_offset + i);
      if (c == 0)
        break;
      name += c;
    }
    // Trim trailing spaces
    while (!name.empty() && name.back() == ' ') {
      name.pop_back();
    }

    if (!name.empty()) {
      publish_state(name);
    }
    return;
  }

  // Thermostat WiFi SSID from 4608
  if (sensor_type_ == "tstat_ssid") {
    if (register_key != REG_TSTAT_WIFI)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_WIFI);
    if (!data || data->size() < 25)
      return;
    publish_state(extract_cstr(*data, 24));
    return;
  }

  // Thermostat WiFi hostname from 4608
  if (sensor_type_ == "tstat_hostname") {
    if (register_key != REG_TSTAT_WIFI)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_WIFI);
    if (!data || data->size() < 140)
      return;
    publish_state(extract_cstr(*data, 139));
    return;
  }

  // Thermostat WiFi MAC address from 4608
  if (sensor_type_ == "tstat_wifi_mac") {
    if (register_key != REG_TSTAT_WIFI)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_WIFI);
    if (!data || data->size() < 5)
      return;
    publish_state(extract_cstr(*data, 4));
    return;
  }

  // Thermostat cloud host from 4609
  if (sensor_type_ == "tstat_cloud_host") {
    if (register_key != REG_TSTAT_CLOUD)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_CLOUD);
    if (!data || data->empty())
      return;
    publish_state(extract_cstr(*data, 0));
    return;
  }

  // Thermostat proxy server IP from 4609
  if (sensor_type_ == "tstat_proxy_server") {
    if (register_key != REG_TSTAT_CLOUD)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_CLOUD);
    if (!data || data->size() < 68)
      return;
    publish_state(extract_cstr(*data, 67));
    return;
  }

  // Dealer name from 460A
  if (sensor_type_ == "tstat_dealer_name") {
    if (register_key != REG_TSTAT_DEALER)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_DEALER);
    if (!data || data->empty())
      return;
    publish_state(extract_cstr(*data, 0));
    return;
  }

  // Dealer brand from 460A
  if (sensor_type_ == "tstat_dealer_brand") {
    if (register_key != REG_TSTAT_DEALER)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_DEALER);
    if (!data || data->size() < 51)
      return;
    publish_state(extract_cstr(*data, 50));
    return;
  }

  // Dealer URL from 460A
  if (sensor_type_ == "tstat_dealer_url") {
    if (register_key != REG_TSTAT_DEALER)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_DEALER);
    if (!data || data->size() < 71)
      return;
    publish_state(extract_cstr(*data, 70));
    return;
  }

  // Comfort profile summary from this zone's table-40 row (400A+zone-1).
  // Zone 1 reads 400A; a `zone: N` sensor reads zone N's row.
  if (sensor_type_ == "comfort_profile") {
    uint16_t comfort_reg = comfort_reg_for_zone(zone_);
    if (register_key != comfort_reg)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, comfort_reg);
    if (!data || data->size() < COMFORT_ACTIVITY_COUNT * COMFORT_ENTRY_SIZE)
      return;

    const char *names[] = {"home", "away", "sleep", "wake", "manual"};
    const char *fan_names[] = {"off", "low", "med", "high"};
    // Show temperatures in both °C and °F for universal readability
    // (HA can't auto-convert text sensor strings)
    std::string result;
    for (uint8_t i = 0; i < COMFORT_ACTIVITY_COUNT; i++) {
      uint8_t base = i * COMFORT_ENTRY_SIZE;
      float ht_c = parent_->comfort_byte_to_celsius((*data)[base + 0]);
      float cl_c = parent_->comfort_byte_to_celsius((*data)[base + 1]);
      float ht_f = ht_c * 9.0f / 5.0f + 32.0f;
      float cl_f = cl_c * 9.0f / 5.0f + 32.0f;
      if (i > 0)
        result += "; ";
      char buf[80];
      snprintf(buf, sizeof(buf), "%s: ht=%.0f\xc2\xb0" "F/%.1f\xc2\xb0" "C cl=%.0f\xc2\xb0" "F/%.1f\xc2\xb0" "C fan=%s",
               names[i],
               ht_f, ht_c, cl_f, cl_c,
               (*data)[base + 2] < 4 ? fan_names[(*data)[base + 2]] : "?");
      result += buf;
    }
    publish_state(result);
    return;
  }

  // Fault history from 4202 (layout and semantics verified live 2026-08-24,
  // issue #22; see the FAULT_* block in infinitesp.h):
  // 10 entries x 7 bytes (newest first) + 2-byte install-relative day trailer.
  // status low 7 bits = occurrence count; bit 7 observed set at logging and
  // clearing within ~2 min for one banner-class fault, persisting on silent
  // diagnostics — NOT liveness, NOT acknowledgment, semantics under-characterized
  // across installs. No fault liveness exists on the bus. Dates are rendered
  // RELATIVE (trailer - entry days) because the day counter is per-install
  // (Infinitude's fixed 2013-01-01 epoch is disproven). Non-thermostat sources
  // may pack garbage into the time fields.
  if (sensor_type_ == "fault_history") {
    if (register_key != REG_TSTAT_FAULTS)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_FAULTS);
    if (!data || data->size() < FAULT_REG_SIZE)
      return;
    auto *state = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    bool have_clock = state && state->size() >= REG3B02_MINUTES + 2;
    uint16_t now_bus_min = 0;
    if (have_clock)
      now_bus_min = ((uint16_t) state->at(REG3B02_MINUTES) << 8) |
                    state->at(REG3B02_MINUTES + 1);

    // Fault codes are decimal numbers. Human-readable descriptions require a
    // verified Carrier fault-code reference, which is not available; add when found.
    // RENDER BUDGET: HA rejects states longer than 255 chars ("falling back to
    // unknown" — the pre-2026.8.6 render exceeded it, so fault_history was
    // always unknown in HA; issue #22's original report). Compact format:
    // code[(xN)][ SRC] [today|yesterday|Nd] HH:MM, entries joined by "; ",
    // newest first, truncated at the budget with a "+N more" tail.
    std::string result;
    uint8_t shown = 0, remaining = 0;
    for (uint8_t i = 0; i < FAULT_ENTRY_COUNT; i++) {
      uint8_t base = i * FAULT_ENTRY_SIZE;
      uint8_t code = (*data)[base + FAULT_CODE];
      uint8_t source = (*data)[base + FAULT_SOURCE];
      uint16_t days = ((uint16_t) (*data)[base + FAULT_DAYS_HI] << 8) |
                      (*data)[base + FAULT_DAYS_LO];
      uint8_t status = (*data)[base + FAULT_STATUS];
      // Bit 7 is NOT rendered: our single-install observations of it (set at
      // logging, clears within ~2 min for one banner-class fault, persists on
      // silent diagnostics) are not characterized well enough across installs
      // to present as user-facing semantics. Decoded if needed for research.
      bool bit7 = (status & 0x80) != 0;
      (void) bit7;
      uint8_t occurrences = status & 0x7F;

      // Skip empty entries (all zeros)
      if (code == 0 && source == 0 && days == 0)
        continue;

      char buf[56];
      // Render: "code[(xN)][ SRC] [today|yesterday|Nd] HH:MM".
      // Human-readable words (today/yesterday), UI source omitted (the
      // common case), occurrences bound to the code. State changes only when
      // the register changes — day granularity, no poll-cadence churn.
      char occ_suffix[6] = "";
      if (occurrences > 1)
        snprintf(occ_suffix, sizeof(occ_suffix), "(x%u)", occurrences);
      const char *src_name = fault_source_name(source);
      bool is_ui = (source >> 4) == CLASS_THERMOSTAT;
      if (have_clock && fault_entry_time_valid(*data, i)) {
        uint16_t trailer = ((uint16_t) (*data)[70] << 8) | (*data)[71];
        uint16_t dd = trailer - days;  // 0 = today, 1 = yesterday, ...
        const char *day;
        char day_buf[6];
        if (dd == 0)
          day = "today";
        else if (dd == 1)
          day = "yesterday";
        else {
          snprintf(day_buf, sizeof(day_buf), "%ud", dd);
          day = day_buf;
        }
        // Source label: omitted for UI (the common case), " ODU"/" IDU" etc otherwise.
        char src_prefix[8] = "";
        if (!is_ui)
          snprintf(src_prefix, sizeof(src_prefix), " %s", src_name);
        snprintf(buf, sizeof(buf), "%u%s%s %s %02d:%02d",
                 code, occ_suffix, src_prefix, day,
                 (*data)[base + FAULT_HOUR], (*data)[base + FAULT_MINUTE]);
      } else {
        // Invalid/garbage time fields (non-thermostat source packing): keep
        // the code visible without rendering a wrong date.
        snprintf(buf, sizeof(buf), "%u%s src 0x%02X ?",
                 code, occ_suffix, source);
      }
      size_t need = strlen(buf) + (result.empty() ? 0 : 2) + (shown + 1 < 10 ? 8 : 0);
      if (result.size() + need > 250) {
        remaining++;
        continue;
      }
      if (!result.empty())
        result += "; ";
      result += buf;
      shown++;
    }
    if (remaining > 0) {
      result += "; +" + std::to_string(remaining) + " more";
    }

    if (result.empty())
      result = "No faults";

    publish_state(result);
    return;
  }

  // Manufacture date derived from 0104 serial number
  // Carrier serial format: first 2 digits = week (01-52), next 2 digits = year (00-99)
  // Device matching: a manual device_address pins one exact bus node;
  // device_class matching rides the base-class bus_class dispatch gate
  // (notify_entities_ never delivers a mismatched class here). Bare
  // manufacture_date — no address, no bus_class — defaults to the thermostat
  // class (2): the entity surface is one sensor per device class, and
  // accepting any class made the bare sensor publish whichever device's
  // serial arrived last. Class, not node, because a class sits at different
  // low-nibble values across installs (ODU: 0x50 or 0x52).
  if (sensor_type_ == "manufacture_date") {
    // Two disjoint identity sources, same duck-typed pattern as the ODU sensors:
    // register 0x0104 (offset 96) on families whose thermostat polls device
    // info, or register 3E09 (offset 0) on the 2-stage/two-capacity family
    // whose thermostat never polls the ODU's 0104 (issue #21). Both carry the
    // Carrier WWYY serial prefix. Only one family's register ever arrives, so
    // there is no precedence conflict.
    if (register_key != REG_DEVICE_INFO && register_key != REG_ODU_3E_SERIAL)
      return;
    if (target_device_addr_ != 0) {
      if (device_addr != target_device_addr_)
        return;
    } else if (get_bus_class() == 0 && (device_addr >> 4) != 2) {
      return;
    }
    const bool from_3e = (register_key == REG_ODU_3E_SERIAL);
    auto *data = parent_->get_register(device_addr, register_key);
    if (!data)
      return;
    if (!from_3e && data->size() < 100)
      return;  // 0104 serial lives at offset 96
    if (from_3e && data->size() < 4)
      return;

    // Serial starts at offset 96 (0104) or offset 0 (3E09); extract first 4 digits
    const uint8_t *serial = data->data() + (from_3e ? 0 : 96);
    if (!std::isdigit(serial[0]) || !std::isdigit(serial[1]) ||
        !std::isdigit(serial[2]) || !std::isdigit(serial[3]))
      return;

    uint8_t week = (serial[0] - '0') * 10 + (serial[1] - '0');
    uint8_t year_short = (serial[2] - '0') * 10 + (serial[3] - '0');
    if (week < 1 || week > 52)
      return;

    // Carrier used 2-digit years. 00-39 → 2000-2039, 40-99 → 1940-1999
    uint16_t year = (year_short < 40) ? (2000 + year_short) : (1900 + year_short);

    // Week → approximate month (midpoint of week)
    static const uint16_t month_cumulative[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    uint16_t day_of_year = (week - 1) * 7 + 3;  // midpoint of the week
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    if (leap && day_of_year > 59) day_of_year++;  // shift past Feb 29
    const char *month_names[] = {"January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December"};
    uint8_t month = 0;
    for (uint8_t m = 1; m < 12; m++) {
      if (day_of_year < month_cumulative[m])
        break;
      month = m;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%s %04u", month_names[month], year);
    publish_state(buf);
    return;
  }
}

} // namespace infinitesp
} // namespace esphome
