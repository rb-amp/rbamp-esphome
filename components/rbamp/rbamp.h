#pragma once

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace rbamp {

enum class Topology : uint8_t {
  SINGLE = 0,       // UI1/UI2/UI3, I1/I2/I3 — one voltage, 1..3 current channels
  SPLIT_PHASE = 1,  // U2I2 (US) — two voltages 180° apart, two current channels
  THREE_PHASE = 2,  // U3I3 — three voltages, three current channels
};

// NVS payload for energy persistence. Layout MUST stay trivially-copyable
// (POD). When layout changes, bump RBAMP_PREF_VERSION so stale data is
// discarded on load instead of producing garbage Wh totals.
struct RbAmpPrefData {
  double energy_total_wh[3];
  double energy_export_wh[3];
} __attribute__((packed));

static constexpr uint32_t RBAMP_PREF_VERSION = 0x52424D31;  // "RBM1"

class RbAmpComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // YAML-bound configuration setters
  void set_drdy_pin(GPIOPin *pin) { drdy_pin_ = pin; }
  void set_ct_model(uint8_t m) { ct_model_ = m; }
  void set_ct_model_ch(uint8_t ch, uint8_t code) {
    if (ch < 3) ct_models_request_[ch] = code;
  }
  void set_sensor_class(uint8_t cls) { sensor_class_request_ = cls; }
  void set_bidirectional(bool b) { bidirectional_ = b; }
  void set_broadcast_latch(bool b) { broadcast_latch_ = b; }
  void set_address_change_request(uint8_t new_addr) { new_addr_request_ = new_addr; }
  void set_topology(uint8_t t) { topology_hint_ = static_cast<Topology>(t); }

  // Sensor setters — single-phase topology fields
  void set_voltage_sensor(sensor::Sensor *s) { voltage_[0] = s; }
  void set_current_sensor(sensor::Sensor *s) { current_[0] = s; }
  void set_current_1_sensor(sensor::Sensor *s) { current_[1] = s; }
  void set_current_2_sensor(sensor::Sensor *s) { current_[2] = s; }
  void set_power_sensor(sensor::Sensor *s) { power_[0] = s; }
  void set_power_1_sensor(sensor::Sensor *s) { power_[1] = s; }
  void set_power_2_sensor(sensor::Sensor *s) { power_[2] = s; }
  void set_energy_sensor(sensor::Sensor *s) { energy_[0] = s; }
  void set_energy_1_sensor(sensor::Sensor *s) { energy_[1] = s; }
  void set_energy_2_sensor(sensor::Sensor *s) { energy_[2] = s; }
  void set_energy_exported_sensor(sensor::Sensor *s) { energy_exported_[0] = s; }
  void set_energy_exported_1_sensor(sensor::Sensor *s) { energy_exported_[1] = s; }
  void set_energy_exported_2_sensor(sensor::Sensor *s) { energy_exported_[2] = s; }
  void set_power_factor_sensor(sensor::Sensor *s) { pf_[0] = s; }
  void set_power_factor_1_sensor(sensor::Sensor *s) { pf_[1] = s; }
  void set_power_factor_2_sensor(sensor::Sensor *s) { pf_[2] = s; }
  void set_reactive_power_sensor(sensor::Sensor *s) { reactive_power_[0] = s; }
  void set_reactive_power_1_sensor(sensor::Sensor *s) { reactive_power_[1] = s; }
  void set_reactive_power_2_sensor(sensor::Sensor *s) { reactive_power_[2] = s; }
  void set_apparent_power_sensor(sensor::Sensor *s) { apparent_power_ = s; }
  void set_frequency_sensor(sensor::Sensor *s) { frequency_ = s; }

  // Sensor setters — phased topology fields (split-phase / three-phase, future SKU)
  void set_voltage_a_sensor(sensor::Sensor *s) { voltage_[0] = s; }
  void set_voltage_b_sensor(sensor::Sensor *s) { voltage_[1] = s; }
  void set_voltage_c_sensor(sensor::Sensor *s) { voltage_[2] = s; }
  void set_current_a_sensor(sensor::Sensor *s) { current_[0] = s; }
  void set_current_b_sensor(sensor::Sensor *s) { current_[1] = s; }
  void set_current_c_sensor(sensor::Sensor *s) { current_[2] = s; }
  void set_power_a_sensor(sensor::Sensor *s) { power_[0] = s; }
  void set_power_b_sensor(sensor::Sensor *s) { power_[1] = s; }
  void set_power_c_sensor(sensor::Sensor *s) { power_[2] = s; }
  void set_energy_a_sensor(sensor::Sensor *s) { energy_[0] = s; }
  void set_energy_b_sensor(sensor::Sensor *s) { energy_[1] = s; }
  void set_energy_c_sensor(sensor::Sensor *s) { energy_[2] = s; }
  void set_energy_exported_a_sensor(sensor::Sensor *s) { energy_exported_[0] = s; }
  void set_energy_exported_b_sensor(sensor::Sensor *s) { energy_exported_[1] = s; }
  void set_energy_exported_c_sensor(sensor::Sensor *s) { energy_exported_[2] = s; }
  void set_power_factor_a_sensor(sensor::Sensor *s) { pf_[0] = s; }
  void set_power_factor_b_sensor(sensor::Sensor *s) { pf_[1] = s; }
  void set_power_factor_c_sensor(sensor::Sensor *s) { pf_[2] = s; }
  void set_reactive_power_a_sensor(sensor::Sensor *s) { reactive_power_[0] = s; }
  void set_reactive_power_b_sensor(sensor::Sensor *s) { reactive_power_[1] = s; }
  void set_reactive_power_c_sensor(sensor::Sensor *s) { reactive_power_[2] = s; }
  void set_power_total_sensor(sensor::Sensor *s) { power_total_ = s; }

 protected:
  // Low-level I²C helpers — the device has no auto-increment, every byte needs a fresh address phase
  bool read_u8_(uint8_t reg, uint8_t *out);
  bool read_float_le_(uint8_t reg, float *out);
  bool read_u32_le_(uint8_t reg, uint32_t *out);
  bool write_u8_(uint8_t reg, uint8_t val);

  // High-level operations
  bool probe_slave_();
  void detect_variant_();
  bool check_develop_mode_(const char *op_name);
  void apply_sensor_class_();
  void apply_ct_model_();
  void apply_ct_models_per_channel_();
  void apply_address_change_();
  void start_latch_phase_();
  void finish_latch_phase_(uint32_t t_latch);
  void publish_rt_block_();
  void load_prefs_();
  void save_prefs_();

  // Configuration state
  GPIOPin *drdy_pin_{nullptr};
  uint8_t ct_model_{0xFF};                 // global CT model (legacy / v1.0-compat path)
  uint8_t ct_models_request_[3]{0xFF, 0xFF, 0xFF};  // per-channel CT model (v1.2+ path)
  uint8_t sensor_class_request_{0xFF};     // 0xFF = "not requested in YAML"; codegen sets to enum value (UNSET=0..BUILTIN_CT=3)
  bool bidirectional_{false};
  bool broadcast_latch_{false};
  uint8_t new_addr_request_{0};

  // User-supplied hint (YAML `topology:` key). Current firmware has no in-band
  // way to report channel count, so this is informational unless/until a future
  // firmware revision exposes a REG_TOPOLOGY byte.
  Topology topology_hint_{Topology::SINGLE};

  // Effective runtime state. `topology_` mirrors `topology_hint_` today; once
  // future firmware exposes a topology register, `detect_variant_()` will
  // prefer the device-reported value and the hint becomes a fallback.
  Topology topology_{Topology::SINGLE};
  uint8_t n_channels_{1};
  bool has_voltage_{true};
  uint8_t firmware_version_{0};

  // Master-side energy accumulators (master clock × avg_P_W / 3600 → Wh)
  // `primed_` distinguishes "first cycle, prime the clock, do not integrate"
  // from "subsequent cycle, integrate over t_now - last_latch_ms_". Using a
  // bool sentinel instead of "last_latch_ms_ != 0" survives millis() wrap.
  bool primed_{false};
  uint32_t last_latch_ms_{0};
  double energy_total_wh_[3]{0.0, 0.0, 0.0};
  double energy_export_wh_[3]{0.0, 0.0, 0.0};

  // NVS persistence — Wh totals survive reboot. Save every SAVE_INTERVAL_MS
  // (5 min default) and on every full-channel-successful integration. Throttle
  // is per-call rate-limit to manage flash wear (NVS sector erase = ~10 ms).
  ESPPreferenceObject energy_pref_;
  uint32_t last_save_ms_{0};
  static constexpr uint32_t SAVE_INTERVAL_MS = 300000;  // 5 minutes

  // Sensors — slot [0..2] maps to channels (SINGLE) or phases (SPLIT/THREE_PHASE)
  sensor::Sensor *voltage_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *current_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *power_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *energy_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *energy_exported_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *pf_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *reactive_power_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *apparent_power_{nullptr};
  sensor::Sensor *frequency_{nullptr};
  sensor::Sensor *power_total_{nullptr};
};

}  // namespace rbamp
}  // namespace esphome
