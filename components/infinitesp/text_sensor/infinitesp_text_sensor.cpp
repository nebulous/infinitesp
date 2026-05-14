#include "infinitesp_text_sensor.h"

namespace esphome {
namespace infinitesp {

void InfinitESPTextSensor::on_register_update(uint8_t device_addr, uint16_t register_key) {
  // Zone name from SAM 3B03 register
  if (sensor_type_ == "zone_name") {
    if (register_key != REG_SAM_ZONES)
      return;

    auto *data = parent_->get_register(parent_->get_address(), REG_SAM_ZONES);
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

  // Comfort profile summary from 400A
  if (sensor_type_ == "comfort_profile") {
    if (register_key != REG_TSTAT_COMFORT)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_COMFORT);
    if (!data || data->size() < COMFORT_ACTIVITY_COUNT * COMFORT_ENTRY_SIZE)
      return;

    const char *names[] = {"home", "away", "sleep", "wake", "manual"};
    const char *fan_names[] = {"off", "low", "med", "high"};
    std::string result;
    for (uint8_t i = 0; i < COMFORT_ACTIVITY_COUNT; i++) {
      uint8_t base = i * COMFORT_ENTRY_SIZE;
      if (i > 0)
        result += "; ";
      char buf[64];
      snprintf(buf, sizeof(buf), "%s: ht=%d cl=%d fan=%s",
               names[i],
               (*data)[base + 0],
               (*data)[base + 1],
               (*data)[base + 2] < 4 ? fan_names[(*data)[base + 2]] : "?");
      result += buf;
    }
    publish_state(result);
    return;
  }

  // Fault history from 4202
  // 10 entries × 7 bytes: code(1), source(1), hour(1), minute(1), days_be16(2), status(1)
  // Days since 2013-01-01 epoch. Status bit 7 = active (0=active, 1=cleared), bits 0-6 = occurrence count.
  if (sensor_type_ == "fault_history") {
    if (register_key != REG_TSTAT_FAULTS)
      return;
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_FAULTS);
    if (!data || data->size() < 70)
      return;

    const char *source_names[] = {"?", "?", "UI", "?", "?", "?", "?", "?",
                                  "?", "?", "?", "?", "?", "?", "?", "?",
                                  "?", "?", "?", "?", "?", "?", "?", "?",
                                  "?", "?", "?", "?", "?", "?", "?", "?",
                                  "IDU", "?", "?", "?", "?", "?", "?", "?",
                                  "?", "?", "?", "?", "?", "?", "?", "?",
                                  "?", "?", "ODU"};
    std::string result;
    for (int i = 0; i < 10; i++) {
      uint8_t base = i * 7;
      uint8_t code = (*data)[base + 0];
      uint8_t source = (*data)[base + 1];
      uint8_t hour = (*data)[base + 2];
      uint8_t minute = (*data)[base + 3];
      uint16_t days = ((uint16_t) (*data)[base + 4] << 8) | (*data)[base + 5];
      uint8_t status = (*data)[base + 6];
      bool active = !(status & 0x80);  // bit 7: 0=active, 1=cleared
      uint8_t occurrences = status & 0x7F;

      // Skip empty entries (all zeros)
      if (code == 0 && source == 0 && days == 0)
        continue;

      if (!result.empty())
        result += "\n";

      // Convert days since 2013-01-01 to a date string
      // 2013-01-01 epoch, account for leap years
      uint32_t total_days = days;
      int year = 2013;
      while (total_days >= 365) {
        uint16_t year_days = ((year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365);
        if (total_days >= year_days) {
          total_days -= year_days;
          year++;
        } else {
          break;
        }
      }
      static const uint8_t mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
      uint8_t month = 0;
      while (month < 12) {
        uint8_t dim = mdays[month];
        if (month == 1 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)))
          dim = 29;
        if (total_days >= dim) {
          total_days -= dim;
          month++;
        } else {
          break;
        }
      }
      uint8_t day = (uint8_t) total_days + 1;
      month++;  // 1-indexed

      const char *src_name = (source < sizeof(source_names) / sizeof(source_names[0]))
                                 ? source_names[source]
                                 : "?";
      char buf[80];
      snprintf(buf, sizeof(buf), "%s code=%02d src=%s %02d:%02d %04d-%02d-%02d occ=%d%s",
               active ? "ACT" : "CLR", code, src_name, hour, minute, year, month, day,
               occurrences, (i == 0 && active) ? " (latest)" : "");
      result += buf;
    }

    if (result.empty())
      result = "No faults";

    publish_state(result);
    return;
  }
}

} // namespace infinitesp
} // namespace esphome
