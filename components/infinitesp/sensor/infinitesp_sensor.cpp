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
    if (data && data->size() >= 3) {
      uint16_t rpm = ((uint16_t) data->at(1) << 8) | data->at(2);
      value = (float) rpm;
    }
  }

  // Airflow CFM from register 0316
  if (register_key == REG_IDU_CONFIG && sensor_type_ == "airflow_cfm") {
    auto *data = parent_->get_register(device_addr, REG_IDU_CONFIG);
    if (data && data->size() >= 6) {
      uint16_t cfm = ((uint16_t) data->at(4) << 8) | data->at(5);
      value = (float) cfm;
    }
  }

  // ODU (Outdoor Unit) passively snooped registers
  // Compressor RPM from register 0604 (first uint16 BE pair = current speed)
  if (register_key == REG_ODU_COMP_SPEED && sensor_type_ == "compressor_rpm") {
    auto *data = parent_->get_register(device_addr, REG_ODU_COMP_SPEED);
    if (data && data->size() >= 2) {
      uint16_t rpm = ((uint16_t) data->at(0) << 8) | data->at(1);
      value = (float) rpm;
    }
  }

  // ODU demand % from register 0608
  if (register_key == REG_ODU_DEMAND && sensor_type_ == "odu_demand") {
    auto *data = parent_->get_register(device_addr, REG_ODU_DEMAND);
    if (data && data->size() >= 7) {
      // data[3] = demand (0 or 100 observed)
      if (sensor_type_ == "odu_demand") {
        value = (float) data->at(3);
      } else if (sensor_type_ == "odu_stage") {
        value = (float) data->at(5);
      } else if (sensor_type_ == "odu_modulation") {
        value = (float) data->at(6);
      }
    }
  }

  // ODU setpoint from register 060b (byte 5 of payload, which is data[4])
  // ODU temperatures are always °F (int16 BE / 16 encoding) regardless of bus unit
  if (register_key == REG_ODU_SETPOINT && sensor_type_ == "odu_setpoint") {
    auto *data = parent_->get_register(device_addr, REG_ODU_SETPOINT);
    if (data && data->size() >= 5) {
      value = ((float) data->at(4) - 32.0f) * (5.0f / 9.0f);
    }
  }

  // ODU operating mode from register 0304 (byte 11 of payload, which is data[10])
  if (register_key == REG_ODU_STATUS3 && sensor_type_ == "odu_operating_mode") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS3);
    if (data && data->size() >= 11) {
      value = (float) data->at(10);
    }
  }

  // ODU IEEE754 float32 values from register 061f — always native °F, convert to °C
  // Layout (stored data offsets, after 3-byte register header removed):
  //   [0]    sub-register (always 0x00)
  //   [1..4]  float 1: constant ~7.5 (superheat target)
  //   [5..8]  float 2: drifting ~10.0 (actual superheat)
  //   [9..12] float 3: drifting ~14.0 (subcooling target?)
  //   [13..16] float 4: drifting ~12.0 (actual subcooling?)
  //   [17..20] float 5: dynamic -68 to +17 (discharge superheat?)
  //   [21..24] float 6: constant ~0.039 (dimensionless)
  if (register_key == REG_ODU_FLOATS && sensor_type_.rfind("odu_float_", 0) == 0) {
    auto *data = parent_->get_register(device_addr, REG_ODU_FLOATS);
    if (data && data->size() >= 25) {
      int idx = sensor_type_[10] - '0';  // odu_float_N → N
      if (idx >= 1 && idx <= 6) {
        size_t offset = 1 + (idx - 1) * 4;
        float fval = parent_->decode_f32_be_(*data, offset);
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
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_outdoor_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 4)
      value = (parent_->decode_int16_f_(*data, 2) - 32.0f) * (5.0f / 9.0f);
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_coil_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 8)
      value = (parent_->decode_int16_f_(*data, 6) - 32.0f) * (5.0f / 9.0f);
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_suction_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 12)
      value = (parent_->decode_int16_f_(*data, 10) - 32.0f) * (5.0f / 9.0f);
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_subcooling_degf_int") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 16)
      value = (parent_->decode_int16_f_(*data, 14)) * (5.0f / 9.0f);  // delta °F → delta °C
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_indoor_coil_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 20)
      value = (parent_->decode_int16_f_(*data, 18) - 32.0f) * (5.0f / 9.0f);
  }
  if (register_key == REG_ODU_STATUS1 && sensor_type_ == "odu_discharge_temp") {
    auto *data = parent_->get_register(device_addr, REG_ODU_STATUS1);
    if (data && data->size() >= 24)
      value = (parent_->decode_int16_f_(*data, 22) - 32.0f) * (5.0f / 9.0f);
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
