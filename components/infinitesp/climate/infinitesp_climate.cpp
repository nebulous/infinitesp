#include "infinitesp_climate.h"

namespace esphome {
namespace infinitesp {

static const uint16_t HOLD_PERMANENT = 65535;  // max uint16 = permanent hold

climate::ClimateTraits InfinitESPClimate::traits() {
  auto traits = climate::ClimateTraits();
  using namespace esphome::climate;
  traits.add_feature_flags(CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           CLIMATE_SUPPORTS_ACTION |
                           CLIMATE_REQUIRES_TWO_POINT_TARGET_TEMPERATURE);

  // The ABCD bus works in whole °F. HA treats all ESPHome climate as °C internally.
  // min/max are in °C (HA converts to °F for display via show_temp()).
  //
  // The step is special: HA's ESPHome integration does round(step, 1) on the raw
  // °C value, and the climate entity does NOT convert it to the display unit.
  // So 5/9 (0.5556) becomes 0.6 in the HA UI, not 1.0°F.
  // Fix: set step = 1.0. HA shows 1.0 as the step. When the user clicks +/-, HA
  // adds 1.0 to the °F display value, converts to °C, and sends it to us. Our
  // control() handler rounds to nearest °F regardless.
  static const float STEP_C = 5.0f / 9.0f;  // exactly 1°F
  traits.set_visual_min_temperature((40.0f - 32.0f) * STEP_C);   // 40°F = 4.444°C
  traits.set_visual_max_temperature((99.0f - 32.0f) * STEP_C);   // 99°F = 37.222°C
  traits.set_visual_temperature_step(1.0f);

  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT);
  traits.add_supported_mode(climate::CLIMATE_MODE_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_HEAT_COOL);
  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);

  traits.add_supported_fan_mode(climate::CLIMATE_FAN_AUTO);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_LOW);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_MEDIUM);
  traits.add_supported_fan_mode(climate::CLIMATE_FAN_HIGH);

  // Standard presets map to comfort profile activities from thermostat register 400A.
  // NONE = cancel hold (resume schedule).
  // HOME/AWAY/SLEEP/WAKE = apply that activity's setpoints+fan from 400A with a permanent hold.
  // The thermostat stores 5 activities: home, away, sleep, wake, manual.
  traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
  traits.add_supported_preset(climate::CLIMATE_PRESET_HOME);
  traits.add_supported_preset(climate::CLIMATE_PRESET_AWAY);
  traits.add_supported_preset(climate::CLIMATE_PRESET_SLEEP);

  // Custom presets: WAKE activity + timed holds using current comfort profile's setpoints
  const char *const custom_presets[] = {
      PRESET_WAKE,    // Comfort activity: wake
      PRESET_HOLD_1H, // 60 min
      PRESET_HOLD_2H, // 120 min
      PRESET_HOLD_4H, // 240 min
      PRESET_HOLD_8H, // 480 min
  };
  traits.set_supported_custom_presets(custom_presets);

  return traits;
}

