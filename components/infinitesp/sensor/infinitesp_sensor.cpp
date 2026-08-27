#include "infinitesp_sensor.h"
#include <cstring>

namespace esphome {
namespace infinitesp {

void InfinitESPSensor::on_register_update(uint8_t device_addr, uint16_t register_key) {
  float value = NAN;

  // Generic raw_register sensor: match the user-configured device+register exactly,
  // then decode per datatype/scale (or a full-frame lambda). Surfaces arbitrary bus
  // fields the curated sensors don't cover (e.g. older/cooling-only ODU families).
  if (sensor_type_ == "raw_register") {
    if (device_addr != raw_device_ || register_key != raw_register_)
      return;
    const auto *data = parent_->get_register(raw_device_, raw_register_);
    if (data == nullptr)
      return;

    if (raw_lambda_) {
      value = raw_lambda_(*data);
    } else if (!raw_datatype_.empty()) {
      size_t o = raw_offset_;
      if (o >= data->size())
        return;
      auto be16 = [&](size_t i) -> uint16_t {
        return (uint16_t) (((*data)[i] << 8) | (*data)[i + 1]);
      };
      auto be32 = [&](size_t i) -> uint32_t {
        return ((uint32_t) (*data)[i] << 24) | ((uint32_t) (*data)[i + 1] << 16) |
               ((uint32_t) (*data)[i + 2] << 8) | (uint32_t) (*data)[i + 3];
      };
      if (raw_datatype_ == "uint8") {
        value = (float) (*data)[o];
      } else if (raw_datatype_ == "int8") {
        value = (float) (int8_t) (*data)[o];
      } else if (raw_datatype_ == "uint16_be") {
        if (o + 1 >= data->size())
          return;
        value = (float) be16(o);
      } else if (raw_datatype_ == "int16_be") {
        if (o + 1 >= data->size())
          return;
        value = (float) (int16_t) be16(o);
      } else if (raw_datatype_ == "uint32_be") {
        if (o + 3 >= data->size())
          return;
        value = (float) be32(o);
      } else if (raw_datatype_ == "int32_be") {
        if (o + 3 >= data->size())
          return;
        value = (float) (int32_t) be32(o);
      } else if (raw_datatype_ == "f32_be") {
        if (o + 3 >= data->size())
          return;
        uint32_t u = be32(o);
        float f;
        memcpy(&f, &u, sizeof(f));
        value = f;
      } else {
        return;  // unknown datatype
      }
      value *= raw_scale_;
    } else {
      return;  // no datatype and no lambda (config validation should prevent this)
    }

    if (raw_has_range_ && (value < raw_min_ || value > raw_max_))
      return;  // out of plausibility range (guards the unpopulated-register footgun)
    publish_state(value);
    return;
  }

  // SAM state registers (3B02): temperature, humidity, outdoor temp
  if (register_key == REG_SAM_STATE) {
    auto *data = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (!data || data->size() < 21)
      return;

    if (sensor_type_ == "outdoor_temperature") {
      value = parent_->bus_temp_to_celsius((float) data->at(REG3B02_OUTDOOR_TEMP));
    } else if (sensor_type_ == "temperature") {
      uint8_t idx = zone_ - 1;
      if (data->at(REG3B02_ACTIVE_ZONES) & (1 << idx)) {
        value = parent_->bus_temp_to_celsius((float) data->at(REG3B02_TEMPS + idx));
      }
    } else if (sensor_type_ == "humidity") {
      uint8_t idx = zone_ - 1;
      if (data->at(REG3B02_ACTIVE_ZONES) & (1 << idx)) {
        value = (float) data->at(REG3B02_HUMIDITY + idx);
      }
    }
  }

  // fault_timestamp: state = epoch time the newest fault-log entry was logged
  // (observation lag applies: 4202 arrives via the slow-poll rotation, so a birth
  // can be seen up to one rotation late; the VALUE is the true logging time).
  // Age math is bus-relative (entry fields + day trailer + SAM 3B02 bus clock),
  // then anchored to the ESPHome time source. No liveness exists on the bus; see
  // the 0x4202 layout block in infinitesp.h (issue #22).
  if (sensor_type_ == "fault_timestamp" && register_key == REG_TSTAT_FAULTS) {
    auto *faults = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_FAULTS);
    auto *state = parent_->get_register(parent_->get_sam_address(), REG_SAM_STATE);
    if (faults && faults->size() >= FAULT_REG_SIZE && state &&
        state->size() >= REG3B02_MINUTES + 2) {
      uint16_t now_bus_min = ((uint16_t) state->at(REG3B02_MINUTES) << 8) |
                             state->at(REG3B02_MINUTES + 1);
      int32_t best_age = -1;
      uint8_t best_code = 0, best_i = 0;
      for (uint8_t i = 0; i < FAULT_ENTRY_COUNT; i++) {
        if ((*faults)[i * FAULT_ENTRY_SIZE + FAULT_CODE] == 0 &&
            (*faults)[i * FAULT_ENTRY_SIZE + FAULT_SOURCE] == 0)
          continue;  // empty slot
        if (!fault_entry_time_valid(*faults, i))
          continue;  // garbage time fields (non-tstat source packing)
        int32_t age = fault_entry_age_minutes(*faults, i, now_bus_min);
        if (age < 0)
          continue;
        if (best_age < 0 || age < best_age) {
          best_age = age;
          best_code = (*faults)[i * FAULT_ENTRY_SIZE + FAULT_CODE];
          best_i = i;
        }
      }
      ESP_LOGD("InfinitESP", "fault_timestamp: newest entry code=%u age=%dmin", best_code,
               best_age);
      if (best_age >= 0) {
        // Publish only when the newest entry changes: the value depends on the
        // 3B02 clock sample, which drifts vs wall time between rotations.
        uint8_t bb = best_i * FAULT_ENTRY_SIZE;
        uint32_t sig = (uint32_t) best_code |
                       ((uint32_t) (*faults)[bb + FAULT_HOUR] << 8) |
                       ((uint32_t) (*faults)[bb + FAULT_MINUTE] << 13) |
                       (((uint32_t) (((*faults)[bb + FAULT_DAYS_HI] << 8) |
                                     (*faults)[bb + FAULT_DAYS_LO]) & 0x1FFF) << 19);
        bool sig_changed = !fault_sig_valid_ || sig != fault_last_sig_;
        if (sig_changed) {
          fault_sig_valid_ = true;
          fault_last_sig_ = sig;
          if (!epoch_provider_) {
            if (!fault_time_warned_) {
              fault_time_warned_ = true;
              ESP_LOGW("InfinitESP", "fault_timestamp: no time_id configured; cannot publish. "
                                     "Set time_id on the sensor to your time source.");
            }
          } else {
            time_t now_epoch = epoch_provider_();  // 0 = not yet synced
            if (now_epoch != 0) {
              publish_state((float) now_epoch - (float) best_age * 60.0f);
            } else if (!fault_time_warned_) {
              fault_time_warned_ = true;
              ESP_LOGW("InfinitESP", "fault_timestamp: time source not yet synced; publishing deferred");
            }
          }
        }
      }
    }
    return;  // handled; skip the generic publish below
  }

