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
      PRESET_VACATION,
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
    sys_mode_ = sys;                 // set before set_system_mode so the
                                     // broadcast's self-call is idempotent
    parent_->set_system_mode(sys);    // propagates to all sibling zones
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
    // Write the active bound, not the target_temperature union alias (which
    // overlaps low — see CLIMATE union gotcha). HA renders the cool slider from
    // target_temperature_high and the heat slider from target_temperature_low.
    if (this->mode == climate::CLIMATE_MODE_HEAT)
      this->target_temperature_low = target_c;
    else if (this->mode == climate::CLIMATE_MODE_COOL)
      this->target_temperature_high = target_c;
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
    } else if (custom == PRESET_VACATION) {
      // Vacation is reported FROM the bus (setpoint-override detection below);
      // setting it from HA isn't supported yet (would require writing the vacation
      // config and triggering the system-wide override). No-op — the detected
      // state reasserts on the next bus poll.
      ESP_LOGW("InfinitESP", "Zone %d: setting Vacation from HA is not yet supported", zone_);
    }
    // Hold Timer and Hold Indefinitely are read-only states set from bus data.
    // Users cancel holds via the Per Schedule preset.
  }

  publish_state();
}

void InfinitESPClimate::on_system_mode_commanded(uint8_t sys) {
  // Called by the parent's set_system_mode() when ANY source (this zone's
  // control(), another zone's, or ASCII MODE!) changes the global system mode.
  // The commanding zone already set sys_mode_ in control(), so the assignment
  // below is a no-op for it; for sibling zones it updates mode + setpoints in
  // lockstep rather than waiting for the lagging bus confirm (which the
  // can_update_mode gate would defer until the next idle frame).
  //
  // Always arm the pending-mode window — even for the commanding zone — so a
  // stale AUTO-direction nibble arriving before the bus confirms can't revert
  // the just-commanded mode via the mode-trust branch.
  pending_mode_ = sys;
  pending_mode_active_ = true;
  pending_mode_until_ms_ = millis() + PENDING_MODE_WINDOW_MS;
  if (sys == sys_mode_)
    return;
  sys_mode_ = sys;
  switch (sys) {
    case SYSMODE_HEAT:  this->mode = climate::CLIMATE_MODE_HEAT; break;
    case SYSMODE_COOL:  this->mode = climate::CLIMATE_MODE_COOL; break;
    case SYSMODE_AUTO:  this->mode = climate::CLIMATE_MODE_HEAT_COOL; break;
    case SYSMODE_EHEAT: this->mode = climate::CLIMATE_MODE_HEAT; break;
    case SYSMODE_OFF:
    default:            this->mode = climate::CLIMATE_MODE_OFF; break;
  }
  // Two-point entity: low/high are the source of truth (never the
  // target_temperature union alias of low). Each zone uses its own setpoints.
  this->target_temperature_low = parent_->setpoint_to_celsius(heat_sp_);
  this->target_temperature_high = parent_->setpoint_to_celsius(cool_sp_);
  ESP_LOGD("InfinitESP", "Zone %d: system mode broadcast -> %d", zone_, sys);
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
  // stage>0 means the system has active demand (Carrier SAM spec). Gate
  // per-zone on the damper: a closed damper means this zone isn't receiving
  // conditioned air even while the system runs. With no zone controller,
  // zone_damper_open() is always true (single-zone system, action tracks
  // the system 1:1).
  //
  // Direction (two-layer design):
  //  1. mode nibble HEAT/COOL/EHEAT → trust it. This covers furnace heating
  //     (a gas furnace is conventional 2-stage → nibble flips to HEAT during
  //     active heat). No inference needed.
  //  2. mode nibble AUTO → infer from THIS zone's demand vs its own setpoints
  //     (temp > cool_sp → COOLING, temp < heat_sp → HEATING). Uses the same
  //     inputs the thermostat uses; system-type-agnostic (AC/HP/furnace).
  //     On variable-speed systems the nibble stays AUTO during active operation,
  //     so this is the path that resolves the old "always IDLE" bug.
  //  3. else (deadband) → IDLE. Never guess.
  climate::ClimateAction action = climate::CLIMATE_ACTION_IDLE;
  if (last_stage_ > 0 && parent_->zone_damper_open(zone_)) {
    switch (last_mode_) {
      case SYSMODE_HEAT:  action = climate::CLIMATE_ACTION_HEATING; break;
      case SYSMODE_COOL:  action = climate::CLIMATE_ACTION_COOLING; break;
      case SYSMODE_EHEAT: action = climate::CLIMATE_ACTION_HEATING; break;
      case SYSMODE_AUTO:
        // Demand of this zone (the one being served, damper open). Bus-fresh
        // cached values from 3B02/3B03, not HA-side attributes. Use >= / <= not
        // strict > / <: the thermostat cools AT cool_sp (hysteresis) — observed
        // temp==cool_sp while actively cooling — so strict > would read deadband
        // and misreport IDLE. Deadband is heat_sp < temp < cool_sp.
        if (current_temp_ >= parent_->setpoint_to_celsius(cool_sp_))
          action = climate::CLIMATE_ACTION_COOLING;
        else if (current_temp_ <= parent_->setpoint_to_celsius(heat_sp_))
          action = climate::CLIMATE_ACTION_HEATING;
        break;  // deadband → IDLE
      default: break;
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
      // Note: the mode nibble during stage>0 does NOT reliably indicate
      // direction across controls. The Touch control rewrites it to heat/cool
      // during active 2-stage operation, but variable-speed gear keeps AUTO
      // mid-cycle (issue #7) and legacy UIZ controls also keep AUTO mid-cycle
      // on conventional 2-stage equipment (issue #11). So we never treat a
      // stage>0 AUTO nibble as anomalous. compute_action_() resolves the
      // heating/cooling direction for the AUTO case via per-zone demand
      // inference, independent of the nibble.
      if (compute_action_())
        changed = true;

      // Mode update logic — when can we copy the bus mode nibble into the HA
      // `mode` (the user's requested POLICY: heat/cool/heat_cool/off)? The stagmode
      // nibble is overloaded: at stage==0 it's the requested policy, at stage>0 it's
      // the active DIRECTION (heat/cool). Direction and policy only DIVERGE for an
      // AUTO-policy system mid-cycle (AUTO at idle → COOL/HEAT while running); for
      // every other policy the stage>0 nibble equals the policy. So we trust the
      // nibble except in the ONE case where trusting it would flap mode
      // heat_cool→cool→heat_cool: an already-established AUTO policy seeing a
      // HEAT/COOL direction nibble. Trusting direction when policy isn't (yet)
      // known AUTO is correct and avoids holding a stale idle value (e.g. OFF
      // shown while cooling) across the whole cycle.
      //   - stage==0: always trust (nibble == policy)
      //   - stage>0 + AUTO nibble: trust (variable-speed, issue #7; unambiguous)
      //   - stage>0 + HEAT/COOL/OFF nibble, sys_mode_ != AUTO: trust (direction==policy)
      //   - stage>0 + HEAT/COOL nibble, sys_mode_ == AUTO: suppress (would flap)
      //   First boot (sys_mode_==0xFF): covered by the !=AUTO branch; best-guess
      //   that self-corrects at the next idle frame.
      bool can_update_mode = false;
      if (pending_mode_active_ && millis() < pending_mode_until_ms_) {
        // Hold the commanded mode: the bus hasn't confirmed it yet, and a stale
        // AUTO-direction nibble (stage>0) would otherwise revert it via the
        // AUTO-trust branch below. sys_mode_ already == pending_mode_. Window
        // expires on confirm or timeout, then normal bus-trust resumes.
      } else {
        pending_mode_active_ = false;
        if (stage == 0 || mode == SYSMODE_AUTO || sys_mode_ != SYSMODE_AUTO)
          can_update_mode = true;
      }
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
        // low/high are the source of truth for this two-point entity; never
        // write the target_temperature union alias (it overlaps low).
        this->target_temperature_low = heat_c;
        this->target_temperature_high = cool_c;
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
      // low/high are the source of truth; never write target_temperature
      // (union alias of low — writing it in cool mode clobbers the heat sp).
      this->target_temperature_low = heat_c;
      this->target_temperature_high = cool_c;

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

      // Vacation is the highest-priority preset: a system-wide override where the
      // thermostat forces every zone's setpoints to register 4012's min/max. 4012
      // itself only carries CONFIG (it reads identically whether vacation is
      // active or not), so the reliable active signal is the setpoint MATCH:
      // heat==4012[0] && cool==4012[1]. Confirmed on hardware: vacation ON → all
      // zones heat/cool == 4012 min/max; OFF → setpoints return to schedule.
      // 4012 is thermostat-internal and only fetched via slow-poll when emulating
      // the SAM, so in pure-passive mode vac is null and this is a harmless no-op.
      bool vacation_active = false;
      auto *vac = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_VACATION);
      if (vac && vac->size() >= 2) {
        uint8_t vac_min = (*vac)[0];  // heat setpoint, same bus encoding as 3B03
        uint8_t vac_max = (*vac)[1];  // cool setpoint
        // 0xFF = unconfigured vacation; only match real configured values
        if (vac_min != 0xFF && vac_max != 0xFF && vac_min <= vac_max &&
            new_heat == vac_min && new_cool == vac_max) {
          vacation_active = true;
          last_activity_ = NO_ACTIVITY;
          hold_end_time_.clear();
          this->set_custom_preset_(PRESET_VACATION);
        }
      }

      if (!vacation_active && hold_duration_ > 0) {
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
      } else if (!vacation_active) {
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
