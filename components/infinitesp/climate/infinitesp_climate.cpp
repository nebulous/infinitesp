#include "infinitesp_climate.h"

namespace esphome {
namespace infinitesp {

climate::ClimateTraits InfinitESPClimate::traits() {
  auto traits = climate::ClimateTraits();
  using namespace esphome::climate;
  traits.add_feature_flags(CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           CLIMATE_SUPPORTS_ACTION |
                           CLIMATE_SUPPORTS_TWO_POINT_TARGET_TEMPERATURE);

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
  traits.add_supported_preset(climate::CLIMATE_PRESET_HOME);
  traits.add_supported_preset(climate::CLIMATE_PRESET_AWAY);
  traits.add_supported_preset(climate::CLIMATE_PRESET_SLEEP);

  const char *const custom_presets[] = {
      PRESET_SCHEDULE,
      PRESET_WAKE,
      PRESET_HOLD_TIMED,
      PRESET_HOLD_PERM,
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

  // Handle setpoint changes. HA sends target_temperature in heat/cool modes,
  // target_temperature_low/high in heat_cool mode.
  if (call.get_target_temperature().has_value()) {
    float target_c = call.get_target_temperature().value();
    uint8_t target_bus = parent_->celsius_to_setpoint(target_c);
    if (this->mode == climate::CLIMATE_MODE_HEAT) {
      heat_sp_ = target_bus;
    } else if (this->mode == climate::CLIMATE_MODE_COOL) {
      cool_sp_ = target_bus;
    }
    parent_->set_zone_setpoint(zone_, heat_sp_, cool_sp_);
    this->target_temperature = target_c;
    set_pending_setpoint_(heat_sp_, cool_sp_);
  }
  if (call.get_target_temperature_low().has_value()) {
    float target_c = call.get_target_temperature_low().value();
    uint8_t target_bus = parent_->celsius_to_setpoint(target_c);
    heat_sp_ = target_bus;
    parent_->set_zone_setpoint(zone_, heat_sp_, cool_sp_);
    this->target_temperature_low = target_c;
    set_pending_setpoint_(heat_sp_, cool_sp_);
  }

  if (call.get_target_temperature_high().has_value()) {
    float target_c = call.get_target_temperature_high().value();
    uint8_t target_bus = parent_->celsius_to_setpoint(target_c);
    cool_sp_ = target_bus;
    parent_->set_zone_setpoint(zone_, heat_sp_, cool_sp_);
    this->target_temperature_high = target_c;
    set_pending_setpoint_(heat_sp_, cool_sp_);
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
      case climate::CLIMATE_PRESET_HOME:
        parent_->apply_activity(zone_, COMFORT_HOME, InfinitESPComponent::HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = InfinitESPComponent::HOLD_PERMANENT;
        last_activity_ = COMFORT_HOME;
        break;
      case climate::CLIMATE_PRESET_AWAY:
        parent_->apply_activity(zone_, COMFORT_AWAY, InfinitESPComponent::HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = InfinitESPComponent::HOLD_PERMANENT;
        last_activity_ = COMFORT_AWAY;
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        parent_->apply_activity(zone_, COMFORT_SLEEP, InfinitESPComponent::HOLD_PERMANENT);
        this->set_preset_(preset);
        hold_duration_ = InfinitESPComponent::HOLD_PERMANENT;
        last_activity_ = COMFORT_SLEEP;
        break;
      default:
        break;
    }
  }

  // Handle custom presets
  if (call.has_custom_preset()) {
    auto custom = call.get_custom_preset();
    if (custom == PRESET_SCHEDULE) {
      // Cancel hold — resume schedule
      parent_->set_zone_hold(zone_, 0);
      hold_duration_ = 0;
      last_activity_ = NO_ACTIVITY;
      this->set_custom_preset_(PRESET_SCHEDULE);
      ESP_LOGI("InfinitESP", "Zone %d: preset PER SCHEDULE → cancel hold", zone_);
    } else if (custom == PRESET_WAKE) {
      parent_->apply_activity(zone_, COMFORT_WAKE, InfinitESPComponent::HOLD_PERMANENT);
      hold_duration_ = InfinitESPComponent::HOLD_PERMANENT;
      last_activity_ = COMFORT_WAKE;
      this->set_custom_preset_(PRESET_WAKE);
      ESP_LOGI("InfinitESP", "Zone %d: preset WAKE → permanent hold", zone_);
    }
    // Hold Timer and Hold Indefinitely are read-only states set from bus data.
    // Users cancel holds via the Per Schedule preset.
  }

  publish_state();
}

void InfinitESPClimate::set_pending_setpoint_(uint8_t heat, uint8_t cool) {
  pending_heat_ = heat;
  pending_cool_ = cool;
  pending_active_ = true;
  pending_until_ms_ = millis() + PENDING_SETPOINT_WINDOW_MS;
  ESP_LOGD("InfinitESP", "Zone %d: pending setpoint overlay ht=%d cl=%d for %dms",
           zone_, heat, cool, PENDING_SETPOINT_WINDOW_MS);
}

bool InfinitESPClimate::compute_action_() {
  // stage>0 means the ODU has demand for a mode (Carrier SAM spec). The mode
  // nibble carries direction during stage>0. Gate per-zone on the damper: a
  // closed damper means this zone isn't receiving conditioned air even while
  // the system runs. With no zone controller, zone_damper_open() is always
  // true (single-zone system, action tracks the system 1:1).
  climate::ClimateAction action = climate::CLIMATE_ACTION_IDLE;
  if (last_stage_ > 0 && parent_->zone_damper_open(zone_)) {
    switch (last_mode_) {
      case SYSMODE_HEAT:  action = climate::CLIMATE_ACTION_HEATING; break;
      case SYSMODE_COOL:  action = climate::CLIMATE_ACTION_COOLING; break;
      case SYSMODE_EHEAT: action = climate::CLIMATE_ACTION_HEATING; break;
      default: break;  // AUTO during stage>0: direction unknown (issue #7)
    }
  }
  if (action != current_action_) {
    current_action_ = action;
    this->action = action;
    return true;
  }
  return false;
}

void InfinitESPClimate::on_register_update(uint8_t device_addr, uint16_t register_key) {
  bool changed = false;

  if (register_key == REG_SAM_STATE) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (data && data->size() >= REG3B02_STAGMODE + 1) {
      uint8_t idx = zone_ - 1;
      uint8_t active = data->at(REG3B02_ACTIVE_ZONES);
      if (!(active & (1 << idx)))
        return;

      float temp_bus = (float)data->at(REG3B02_TEMPS + idx);
      float temp = parent_->bus_temp_to_celsius(temp_bus);
      if (temp != current_temp_) {
        current_temp_ = temp;
        this->current_temperature = temp;
        changed = true;
      }

      uint8_t stagmode = data->at(REG3B02_STAGMODE);
      uint8_t mode = stagmode & 0x0F;
      uint8_t stage = (stagmode >> 4) & 0x0F;

      // Cache the raw stage/mode nibbles so action can be recomputed when a
      // ZC damper update arrives without a new 3B02 frame. During stage>0 the
      // mode nibble reflects direction (heat/cool); at stage==0 it holds the
      // requested mode and action is IDLE regardless.
      last_stage_ = stage;
      last_mode_ = mode;
      // On conventional 2-stage equipment (stage 1-2), the thermostat rewrites
      // the mode nibble to heat/cool during active operation, so AUTO while
      // stage>0 is genuinely unexpected - log it. On variable-speed equipment
      // (stage 3+), the mode nibble can stay AUTO during active operation
      // (issue #7), so suppress the warning there. compute_action_() handles
      // the AUTO case independently (falls through to IDLE).
      if (mode == SYSMODE_AUTO && stage > 0 && stage <= 2) {
        ESP_LOGW("infinitesp", "Unexpected: stage=%d but mode=AUTO", stage);
      }
      if (compute_action_())
        changed = true;

      // Mode update logic:
      // - When stage==0 (idle): mode nibble is the true requested mode. Always trust it.
      // - When stage>0 (active): mode nibble shows active direction (heat/cool) even if
      //   the requested mode is AUTO. So only trust it for non-AUTO modes.
      //   On first boot (sys_mode_==0xFF), accept any reading to avoid staying stuck
      //   at CLIMATE_MODE_OFF.
      bool can_update_mode = false;
      if (stage == 0) {
        can_update_mode = true;
      } else if (mode != SYSMODE_AUTO && mode != SYSMODE_EHEAT) {
        // Actively heating or cooling — mode nibble is correct for heat/cool/off.
        // Don't update from AUTO (stage>0 can't be AUTO on the bus anyway).
        can_update_mode = true;
      }
      // On first boot, accept any mode reading to unstick from OFF
      if (!can_update_mode && sys_mode_ == 0xFF)
        can_update_mode = true;

      if (can_update_mode && mode != sys_mode_) {
        sys_mode_ = mode;
        switch (mode) {
          case SYSMODE_HEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
          case SYSMODE_COOL: this->mode = climate::CLIMATE_MODE_COOL; break;
          case SYSMODE_AUTO: this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
          case SYSMODE_EHEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
          case SYSMODE_OFF:
          default: this->mode = climate::CLIMATE_MODE_OFF; break;
        }
        // Update setpoints based on mode: single target for heat/cool, dual for heat_cool
        float heat_c = parent_->setpoint_to_celsius(heat_sp_);
        float cool_c = parent_->setpoint_to_celsius(cool_sp_);
        this->target_temperature_low = heat_c;
        this->target_temperature_high = cool_c;
        if (this->mode == climate::CLIMATE_MODE_HEAT)
          this->target_temperature = heat_c;
        else if (this->mode == climate::CLIMATE_MODE_COOL)
          this->target_temperature = cool_c;
        changed = true;
      }
    }
  }

  if (register_key == REG_SAM_ZONES) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_ZONES);
    if (data && data->size() >= REG3B03_COOL_SETPOINTS + 8) {
      uint8_t idx = zone_ - 1;
      uint8_t active = data->at(REG3B03_ACTIVE_ZONES);
      if (!(active & (1 << idx)))
        return;

      uint8_t new_heat = data->at(REG3B03_HEAT_SETPOINTS + idx);
      uint8_t new_cool = data->at(REG3B03_COOL_SETPOINTS + idx);
      uint8_t new_fan = data->at(REG3B03_FAN_MODES + idx);

      // Pending setpoint overlay: after a write, suppress stale poll data
      // for a window to prevent HA snapback while the thermostat processes the change.
      if (pending_active_ && millis() < pending_until_ms_) {
        if (new_heat != pending_heat_)
          new_heat = pending_heat_;
        if (new_cool != pending_cool_)
          new_cool = pending_cool_;
      } else {
        pending_active_ = false;  // window expired or thermostat confirmed
      }

      // Track whether cached setpoints changed (for publish gating)
      bool sp_changed = false;
      if (new_heat != heat_sp_) {
        heat_sp_ = new_heat;
        sp_changed = true;
      }
      if (new_cool != cool_sp_) {
        cool_sp_ = new_cool;
        sp_changed = true;
      }

      // Always update target temperatures from bus data.
      // On first boot, target_temperature_low/high are NaN even though the bus
      // values may match our defaults (68/76). We must set them unconditionally.
      float heat_c = parent_->setpoint_to_celsius(new_heat);
      float cool_c = parent_->setpoint_to_celsius(new_cool);
      if (std::isnan(this->target_temperature_low) || std::isnan(this->target_temperature_high))
        sp_changed = true;  // force publish to initialize HA state
      this->target_temperature_low = heat_c;
      this->target_temperature_high = cool_c;
      // Set single target_temperature for the current mode (HA uses this in heat/cool)
      if (this->mode == climate::CLIMATE_MODE_HEAT)
        this->target_temperature = heat_c;
      else if (this->mode == climate::CLIMATE_MODE_COOL)
        this->target_temperature = cool_c;

      if (sp_changed)
        changed = true;
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
      uint16_t hold_dur = parent_->get_zone_hold_duration(zone_);
      ESP_LOGD("InfinitESP", "Zone %d hold: zones_holding=0x%02X duration=%d",
               zone_, data->at(REG3B03_ZONES_HOLDING), hold_dur);
      if (hold_dur != hold_duration_) {
        hold_duration_ = hold_dur;
        hold_changed = true;
      }

      // Determine preset display.
      // Priority: if hold is active, show hold preset. Otherwise, match
      // setpoints+fan against comfort profiles to infer activity.
      auto old_custom = this->get_custom_preset();
      auto old_preset = this->preset;

      if (hold_duration_ > 0) {
        // Hold is active — show hold preset and compute end time
        if (hold_duration_ >= InfinitESPComponent::HOLD_PERMANENT) {
          this->set_custom_preset_(PRESET_HOLD_PERM);
          hold_end_time_ = "Permanent";
        } else {
          this->set_custom_preset_(PRESET_HOLD_TIMED);
          std::string end = parent_->format_hold_end(hold_duration_);
          if (!end.empty())
            hold_end_time_ = end;
        }
      } else {
        hold_end_time_.clear();
        // No hold — match setpoints+fan against comfort profiles from register 400A.
        auto *comfort = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_COMFORT);
        ESP_LOGD("InfinitESP", "Zone %d preset match: heat=%d cool=%d fan=%d comfort=%p size=%d",
                 zone_, new_heat, new_cool, new_fan,
                 comfort ? (void*)comfort : nullptr,
                 comfort ? (int)comfort->size() : -1);
        bool matched = false;
        if (comfort && comfort->size() >= COMFORT_ACTIVITY_COUNT * COMFORT_ENTRY_SIZE) {
          for (uint8_t a = 0; a < COMFORT_ACTIVITY_COUNT; a++) {
            uint8_t base = a * COMFORT_ENTRY_SIZE;
            // Comfort profiles use different encoding than 3B03 setpoints:
            // °F mode: both are whole °F — direct comparison
            // °C mode: comfort = half-degrees, setpoints = whole °C
            //   Convert both to °C for comparison
            float ht_c = parent_->comfort_byte_to_celsius((*comfort)[base + 0]);
            float cl_c = parent_->comfort_byte_to_celsius((*comfort)[base + 1]);
            float sp_ht_c = parent_->setpoint_to_celsius(new_heat);
            float sp_cl_c = parent_->setpoint_to_celsius(new_cool);
            // Compare as °C with tolerance for half-degree rounding
            bool ht_match = (fabsf(ht_c - sp_ht_c) < 0.3f);
            bool cl_match = (fabsf(cl_c - sp_cl_c) < 0.3f);
            if (ht_match && cl_match && (*comfort)[base + 2] == new_fan) {
              last_activity_ = a;
              switch (a) {
                case COMFORT_HOME:  this->set_preset_(climate::CLIMATE_PRESET_HOME); break;
                case COMFORT_AWAY:  this->set_preset_(climate::CLIMATE_PRESET_AWAY); break;
                case COMFORT_SLEEP: this->set_preset_(climate::CLIMATE_PRESET_SLEEP); break;
                case COMFORT_WAKE:  this->set_custom_preset_(PRESET_WAKE); break;
                default:
                  this->set_custom_preset_(PRESET_SCHEDULE);
                  break;
              }
              matched = true;
              break;
            }
          }
        }
        if (!matched) {
          last_activity_ = NO_ACTIVITY;
          this->set_custom_preset_(PRESET_SCHEDULE);
        }
      }

      // Only publish if something actually changed
      bool preset_changed = (this->preset != old_preset) ||
                            (this->get_custom_preset() != old_custom);
      if (sp_changed || hold_changed || preset_changed) {
        changed = true;
      }
    }
  }

  // ZC damper changes can flip this zone's action without a new 3B02 frame
  // (e.g. the thermostat closes this zone's damper while the ODU keeps running
  // for another zone). Recompute from cached stage/mode + fresh damper state.
  if (register_key == REG_ZC_DAMPER_CMD || register_key == REG_ZC_ZONE_CONFIG) {
    if (compute_action_())
      changed = true;
  }

  if (changed) {
    publish_state();
  }
}

} // namespace infinitesp
} // namespace esphome