  // Thermostat vacation settings (4012)
  if (register_key == REG_TSTAT_VACATION && sensor_type_ == "vacation_min_temp") {
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_VACATION);
    if (data && data->size() >= 2)
      value = parent_->setpoint_to_celsius(data->at(0));
  }
  if (register_key == REG_TSTAT_VACATION && sensor_type_ == "vacation_max_temp") {
    auto *data = parent_->get_register(ADDR_THERMOSTAT, REG_TSTAT_VACATION);
    if (data && data->size() >= 2)
      value = parent_->setpoint_to_celsius(data->at(1));
  }

  // IDU (Indoor Unit) passively snooped registers
  // Blower RPM from register 0306
  if (register_key == REG_IDU_STATUS && sensor_type_ == "blower_rpm") {
    auto *data = parent_->get_register(device_addr, REG_IDU_STATUS);
    if (data) {
      float rpm = parent_->idu_blower_rpm_(*data);
      if (!std::isnan(rpm))
        value = rpm;
    }
  }

  // Airflow CFM from register 0316
  if (register_key == REG_IDU_CONFIG && sensor_type_ == "airflow_cfm") {
    auto *data = parent_->get_register(device_addr, REG_IDU_CONFIG);
    if (data) {
      float cfm = parent_->idu_airflow_cfm_(*data);
      if (!std::isnan(cfm))
        value = cfm;
    }
  }