void InfinitESPClimate::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    this->mode = call.get_mode().value();
    auto mode = call.get_mode().value();
    uint8_t sys = SYSMODE_OFF;
    switch (mode) {
      case climate::CLIMATE_MODE_HEAT:      sys = SYSMODE_HEAT; break;
      case climate::CLIMATE_MODE_COOL:      sys = SYSMODE_COOL; break;
      case climate::CLIMATE_MODE_HEAT_COOL: sys = SYSMODE_AUTO; break;
      case climate::CLIMATE_MODE_OFF:       sys = SYSMODE_OFF; break;
      default: break;
    }
    parent_->set_system_mode(sys);
    sys_mode_ = sys;
  }

  if (call.get_target_temperature_low().has_value()) {
    float target_c = call.get_target_temperature_low().value();
    uint8_t target_f = (uint8_t) roundf(target_c * (9.0f / 5.0f) + 32.0f);
    heat_sp_ = target_f;
    parent_->set_zone_setpoint(zone_, heat_sp_, cool_sp_);
    this->target_temperature_low = target_c;
  }

  if (call.get_target_temperature_high().has_value()) {
    float target_c = call.get_target_temperature_high().value();
    uint8_t target_f = (uint8_t) roundf(target_c * (9.0f / 5.0f) + 32.0f);
    cool_sp_ = target_f;
    parent_->set_zone_setpoint(zone_, heat_sp_, cool_sp_);
    this->target_temperature_high = target_c;
  }

  if (call.get_fan_mode().has_value()) {
    this->fan_mode = call.get_fan_mode().value();
    auto fan = call.get_fan_mode().value();
    uint8_t fm = FAN_AUTO;
    switch (fan) {
      case climate::CLIMATE_FAN_AUTO:   fm = FAN_AUTO; break;
      case climate::CLIMATE_FAN_LOW:    fm = FAN_LOW; break;
      case climate::CLIMATE_FAN_MEDIUM: fm = FAN_MED; break;
      case climate::CLIMATE_FAN_HIGH:   fm = FAN_HIGH; break;
      default: break;
    }
    parent_->set_zone_fan(zone_, fm);
    fan_mode_ = fm;
  }

  // Handle standard presets — activity-based holds using comfort profiles from 400A
  if (call.get_preset().has_value()) {
    auto preset = call.get_preset().value();
    switch (preset) {
      case climate::CLIMATE_PRESET_NONE:
        // Cancel hold — resume schedule
        parent_->set_zone_hold(zone_, 0);
        this->set_preset_(preset);
        hold_duration_ = 0;
        last_activity_ = NO_ACTIVITY;
        break;
      case climate::CLIMATE_PRESET_HOME:
        parent_->apply_activity(zone_, COMFORT_HOME, HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = HOLD_PERMANENT;
        last_activity_ = COMFORT_HOME;
        break;
      case climate::CLIMATE_PRESET_AWAY:
        parent_->apply_activity(zone_, COMFORT_AWAY, HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = HOLD_PERMANENT;
        last_activity_ = COMFORT_AWAY;
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        parent_->apply_activity(zone_, COMFORT_SLEEP, HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = HOLD_PERMANENT;
        last_activity_ = COMFORT_SLEEP;
        break;
      default:
        break;
    }
  }

  // Handle custom presets (WAKE activity + timed holds)
  if (call.has_custom_preset()) {
    auto custom = call.get_custom_preset();
    if (custom == PRESET_WAKE) {
      parent_->apply_activity(zone_, COMFORT_WAKE, HOLD_PERMANENT);
      hold_duration_ = HOLD_PERMANENT;
      last_activity_ = COMFORT_WAKE;
      this->set_custom_preset_(PRESET_WAKE);
      ESP_LOGI("InfinitESP", "Zone %d: preset WAKE → permanent hold", zone_);
    } else {
      uint16_t duration = 0;
      if (custom == PRESET_HOLD_1H)
        duration = 60;
      else if (custom == PRESET_HOLD_2H)
        duration = 120;
      else if (custom == PRESET_HOLD_4H)
        duration = 240;
      else if (custom == PRESET_HOLD_8H)
        duration = 480;

      if (duration > 0) {
        parent_->set_zone_hold(zone_, duration);
        hold_duration_ = duration;
        last_activity_ = NO_ACTIVITY;
        this->set_custom_preset_(custom);
        ESP_LOGI("InfinitESP", "Zone %d: custom preset '%s' → hold for %d min",
                 zone_, custom.c_str(), duration);
      }
    }
  }

  publish_state();
}

void InfinitESPClimate::on_register_update(uint8_t device_addr, uint16_t register_key) {
  bool changed = false;

  if (register_key == REG_SAM_STATE) {
    auto *data = parent_->get_register(parent_->get_address(), REG_SAM_STATE);
    if (data && data->size() >= REG3B02_STAGMODE + 1) {
      uint8_t idx = zone_ - 1;
      uint8_t active = data->at(REG3B02_ACTIVE_ZONES);
      if (!(active & (1 << idx)))
        return;

      float temp_f = (float)data->at(REG3B02_TEMPS + idx);
      float temp = (temp_f - 32.0f) * (5.0f / 9.0f);
      if (temp != current_temp_) {
        current_temp_ = temp;
        this->current_temperature = temp;
        changed = true;
      }

      uint8_t stagmode = data->at(REG3B02_STAGMODE);
      uint8_t mode = stagmode & 0x0F;
      uint8_t stage = (stagmode >> 4) & 0x0F;

      climate::ClimateAction action = climate::CLIMATE_ACTION_IDLE;
      if (stage > 0) {
        switch (mode) {
          case SYSMODE_HEAT: action = climate::CLIMATE_ACTION_HEATING; break;
          case SYSMODE_COOL: action = climate::CLIMATE_ACTION_COOLING; break;
          case SYSMODE_EHEAT: action = climate::CLIMATE_ACTION_HEATING; break;
          default: break;
        }
      }
      if (action != current_action_) {
        current_action_ = action;
        this->action = action;
        changed = true;
      }

      if (mode != sys_mode_) {
        sys_mode_ = mode;
        switch (mode) {
          case SYSMODE_HEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
          case SYSMODE_COOL: this->mode = climate::CLIMATE_MODE_COOL; break;
          case SYSMODE_AUTO: this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
          case SYSMODE_EHEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
          case SYSMODE_OFF:
          default: this->mode = climate::CLIMATE_MODE_OFF; break;
        }
        this->target_temperature_low = ((float)heat_sp_ - 32.0f) * (5.0f / 9.0f);
        this->target_temperature_high = ((float)cool_sp_ - 32.0f) * (5.0f / 9.0f);
        changed = true;
      }
    }
  }

  if (register_key == REG_SAM_ZONES) {
    auto *data = parent_->get_register(parent_->get_address(), REG_SAM_ZONES);
    if (data && data->size() >= REG3B03_COOL_SETPOINTS + 8) {
      uint8_t idx = zone_ - 1;
      uint8_t active = data->at(REG3B03_ACTIVE_ZONES);
      if (!(active & (1 << idx)))
        return;

      uint8_t new_heat = data->at(REG3B03_HEAT_SETPOINTS + idx);
      uint8_t new_cool = data->at(REG3B03_COOL_SETPOINTS + idx);
      uint8_t new_fan = data->at(REG3B03_FAN_MODES + idx);

      bool sp_changed = false;
      if (new_heat != heat_sp_) {
        heat_sp_ = new_heat;
        sp_changed = true;
      }
      if (new_cool != cool_sp_) {
        cool_sp_ = new_cool;
        sp_changed = true;
      }
      if (sp_changed) {
        this->target_temperature_low = ((float)new_heat - 32.0f) * (5.0f / 9.0f);
        this->target_temperature_high = ((float)new_cool - 32.0f) * (5.0f / 9.0f);
        changed = true;
      }
      if (new_fan != fan_mode_) {
        fan_mode_ = new_fan;
        switch (new_fan) {
          case FAN_AUTO: this->fan_mode = climate::CLIMATE_FAN_AUTO; break;
          case FAN_LOW:  this->fan_mode = climate::CLIMATE_FAN_LOW; break;
          case FAN_MED:  this->fan_mode = climate::CLIMATE_FAN_MEDIUM; break;
          case FAN_HIGH: this->fan_mode = climate::CLIMATE_FAN_HIGH; break;
        }
        changed = true;
      }

      // Infer current preset from setpoints+fan matching against comfort profiles.
      // Always run — not gated on hold_duration change — so preset reflects current
      // schedule activity even when no hold is active.
      bool hold_changed = false;
      if (data->size() >= REG3B03_HOLD_DURATIONS + idx * 2 + 2) {
        uint16_t hold_dur = (data->at(REG3B03_HOLD_DURATIONS + idx * 2) << 8) |
                            data->at(REG3B03_HOLD_DURATIONS + idx * 2 + 1);
        ESP_LOGD("InfinitESP", "Zone %d hold: zones_holding=0x%02X duration=%d",
                 zone_, data->at(REG3B03_ZONES_HOLDING), hold_dur);
        if (hold_dur != hold_duration_) {
          hold_duration_ = hold_dur;
          hold_changed = true;
        }
      }

      // Match current setpoints+fan against comfort profiles from register 400A.
      // Track whether the inferred preset actually changed to avoid spurious publishes.
      auto old_preset = this->preset;

      auto *comfort = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_COMFORT);
      ESP_LOGD("InfinitESP", "Zone %d preset match: heat=%d cool=%d fan=%d comfort=%p size=%d",
               zone_, new_heat, new_cool, new_fan,
               comfort ? (void*)comfort : nullptr,
               comfort ? (int)comfort->size() : -1);
      bool matched = false;
      if (comfort && comfort->size() >= COMFORT_ACTIVITY_COUNT * COMFORT_ENTRY_SIZE) {
        for (uint8_t a = 0; a < COMFORT_ACTIVITY_COUNT; a++) {
          uint8_t base = a * COMFORT_ENTRY_SIZE;
          if ((*comfort)[base + 0] == new_heat && (*comfort)[base + 1] == new_cool &&
              (*comfort)[base + 2] == new_fan) {
            last_activity_ = a;
            switch (a) {
              case COMFORT_HOME:  this->set_preset_(climate::CLIMATE_PRESET_HOME); break;
              case COMFORT_AWAY:  this->set_preset_(climate::CLIMATE_PRESET_AWAY); break;
              case COMFORT_SLEEP: this->set_preset_(climate::CLIMATE_PRESET_SLEEP); break;
              case COMFORT_WAKE:  this->set_custom_preset_(PRESET_WAKE); break;
              default:
                this->preset = climate::CLIMATE_PRESET_HOME;
                break;
            }
            matched = true;
            break;
          }
        }
      }
      if (!matched) {
        // Setpoints don't match any known activity.
        // If no hold, show NONE (schedule running with unknown activity).
        // If hold active, show HOME as fallback.
        last_activity_ = NO_ACTIVITY;
        if (hold_duration_ == 0) {
          this->set_preset_(climate::CLIMATE_PRESET_NONE);
        } else {
          this->set_preset_(climate::CLIMATE_PRESET_HOME);
        }
      }

      // Only publish if something actually changed
      if (sp_changed || hold_changed || this->preset != old_preset) {
        changed = true;
      }
    }
  }

  if (changed) {
    publish_state();
  }
}

} // namespace infinitesp
} // namespace esphome
