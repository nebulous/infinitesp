#include "infinitesp_sensor.h"
#include <cstring>

namespace esphome {
namespace infinitesp {

void InfinitESPSensor::on_register_update(uint8_t device_addr, uint16_t register_key) {
  float value = NAN;

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

  // ODU IEEE754 float32 values from register 061f. Native °F for idx 1..5,
  // but 1..5 are DELTAS (superheat/subcooling ΔT), so convert ΔF→ΔC (×5/9,
  // no -32 offset). idx 6 is dimensionless, passed through.
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
        if (!std::isnan(fval)) {
          if (idx <= 5)
            value = fval * (5.0f / 9.0f);  // °F delta → °C delta (superheat/subcooling are ΔT, not absolute)
          else
            value = fval;  // float 6 is dimensionless
        }
      }
    }
  }

  // ODU register 0302: int16 BE / 16, always native °F. Convert to °C.
  // Field idx via accessor odu_status1_meas_f_(idx): 0=outdoor 1=coil 2=suction
  // 3=suction_superheat(ΔT) 4=indoor_amb 5=discharge. idx 3 is a delta (no -32);
  //   confirmed superheat (matches thermostat 16<->17°F display; 56-40=16°F).
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
        float f = parent_->odu_status1_meas_f_(*data, fld.idx);
        if (!std::isnan(f))
          value = fld.delta ? (f * (5.0f / 9.0f))            // ΔF → ΔC
                            : ((f - 32.0f) * (5.0f / 9.0f));  // °F → °C
      }
      break;  // at most one suffix matches
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