  // ODU (Outdoor Unit) passively snooped registers
  // Compressor RPM from register 0604. Two uint16 BE pairs per stage:
  //   target (commanded) at [0..1], actual (measured) at [2..3].
  // See Infinitude OutdoorUnit.pm 0604 (target_rpm / current_rpm — Infinitude
  // uses 'current' here but 'actual' elsewhere; InfinitESP unifies on 'actual').
  if (register_key == REG_ODU_COMP_SPEED && sensor_type_ == "target_compressor_rpm") {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data) {
      float rpm = parent_->odu_compressor_target_rpm_(*data);
      if (!std::isnan(rpm))
        value = rpm;
    }
  }
  if (register_key == REG_ODU_COMP_SPEED && sensor_type_ == "compressor_rpm") {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data) {
      float rpm = parent_->odu_compressor_actual_rpm_(*data);
      if (!std::isnan(rpm))
        value = rpm;
    }
  }

  // Compressor drive frequency from register 0608 (uint16 BE at [5..6], 0.1 Hz)
  if (register_key == REG_ODU_DEMAND && sensor_type_ == "compressor_frequency") {
    auto *data = parent_->get_register(device_addr, REG_ODU_DEMAND);
    if (data) {
      float f = parent_->odu_compressor_frequency_(*data);
      if (!std::isnan(f))
        value = f;
    }
  }

  // Expansion valve position from register 0608 byte [2] (0-100 percent).
  // Expect 0 (off) or 100 (running) most of the time; brief ramps on cycle transitions.
  if (register_key == REG_ODU_DEMAND && sensor_type_ == "odu_expansion_valve") {
    auto *data = parent_->get_register(device_addr, REG_ODU_DEMAND);
    if (data) {
      float v = parent_->odu_expansion_valve_(*data);
      if (!std::isnan(v))
        value = v;
    }
  }

  // Variable-speed stage index from register 060e (byte 0: 0=off, 1..5=stage)
  if (register_key == REG_ODU_STAGE_INFO && sensor_type_ == "odu_stage") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STAGE_INFO);
    if (data) {
      float s = parent_->odu_stage_(*data);
      if (!std::isnan(s))
        value = s;
    }
  }

  // Commanded compressor stage from register 0605 (float32 BE at [0..3]: 0.0/1.0..5.0)
  // Write-only (thermostat→ODU); captured in handle_passive_frame_. Drives the
  // actual stage (060e) with ~15s lag.
  if (register_key == REG_ODU_CMD_STAGE && sensor_type_ == "odu_commanded_stage") {
    auto *data = parent_->get_register(device_addr, REG_ODU_CMD_STAGE);
    if (data) {
      float s = parent_->odu_commanded_stage_(*data);
      if (!std::isnan(s))
        value = s;
    }
  }


  // ODU line voltage from register 0304 byte 7 (whole volts, state-independent).
  // Validated against Carrier cloud linevolt: bus 238-240 vs cloud 239V.
  if (register_key == REG_ODU_STATUS3 && sensor_type_ == "odu_line_voltage") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS3);
    if (data) {
      float v = parent_->odu_line_voltage_(*data);
      if (!std::isnan(v))
        value = v;
    }
  }

  // ODU operating mode from register 0304 (byte 11 of payload, which is data[10])
  if (register_key == REG_ODU_STATUS3 && sensor_type_ == "odu_operating_mode") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS3);
    if (data) {
      float mode = parent_->odu_operating_mode_(*data);
      if (!std::isnan(mode))
        value = mode;
    }
  }

  // ODU IEEE754 float32 values from register 061f. idx 1..5 are DELTAS
  // (superheat/subcooling/control ΔT), published in NATIVE °F (no conversion).
  // These sensors have no device_class in yaml: HA's temperature conversion applies
  // a +32 offset that corrupts deltas (3°F → 1.67°C → displayed 35°F).
  // idx 6 is dimensionless.
  // Layout via accessor odu_float_(idx): idx 1..6 at offset 1+(idx-1)*4.
  //   1: superheat target  2: superheat actual  3: subcooling target
  //   4: subcooling actual 5: discharge-related control delta (NOT discharge superheat -
  //      refuted: it goes negative ~75% while running, impossible for superheat).
  //      Likely a discharge-temp/superheat control deviation incorporating head
  //      pressure. Exact identity unconfirmed. See Infinitude OutdoorUnit.pm 061F.
  //   6: dimensionless constant
  if (register_key == REG_ODU_FLOATS && sensor_type_.rfind("odu_float_", 0) == 0) {
    auto *data = parent_->get_register(device_addr, REG_ODU_FLOATS);
    if (data) {
      int idx = sensor_type_[10] - '0';  // odu_float_N → N
      if (idx >= 1 && idx <= 6) {
        float fval = parent_->odu_float_(*data, idx);
        if (!std::isnan(fval))
          value = fval;  // idx 1..5 = native °F delta, idx 6 = dimensionless (no conversion)
      }
    }
  }

  // ODU register 0302: int16 BE / 16, always native °F. Absolute temps convert
  // to °C; the superheat delta (idx 3) is published in native °F with no
  // device_class (HA's temp conversion adds +32, corrupting deltas).
  // Field idx via accessor odu_status1_meas_f_(idx): 0=outdoor 1=coil 2=suction
  // 3=suction_superheat(ΔT) 4=indoor_amb 5=discharge. idx 3 confirmed superheat
  //   (matches thermostat display, e.g. 16-17°F or 3.0°F depending on state).
  if (register_key == REG_ODU_STATUS1) {
    struct Field { const char *suffix; uint8_t idx; bool delta; };
    static const Field fields[] = {
        {"odu_outdoor_temp", 0, false}, {"odu_coil_temp", 1, false},
        {"odu_suction_temp", 2, false}, {"odu_suction_superheat", 3, true},
        {"odu_indoor_ambient", 4, false}, {"odu_discharge_temp", 5, false},
    };
    for (const auto &fld : fields) {
      if (sensor_type_ != fld.suffix)
        continue;
      auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
      if (data) {
        // Unpopulated-register guard: some systems serve 0302 with all bytes
        // zero (documented footgun: slot0 then decodes 0.0 °F = -17.8 °C). A
        // populated register always has non-zero threshold constants around
        // the measurements, so all-zero means "not populated", not "very cold".
        bool any_nonzero = false;
        for (uint8_t b : *data) {
          if (b) { any_nonzero = true; break; }
        }
        if (any_nonzero) {
          float f = parent_->odu_status1_meas_f_(*data, fld.idx);
          if (!std::isnan(f))
            value = fld.delta ? f                               // ΔF published raw (no device_class)
                              : ((f - 32.0f) * (5.0f / 9.0f));  // °F → °C
        }
      }
      break;  // at most one suffix matches
    }
  }

  // ODU register 3E01 (REG_ODU_3E_TEMPS, 2-stage/two-capacity family): int16
  // BE /16 °F slots. slot0 = outdoor ambient, slot1 = coil temp. Unpopulated
  // slots read 0x03FF and are rejected inside odu_3e_meas_f_. Absolute °F → °C.
  // On variable-speed units this register never arrives (FUNC 0x15 refused),
  // so this block is inert there and the table-03 block above feeds the same
  // sensors. Panel-validated on a 24ANA160A (issue #21).
  if (register_key == REG_ODU_3E_TEMPS) {
    if (sensor_type_ == "odu_outdoor_temp" || sensor_type_ == "odu_coil_temp") {
      uint8_t slot = (sensor_type_ == "odu_outdoor_temp") ? 0 : 1;
      auto *data = parent_->get_register(device_addr, REG_ODU_3E_TEMPS);
      if (data) {
        float f = parent_->odu_3e_meas_f_(*data, slot);
        if (!std::isnan(f))
          value = (f - 32.0f) * (5.0f / 9.0f);
      }
    }
  }

  // ODU register 3E02 (REG_ODU_3E_STAGE, 2-stage/two-capacity family): stage
  // byte >>1 → 0=off/1=low/2=high. The store holds both the thermostat's
  // commanded write (00/02/04) and the ODU's read reply (01/02/04); both
  // decode identically after >>1, so no write/read split is needed.
  if (register_key == REG_ODU_3E_STAGE && sensor_type_ == "odu_stage") {
    auto *data = parent_->get_register(device_addr, REG_ODU_3E_STAGE);
    if (data) {
      float s = parent_->odu_3e_stage_(*data);
      if (!std::isnan(s))
        value = s;
    }
  }

  // --- ZC register 0302 (REG_ZC_ZONE_STATUS) ---
  // 24-byte TLV: six entries [tag, id, val_hi, val_lo] in id order
  //   0x01 local-z1, 0x02 z2, 0x03 z3, 0x04 z4, 0x14 LAT, 0x1C HPT.
  // tag 0x01 = present (value valid); 0x04 = not installed (0x0000).
  // °F = uint16_BE / 16. Multi-ZC: system zone N maps to controller
  // zc_addr_for_zone_(N) and LOCAL id zc_local_id_for_zone_(N) (zones 5-8 are
  // local 1-4 on the secondary controller 0x61). LAT/HPT are thermistor ports
  // on the primary controller (0x60). Only react to the controller that owns
  // the wanted entry, and skip entries whose tag isn't present to avoid
  // publishing 0°F → -17.8°C for uninstalled sensors.
  if (register_key == REG_ZC_ZONE_STATUS) {
    uint8_t want_id = 0;
    uint8_t want_addr = 0;
    if (sensor_type_ == "zc_zone_temperature") {
      want_id = parent_->zc_local_id_for_zone_(zone_);
      want_addr = parent_->zc_addr_for_zone_(zone_);
    } else if (sensor_type_ == "zc_lat") {
      want_id = ZC_ID_LAT;      // 0x14
      want_addr = parent_->zc_addr_for_zone_(1);  // primary controller
    } else if (sensor_type_ == "zc_hpt") {
      want_id = ZC_ID_HPT;      // 0x1C
      want_addr = parent_->zc_addr_for_zone_(1);  // primary controller
    }
    if (want_id != 0 && device_addr == want_addr) {
      auto *data = parent_->get_register(device_addr, REG_ZC_ZONE_STATUS);
      if (data && data->size() == 24) {
        for (uint8_t e = 0; e + 3 < 24; e += 4) {
          if (data->at(e + 1) == want_id && data->at(e) == ZC_0302_TAG_PRESENT) {
            uint16_t raw = ((uint16_t) data->at(e + 2) << 8) | data->at(e + 3);
            float temp_f = (float) raw / ZC_TEMP_SCALE;
            value = (temp_f - 32.0f) * (5.0f / 9.0f);  // °F → °C for HA
            break;
          }
        }
      }
    }
  }

  // --- Cycle counters and runtime hours (registers 0310/0311) ---
  // Format: sequence of 4-byte entries: [key, b1, b2, b3]
  // where value = (b1 << 16) | (b2 << 8) | b3 (24-bit unsigned)
  //
  // IDU keys: 0x23=low_heat, 0x24=high_heat, 0x48=med_heat,
  //           0x2B=poweron, 0x2D=blower
  // ODU keys: 0x23=heat, 0x28=cool, 0x3C=defrost, 0x2B=poweron
  // _cycles = register 0310, _hours = register 0311
  if (register_key == REG_IDU_CYCLES || register_key == REG_IDU_RUNTIME ||
      register_key == REG_ODU_CYCLES || register_key == REG_ODU_RUNTIME) {
    struct KVMap { const char *suffix; uint16_t reg; uint8_t key; };
    static const KVMap kv_map[] = {
      // IDU cycles (0310)
      {"idu_low_heat_cycles",  REG_IDU_CYCLES,  0x23},
      {"idu_high_heat_cycles", REG_IDU_CYCLES,  0x24},
      {"idu_med_heat_cycles",  REG_IDU_CYCLES,  0x48},
      {"idu_poweron_cycles",   REG_IDU_CYCLES,  0x2B},
      {"idu_blower_cycles",    REG_IDU_CYCLES,  0x2D},
      // IDU hours (0311)
      {"idu_low_heat_hours",   REG_IDU_RUNTIME, 0x25},
      {"idu_high_heat_hours",  REG_IDU_RUNTIME, 0x26},
      {"idu_med_heat_hours",   REG_IDU_RUNTIME, 0x49},
      {"idu_poweron_hours",    REG_IDU_RUNTIME, 0x2C},
      {"idu_blower_hours",     REG_IDU_RUNTIME, 0x2E},
      // ODU cycles (0310)
      {"odu_heat_cycles",      REG_ODU_CYCLES,  0x23},
      {"odu_cool_cycles",      REG_ODU_CYCLES,  0x28},
      {"odu_defrost_cycles",   REG_ODU_CYCLES,  0x3C},
      {"odu_poweron_cycles",   REG_ODU_CYCLES,  0x2B},
      // ODU hours (0311)
      {"odu_heat_hours",       REG_ODU_RUNTIME, 0x25},
      {"odu_cool_hours",       REG_ODU_RUNTIME, 0x2A},
      {"odu_defrost_hours",    REG_ODU_RUNTIME, 0x3D},
      {"odu_poweron_hours",    REG_ODU_RUNTIME, 0x2C},
    };

    for (const auto &km : kv_map) {
      if (sensor_type_ == km.suffix) {
        auto *data = parent_->get_register(device_addr, km.reg);
        if (data && data->size() >= 4) {
          for (size_t i = 0; i + 3 < data->size(); i += 4) {
            if ((*data)[i] == km.key) {
              uint32_t val = ((uint32_t)(*data)[i+1] << 16) |
                             ((uint32_t)(*data)[i+2] << 8) |
                             (uint32_t)(*data)[i+3];
              value = (float) val;
              break;
            }
          }
        }
        break;
      }
    }
  }

  if (!std::isnan(value)) {
    publish_state(value);
  }
}

} // namespace infinitesp
} // namespace esphome
