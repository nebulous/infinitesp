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
  // Compressor RPM from register 0604 (first uint16 BE pair = current speed)
  if (register_key == REG_ODU_COMP_SPEED && sensor_type_ == "compressor_rpm") {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data) {
      float rpm = parent_->odu_compressor_rpm_(*data);
      if (!std::isnan(rpm))
        value = rpm;
    }
  }

  // ODU demand % / stage / modulation from register 0608
  if (register_key == REG_ODU_DEMAND &&
      (sensor_type_ == "odu_demand" || sensor_type_ == "odu_stage" || sensor_type_ == "odu_modulation")) {
    auto *data = parent_->get_register(device_addr, REG_ODU_DEMAND);
    if (data) {
      float v = NAN;
      if (sensor_type_ == "odu_demand")      v = parent_->odu_demand_(*data);
      else if (sensor_type_ == "odu_stage")   v = parent_->odu_stage_(*data);
      else if (sensor_type_ == "odu_modulation") v = parent_->odu_modulation_(*data);
      if (!std::isnan(v))
        value = v;
    }
  }

  // ODU setpoint from register 060b (byte 5 of payload, which is data[4])
  // ODU temperatures are always °F (int16 BE / 16 encoding) regardless of bus unit
  if (register_key == REG_ODU_SETPOINT && sensor_type_ == "odu_setpoint") {
    auto *data = parent_->get_register(device_addr, REG_ODU_SETPOINT);
    if (data) {
      float sp_f = parent_->odu_setpoint_f_(*data);
      if (!std::isnan(sp_f))
        value = (sp_f - 32.0f) * (5.0f / 9.0f);
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

  // ODU IEEE754 float32 values from register 061f — always native °F, convert to °C
  // Layout via accessor odu_float_(idx): idx 1..6 at offset 1+(idx-1)*4.
  //   1: superheat target  2: superheat actual  3: subcooling target
  //   4: subcooling actual 5: discharge superheat (all °F deltas)
  //   6: dimensionless constant
  if (register_key == REG_ODU_FLOATS && sensor_type_.rfind("odu_float_", 0) == 0) {
    auto *data = parent_->get_register(device_addr, REG_ODU_FLOATS);
    if (data) {
      int idx = sensor_type_[10] - '0';  // odu_float_N → N
      if (idx >= 1 && idx <= 6) {
        float fval = parent_->odu_float_(*data, idx);
        if (!std::isnan(fval)) {
          if (idx <= 5)
            value = (fval - 32.0f) * (5.0f / 9.0f);  // °F → °C
          else
            value = fval;  // float 6 is dimensionless
        }
      }
    }
  }

  // ODU register 0302: int16 BE / 16, always native °F — convert to °C
  // Field idx via accessor odu_status1_meas_f_(idx): 0=outdoor 1=coil 2=suction
  // 3=subcooling(ΔT) 4=indoor_amb 5=discharge. idx 3 is a delta (no -32).
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_outdoor_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 0);
      if (!std::isnan(f)) value = (f - 32.0f) * (5.0f / 9.0f);
    }
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_coil_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 1);
      if (!std::isnan(f)) value = (f - 32.0f) * (5.0f / 9.0f);
    }
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_suction_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 2);
      if (!std::isnan(f)) value = (f - 32.0f) * (5.0f / 9.0f);
    }
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_subcooling_degf_int") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 3);  // delta °F
      if (!std::isnan(f)) value = f * (5.0f / 9.0f);  // delta °F → delta °C
    }
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_indoor_ambient") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 4);
      if (!std::isnan(f)) value = (f - 32.0f) * (5.0f / 9.0f);
    }
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_discharge_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data) {
      float f = parent_->odu_status1_meas_f_(*data, 5);
      if (!std::isnan(f)) value = (f - 32.0f) * (5.0f / 9.0f);
    }
  }

  // --- ZC zone temperatures (register 0302, ZC device address 0x60) ---
  // Per-zone: [tag, id, value_hi, value_lo] where °F = uint16_BE / 16
  if (register_key == REG_ZC_ZONE_STATUS && sensor_type_ == "zc_zone_temperature") {
    auto *data = parent_->get_register(device_addr, REG_ZC_ZONE_STATUS);
    if (data && data->size() == 24 && zone_ >= 2 && zone_ <= 4) {
      uint8_t off_hi = 4 + (zone_ - 2) * 4 + 2;
      uint8_t off_lo = off_hi + 1;
      uint16_t raw = ((uint16_t) data->at(off_hi) << 8) | data->at(off_lo);
      float temp_f = (float) raw / ZC_TEMP_SCALE;
      value = (temp_f - 32.0f) * (5.0f / 9.0f);  // °F → °C for HA
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
