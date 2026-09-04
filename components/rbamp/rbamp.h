#pragma once

#include <string>

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
// NVS-persisted Wh totals. v1.5 extends [3]→[7] for senior SKUs UI5/UI7.
// Layout change is a schema migration — legacy 3-channel data is read as-is
// (first 3 slots), ch3-6 initialize to zero on first v1.5 boot (fresh flash
// prefs slot allocated for the v1.5 schema).
struct RbAmpPrefData {
  double energy_total_wh[7];
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
    // v1.6: 16-channel reserve; codegen already length-caps at 16 slots.
    // Runtime apply path guards on n_channels_ and only writes what the
    // detected variant supports — a stray slot past variant N is a no-op
    // with a warning line, not a crash.
    if (ch < 16) ct_models_request_[ch] = code;
  }
  void set_sensor_class(uint8_t cls) { sensor_class_request_ = cls; }
  void set_bidirectional(bool b) { bidirectional_ = b; }
  void set_broadcast_latch(bool b) { broadcast_latch_ = b; }
  void set_address_change_request(uint8_t new_addr) { new_addr_request_ = new_addr; }
  void set_topology(uint8_t t) { topology_hint_ = static_cast<Topology>(t); }

  // v1.3 fleet / multi-module configuration
  void set_fleet_gc_enable(bool b) { fleet_gc_enable_request_ = b ? 1 : 0; }
  void set_group_id(uint8_t g) { group_id_request_ = g; group_id_request_valid_ = true; }

  // v1.4 — LATCH cadence decoupled from RT-read cadence. When set (>0), the
  // component only sends CMD_LATCH_PERIOD every `latch_interval_ms` while
  // still reading the RT block every `update_interval` (V/I/P/PF/Q refresh
  // stays fast, Wh integration + period reads slow down). When 0 (unset in
  // YAML), the v1.3 single-cadence behaviour applies (LATCH each update()).
  void set_latch_interval_ms(uint32_t ms) { latch_interval_ms_ = ms; }

  // GC broadcast LATCH — public API (callable from YAML lambda / service in
  // multi-module deployments where one instance acts as bus coordinator).
  // Transmits 5-byte canonical frame `A5 27 <group> <tick_lo> <tick_hi>` to
  // I²C address 0x00 (general call). Returns true on bus write success;
  // does NOT verify slave uptake — call gc_witness_check_() after the
  // SETTLE_MS_LATCH_PERIOD + 300 ms settle to verify uptake via PERIOD_VALID.
  // Pre-conditions (logged + skipped if violated):
  //   - REG_CAPABILITY bit CAP_GC_LATCH set on at least one expected slave
  //   - REG_FLEET_CONFIG.bit0 set device-side (call apply_fleet_config_ once)
  bool transmit_gc_frame();

  // Read-back: returns the last GC tick this instance saw (from REG_GC_TICK).
  // 0xFFFF = never received. Useful for fleet-side correlation in HA.
  uint16_t read_gc_tick_received();

  // S5 bench-test helpers — public read accessors for identity/diagnostic
  // surfaces, callable from YAML lambda. Each does a fresh read at call time.
  uint8_t get_firmware_version() const { return firmware_version_; }
  uint16_t get_capability_cached() const { return cached_capability_; }
  std::string get_variant_str();           // REG_HW_VARIANT → "UI1"/"UI2"/"UI3"/"I1"/"I2"/"I3"/"UNK"
  std::string get_capability_hex();        // "0x0FF"
  std::string get_uid_hex();               // 96-bit hex
  std::string get_last_error_str();        // REG_ERROR decoded
  uint8_t get_event_flags();               // REG_EVENT_FLAGS u8 (caller masks bits)
  bool write_clear_error();                // CMD_CLEAR_ERROR opcode
  bool write_reset();                      // CMD_RESET opcode
  bool fleet_apply_now();                  // re-run apply_fleet_config_ (from service)

  // Sensor setters — single-phase topology fields
  void set_voltage_sensor(sensor::Sensor *s) { voltage_[0] = s; }
  void set_current_sensor(sensor::Sensor *s) { current_[0] = s; }
  void set_current_1_sensor(sensor::Sensor *s) { current_[1] = s; }
  void set_current_2_sensor(sensor::Sensor *s) { current_[2] = s; }
  void set_current_3_sensor(sensor::Sensor *s) { current_[3] = s; }
  void set_current_4_sensor(sensor::Sensor *s) { current_[4] = s; }
  void set_current_5_sensor(sensor::Sensor *s) { current_[5] = s; }
  void set_current_6_sensor(sensor::Sensor *s) { current_[6] = s; }
  void set_power_sensor(sensor::Sensor *s) { power_[0] = s; }
  void set_power_1_sensor(sensor::Sensor *s) { power_[1] = s; }
  void set_power_2_sensor(sensor::Sensor *s) { power_[2] = s; }
  void set_power_3_sensor(sensor::Sensor *s) { power_[3] = s; }
  void set_power_4_sensor(sensor::Sensor *s) { power_[4] = s; }
  void set_power_5_sensor(sensor::Sensor *s) { power_[5] = s; }
  void set_power_6_sensor(sensor::Sensor *s) { power_[6] = s; }
  void set_energy_sensor(sensor::Sensor *s) { energy_[0] = s; }
  void set_energy_1_sensor(sensor::Sensor *s) { energy_[1] = s; }
  void set_energy_2_sensor(sensor::Sensor *s) { energy_[2] = s; }
  void set_energy_3_sensor(sensor::Sensor *s) { energy_[3] = s; }
  void set_energy_4_sensor(sensor::Sensor *s) { energy_[4] = s; }
  void set_energy_5_sensor(sensor::Sensor *s) { energy_[5] = s; }
  void set_energy_6_sensor(sensor::Sensor *s) { energy_[6] = s; }
  void set_energy_exported_sensor(sensor::Sensor *s) { energy_exported_[0] = s; }
  void set_energy_exported_1_sensor(sensor::Sensor *s) { energy_exported_[1] = s; }
  void set_energy_exported_2_sensor(sensor::Sensor *s) { energy_exported_[2] = s; }
  void set_power_factor_sensor(sensor::Sensor *s) { pf_[0] = s; }
  void set_power_factor_1_sensor(sensor::Sensor *s) { pf_[1] = s; }
  void set_power_factor_2_sensor(sensor::Sensor *s) { pf_[2] = s; }
  void set_power_factor_3_sensor(sensor::Sensor *s) { pf_[3] = s; }
  void set_power_factor_4_sensor(sensor::Sensor *s) { pf_[4] = s; }
  void set_power_factor_5_sensor(sensor::Sensor *s) { pf_[5] = s; }
  void set_power_factor_6_sensor(sensor::Sensor *s) { pf_[6] = s; }
  void set_reactive_power_sensor(sensor::Sensor *s) { reactive_power_[0] = s; }
  void set_reactive_power_1_sensor(sensor::Sensor *s) { reactive_power_[1] = s; }
  void set_reactive_power_2_sensor(sensor::Sensor *s) { reactive_power_[2] = s; }
  void set_reactive_power_3_sensor(sensor::Sensor *s) { reactive_power_[3] = s; }
  void set_reactive_power_4_sensor(sensor::Sensor *s) { reactive_power_[4] = s; }
  void set_reactive_power_5_sensor(sensor::Sensor *s) { reactive_power_[5] = s; }
  void set_reactive_power_6_sensor(sensor::Sensor *s) { reactive_power_[6] = s; }
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

  // v1.5 — Channel access window helper. Reads a float32 field for a given
  // channel via select-and-read protocol: writes REG_CHANNEL_SELECT with
  // (field<<4)|ch, then reads 4 bytes at REG_CHANNEL_DATA (float LE).
  // MUST re-select on every call — the data buffer is a snapshot at select
  // time; repeat reads without re-select return the same stale snapshot.
  // Public field codes: CHFIELD_I_RMS / _I_PEAK / _P_REAL / _PF / _PERIOD_AVG_P.
  bool read_channel_window_float_(uint8_t field, uint8_t ch, float *out);

  // v1.6 — Channel access window u8 write (CT_MODEL uses this). Sequence:
  //   1. write REG_CHANNEL_SELECT = (field<<4)|ch
  //   2. write REG_CHANNEL_DATA+0 = val
  //   3. write REG_CHANNEL_DATA+1 = 0    (upper bytes zero-padded — burst not supported)
  //   4. write REG_CHANNEL_DATA+2 = 0
  //   5. write REG_CHANNEL_DATA+3 = 0
  // Firmware applies the write on selector match. Unknown (class,model) pairs
  // return ERR_PARAM 0xFE and the channel keeps its previous value —
  // caller MUST read back via read_channel_window_u8_lo_ to confirm.
  bool write_channel_window_u8_(uint8_t field, uint8_t ch, uint8_t val);

  // v1.6 — Channel access window u8 read (low byte of the 4-byte data word).
  // Convenience wrapper around read_u32_le_ that extracts the low byte —
  // used for CT_MODEL readback verification after write.
  bool read_channel_window_u8_lo_(uint8_t field, uint8_t ch, uint8_t *out);

  // High-level operations
  bool probe_slave_();
  void detect_variant_();
  bool check_factory_mode_(const char *op_name);
  void apply_sensor_class_();
  void apply_ct_model_();
  void apply_ct_models_per_channel_();
  // v1.6 CT_MODEL per-channel router (LESSONS L9 single-site guard).
  // Both write helpers apply IN RAM ONLY — caller must issue a single
  // save_user_config_() after the apply cycle to persist the whole
  // ct_model_ch page. Flat-path (ch<3) for the 0x51-0x53 mirror set,
  // window-path (ch>=3) via CHFIELD_CT_MODEL for senior channels.
  bool ct_model_write_flat_(uint8_t ch, uint8_t code);
  bool ct_model_write_via_window_(uint8_t ch, uint8_t code);
  bool ct_model_read_(uint8_t ch, uint8_t *out);
  void apply_address_change_();
  void apply_fleet_config_();
  bool check_capability_(uint16_t bit);
  bool gc_witness_check_();
  void start_latch_phase_();
  void finish_latch_phase_(uint32_t t_latch);
  void publish_rt_block_();
  void load_prefs_();
  void save_prefs_();
  // S4: capability-gated CMD_SAVE_USER_CONFIG (v1.3) vs CMD_SAVE_GAINS (legacy)
  // selector. Picks the v1.3 opcode when CAP_SAVE_USER_CONFIG is advertised,
  // falls back to SAVE_GAINS otherwise. All settle to 700 ms either way.
  bool save_user_config_();
  // S4: per-quantity sanity bounds. Replaces the blanket |val|<10000 cap with
  // per-physical-quantity limits that pass through brownouts (U=0V) and
  // off-grid scenarios while still rejecting IDF buffer-leak ghost bytes.
  static bool sanity_voltage_(float v) { return std::isfinite(v) && v >= 0.0f && v <= 500.0f; }
  static bool sanity_current_(float i) { return std::isfinite(i) && i >= 0.0f && i <= 200.0f; }
  static bool sanity_power_(float p) { return std::isfinite(p) && p >= -50000.0f && p <= 50000.0f; }
  static bool sanity_pf_(float f) { return std::isfinite(f) && f >= -1.0f && f <= 1.0f; }
  static bool sanity_freq_(uint8_t f) { return f >= 40 && f <= 70; }

  // Configuration state
  GPIOPin *drdy_pin_{nullptr};
  uint8_t ct_model_{0xFF};                 // global CT model (legacy / v1.0-compat path)
  // Per-channel CT model. v1.6: 16-slot reserve — window selector nibble is
  // 4 bits so the wire protocol allows ch 0..15. Runtime apply path only
  // touches slots [0..n_channels_-1] detected from REG_TOPOLOGY, so extra
  // YAML slots on a smaller variant are ignored with a warning.
  uint8_t ct_models_request_[16]{
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t sensor_class_request_{0xFF};     // 0xFF = "not requested in YAML"; codegen sets to enum value (UNSET=0..BUILTIN_CT=3)
  bool bidirectional_{false};
  bool broadcast_latch_{false};
  uint8_t new_addr_request_{0};

  // v1.3 fleet state. fleet_gc_enable_request_: 0=disabled, 1=enabled,
  // 0xFF=not requested in YAML (skip apply_fleet_config_). group_id_request_:
  // 0 by default (all-call); group_id_request_valid_ distinguishes "default 0"
  // from "explicitly set to 0" so we only issue REG_GROUP_ID write when
  // requested.
  uint8_t fleet_gc_enable_request_{0xFF};
  uint8_t group_id_request_{0};
  bool group_id_request_valid_{false};
  uint16_t gc_tick_counter_{0};            // monotonic tick allocator (per master instance)
  uint16_t cached_capability_{0};          // cached REG_CAPABILITY u16, populated by detect_variant_
  bool capability_cached_{false};

  // v1.4 LATCH cadence decoupling. 0 = use PollingComponent::get_update_interval()
  // (v1.3 single-cadence behaviour). Positive = LATCH-only cadence in ms;
  // RT-block reads stay at update_interval. `last_latch_wall_ms_` tracks the
  // last completed LATCH tick for gating (separate from per-channel
  // `last_latch_ms_[ch]` which is the Wh-integration dt base).
  uint32_t latch_interval_ms_{0};
  uint32_t last_latch_wall_ms_{0};

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

  // Master-side energy accumulators (master clock × avg_P_W / 3600 → Wh).
  // Per-channel `primed_[ch]` distinguishes "first cycle, prime the clock,
  // do not integrate" from "subsequent cycle, integrate over t_now -
  // last_latch_ms_[ch]". Using bool sentinels (not "last_latch_ms_[ch] != 0")
  // survives the 49-day millis() wrap. Per-channel state is required for
  // mixed-CT (UI3) partial-fail recovery: one channel's CT can clock-stretch
  // beyond timeout independently of the others; healthy channels continue
  // integrating without waiting on the unhealthy one.
  // v1.5: extended to 7 slots for senior SKUs UI5 (5 ch) / UI7 (7 ch) — ch3-6
  // integrate from PERIOD_AVG_P via channel-access window (CHFIELD_PERIOD_AVG_P
  // = 13) after CMD_LATCH_PERIOD, same wall-clock dt integration as ch0-2.
  bool primed_[7]{false, false, false, false, false, false, false};
  uint32_t last_latch_ms_[7]{0, 0, 0, 0, 0, 0, 0};
  double energy_total_wh_[7]{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  // energy_export_wh_ scoped to primary channel only (bidirectional feature
  // for UI-class primary sense; senior SKUs use energy_total_wh_ for all ch).
  double energy_export_wh_[3]{0.0, 0.0, 0.0};

  // NVS persistence — Wh totals survive reboot. Save every SAVE_INTERVAL_MS
  // (5 min default) and on every full-channel-successful integration. Throttle
  // is per-call rate-limit to manage flash wear (NVS sector erase = ~10 ms).
  ESPPreferenceObject energy_pref_;
  uint32_t last_save_ms_{0};
  static constexpr uint32_t SAVE_INTERVAL_MS = 300000;  // 5 minutes

  // Sensors — slots for single-phase per-channel (v1.5: extended to 7 for
  // senior SKUs UI5/UI7). voltage_ + energy_exported_ remain [3] (max 3 phases
  // in SPLIT/THREE_PHASE topology; senior SKUs have one U + N I currents).
  sensor::Sensor *voltage_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *current_[7]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  sensor::Sensor *power_[7]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  sensor::Sensor *energy_[7]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  sensor::Sensor *energy_exported_[3]{nullptr, nullptr, nullptr};
  sensor::Sensor *pf_[7]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  sensor::Sensor *reactive_power_[7]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  sensor::Sensor *apparent_power_{nullptr};
  sensor::Sensor *frequency_{nullptr};
  sensor::Sensor *power_total_{nullptr};
};

}  // namespace rbamp
}  // namespace esphome
