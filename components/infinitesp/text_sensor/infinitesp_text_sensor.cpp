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
}

} // namespace infinitesp
} // namespace esphome
