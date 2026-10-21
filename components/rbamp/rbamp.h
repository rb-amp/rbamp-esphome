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

// v1.8 — second NVS slot for the grand-total / reactive accumulators fed by
// the on-device energy counters (STD line, CAPABILITIES bit13/15). Kept
// separate from RbAmpPrefData so existing per-channel Wh survive the upgrade
// untouched (no layout migration); fresh installs start these at 0.
struct RbAmpStdEnergyPrefData {
  double grand_import_wh;
  double grand_export_wh;
  double rea_import_varh;
  double rea_export_varh;
} __attribute__((packed));

static constexpr uint32_t RBAMP_STD_ENERGY_PREF_VERSION = 0x52425331;  // "RBS1"

class RbAmpComponent : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  // Clean-shutdown Wh flush (reboot / OTA): bounds the NVS loss window to the
  // throttled cadence only for power loss, not for every deliberate reboot.
  void on_shutdown() override { this->save_prefs_(); }
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
  void set_ct_model_force(bool f) { ct_model_force_ = f; }
  void set_bidirectional(bool b) { bidirectional_ = b; }
  void set_broadcast_latch(bool b) { broadcast_latch_ = b; }
  void set_address_change_request(uint8_t new_addr) { new_addr_request_ = new_addr; }
  void set_topology(uint8_t t) { topology_hint_ = static_cast<Topology>(t); }

  // v1.8 (STD line, fw 0x7B+) — starting current for energy accounting,
  // CFG_START_UA in µA. While a channel's current is below this value its
  // energy does not count into import/export; instantaneous readings are
  // unaffected. 0 = off; 10 µA steps up to 2540 (schema-validated).
  // 0xFFFF sentinel = key absent in YAML.
  void set_start_current_ua(uint16_t ua) { start_current_ua_request_ = ua; }

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
  // v1.8 STD accessors (lambda / template-sensor friendly)
  uint32_t get_std_image_version() const { return std_fw_ver_; }  // running image (0 = unknown / BASIC)
  std::string get_phase_sequence_str();    // "n/a"/"forward"/"reverse"/"abnormal"/"unavailable"
  uint16_t read_start_current_ua();        // fresh CFG_START_UA read; 0xFFFF = unavailable
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

  // v1.8 — fed by on-device energy counters (STD line). Grand totals cover
  // ALL device channels (also ones without declared per-channel sensors).
  void set_energy_total_sensor(sensor::Sensor *s) { energy_grand_import_ = s; }
  void set_energy_exported_total_sensor(sensor::Sensor *s) { energy_grand_export_ = s; }
  void set_energy_net_sensor(sensor::Sensor *s) { energy_grand_net_ = s; }
  void set_reactive_energy_sensor(sensor::Sensor *s) { reactive_energy_import_ = s; }
  void set_reactive_energy_exported_sensor(sensor::Sensor *s) { reactive_energy_export_ = s; }
  // v1.8 — voltage phase angles vs phase A (U3I3; fw 0x7A+) + phase sequence.
  void set_voltage_angle_b_sensor(sensor::Sensor *s) { voltage_angle_[1] = s; }
  void set_voltage_angle_c_sensor(sensor::Sensor *s) { voltage_angle_[2] = s; }
  void set_phase_sequence_sensor(sensor::Sensor *s) { phase_sequence_ = s; }

 protected:
  // ==========================================================================
  // STD line (16-bit register pointer) — v1.7 extension, NOT a fork.
  //
  // The component supports BOTH product lines on the same YAML surface:
  //   * BASIC — 8-bit register pointer, float32 registers, no auto-increment
  //     (everything below this block; unchanged).
  //   * STD   — 16-bit big-endian register pointer with auto-increment burst
  //     reads, fixed-point little-endian data (see rbamp_std_registers.h).
  //
  // Line detection happens ONCE in setup() and MUST run before any 8-bit
  // transaction: an STD slave takes a lone pointer byte as the pointer HIGH
  // byte and answers from 0xNN00 with plausible non-zero values, so the BASIC
  // VERSION probe would false-pass on an STD module. The STD identity probe
  // is benign on BASIC (the pointer pair lands as "reg 0x00 <- 0x00", a write
  // to the read-only STATUS byte that BASIC firmware drops; the burst read
  // returns its control block, which can never spell the 'S','T' magic).
  //
  // Once std_ is true, ALL device I/O goes through the std_* helpers — the
  // 8-bit helpers/appliers below are never called on an STD handle.
  // ==========================================================================

  // Burst read of n bytes from 16-bit address reg (BE pointer write +
  // repeated-start read, chunked at 32 bytes, per-chunk NACK retry).
  bool std_read_(uint16_t reg, uint8_t *buf, size_t n);
  // v1.8 — coherent read: ONE bus transaction (no chunking, n ≤ 128), whole-
  // read NACK retry. The energy counter snapshots must not tear between
  // chunks (import vs export of the same instant), so they bypass the
  // 32-byte chunker. Family canon: coherent reads up to 128 bytes.
  bool std_read_coherent_(uint16_t reg, uint8_t *buf, size_t n);
  bool std_write_u16_(uint16_t reg, uint16_t val);  // LE data, NACK retry
  // Identity probe of the 10-byte block 0x0000-0x0009. Returns true when the
  // 'S','T' magic answered (STD line — BASIC setup must NOT run; on an
  // unsupported map version the component is marked failed). Returns false
  // for "not STD" (BASIC probe proceeds and owns error reporting).
  bool std_probe_();
  void std_setup_();    // adopt discovery counts (N_VOLTAGE/N_CURRENT/VARIANT)
  void std_update_();   // realtime poll: U/I/P/Q/S/PF/freq, fixed-point → SI
  const char *std_variant_str_() const;

  // Single-byte write to a 16-bit address (BE pointer + data, NACK retry).
  bool std_write_u8_(uint16_t reg, uint8_t val);
  // Opcode to COMMAND + settle + sync ERROR verify. On map 0x34+ the module
  // clears ERROR when it dispatches a command, so ERROR after the settle is
  // the verdict for THIS command (0x00 OK / 0xFD PARAM / 0xFE UNKNOWN_CMD).
  // On older maps ERROR is sticky and says nothing — verify is skipped.
  bool std_cmd_verify_(uint8_t opcode, uint16_t settle_ms, const char *op_name);
  // Config writes (sensor class / CT model / address) via the CFG block +
  // CMD_SAVE_CONFIG (RMW device-side, calibration preserved).
  void std_apply_config_();
  void std_apply_address_change_();
  // Latch bank 0 → non-blocking 300 ms settle (the latch is deferred to the
  // module's next RT-window boundary; PD_VALID is sticky, NOT a freshness
  // flag — wall-clock wait is the contract) → per-channel Wh integration
  // from (E_pos, E_neg, NSAMP) with MASTER wall-clock dt (the module has no
  // clock at all on this line).
  void std_start_latch_phase_();
  void std_finish_latch_phase_(uint32_t t_latch);
  // v1.8 — on-device energy counters (CAPABILITIES bit13 ONDEVICE_ENERGY /
  // bit15 ENERGY_UWH). Replaces the latch-integration path entirely on
  // capable firmware: counters are module-persisted, so a master reboot
  // loses nothing and no arm/re-arm dance is needed. Master accumulates
  // counter DELTAS into the NVS totals (published values stay monotonic
  // across module restart step-back and LT generation resets).
  void std_energy_counters_update_();
  void load_std_energy_prefs_();

  // STD identity, captured by std_probe_ from the discovery burst.
  bool std_{false};
  uint8_t std_version_{0};   // REG_VERSION (map layout; gate features on it)
  uint16_t std_caps_{0};     // REG_CAPABILITIES bitmap
  uint8_t std_status_{0};    // REG_STATUS at probe time
  uint8_t std_variant_{0};   // REG_VARIANT family enum
  uint8_t n_voltage_{0};     // REG_N_VOLTAGE (0 on I-only variants)
  uint8_t std_n_current_{0}; // REG_N_CURRENT raw (setup clamps to sensor slots)

  // v1.8 STD state
  uint32_t std_fw_ver_{0};        // REG_FW_VER (running image, u32); 0 = unknown
  bool std_angles_ok_{false};     // fw >= 0x0001007A: ANGLE / PHASE_SEQ meaningful
  bool std_ode_{false};           // on-device energy counters in use
  bool std_ode_fine_{false};      // µWh blocks (bit15), else mWh coarse
  bool std_ode_fine_rea_{false};  // µvarh blocks: bit15 AND image ≥ 0x0001007B (read 0 on the 0x..7A image)
  uint16_t start_current_ua_request_{0xFFFF};

  // Counter-delta state. Baselines are µ-normalized (coarse reads are scaled
  // up on ingest: mWh×1000, varh×1e6) so the delta path is unit-uniform.
  bool std_cnt_primed_{false};
  uint32_t std_cnt_last_ms_{0};
  uint32_t std_prev_gen_{0};      // REG_LT_GEN — change => counters re-based, no delta
  uint32_t std_prev_uptime_{0};   // ENERGY_UPTIME — decrease => module restart
  int64_t std_prev_grand_imp_{0}, std_prev_grand_exp_{0};        // µWh
  int64_t std_prev_rea_imp_{0}, std_prev_rea_exp_{0};            // µvarh
  int64_t std_prev_ch_imp_[7]{0}, std_prev_ch_exp_[7]{0};        // µWh

  // Grand accumulators (NVS "RBS1" slot). NET is derived as imp − exp at
  // publish time — never from the NET counter (clamping its delta would skew).
  double std_grand_import_wh_{0.0}, std_grand_export_wh_{0.0};
  double std_rea_import_varh_{0.0}, std_rea_export_varh_{0.0};
  ESPPreferenceObject std_energy_pref_;

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

  // SCT rating switch (family spec SCT_RATING_SWITCH.md, BASIC + SENSOR_CLASS
  // 1 only): every SCT batch is factory-calibrated on the -030 model, so when
  // the channel model is switched the master multiplies everything
  // proportional to the current gain by s = preset(X)·c(X)/preset(3),
  // computed against the gain the module actually applies (decision logic in
  // rbamp_sct_rating.h, host-tested). Runs at setup after the model appliers
  // and again whenever the module uptime goes backwards (module restart).
  void sct_rating_update_();
  // Applied per-channel gain, readable in user mode (flat float32 regs for
  // ch0-2, window field 5 for ch3+). The component never writes it.
  bool read_applied_gain_(uint8_t ch, float *out);
  // Spec reset flow: capture UID → CMD_RESET → settle → re-probe → compare
  // UID (guards against another module answering after an address collision).
  bool basic_reset_verify_uid_();
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
  // Service option: CT model writes to a factory-paired module (device
  // sensor class WIRED_CT/BUILTIN_CT) replace its individual calibration
  // with the generic preset, so they are refused unless this is set.
  // With force, the write proceeds even on an unchanged model (deliberate
  // preset re-apply during service).
  bool ct_model_force_{false};
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
  // SCT rating-switch correction state (BASIC, SENSOR_CLASS 1). Scales apply
  // to I/P/Q and the period avg_p energy integration; U and PF never scale.
  float sct_scale_[7]{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
  bool sct_enabled_{false};            // class-1 module: uptime restart watch on
  bool ct_model_written_this_boot_{false};  // model applier wrote+saved → spec reset flow
  uint32_t basic_prev_uptime_s_{0};    // 0 = not primed yet
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
  // v1.8 — STD on-device energy + 3φ angle surfaces (never publish on BASIC).
  sensor::Sensor *energy_grand_import_{nullptr};
  sensor::Sensor *energy_grand_export_{nullptr};
  sensor::Sensor *energy_grand_net_{nullptr};
  sensor::Sensor *reactive_energy_import_{nullptr};
  sensor::Sensor *reactive_energy_export_{nullptr};
  sensor::Sensor *voltage_angle_[3]{nullptr, nullptr, nullptr};  // [0] unused (phase A is the reference)
  sensor::Sensor *phase_sequence_{nullptr};

  // Bounds contract behind the loop guards: n_channels_ reaches 7 (UI5/UI7,
  // STD clamp), so every array a `ch < n_channels_` loop may index must hold
  // 7 slots; the [3] arrays (voltage / export / angle) are only reachable
  // through an explicit `< 3` clamp at the index site. If a size here
  // changes, re-audit every index site before relaxing the assert.
  static_assert(sizeof(current_) == 7 * sizeof(sensor::Sensor *) &&
                sizeof(power_) == 7 * sizeof(sensor::Sensor *) &&
                sizeof(energy_) == 7 * sizeof(sensor::Sensor *) &&
                sizeof(pf_) == 7 * sizeof(sensor::Sensor *) &&
                sizeof(reactive_power_) == 7 * sizeof(sensor::Sensor *) &&
                sizeof(energy_total_wh_) == 7 * sizeof(double) &&
                sizeof(sct_scale_) == 7 * sizeof(float) &&
                sizeof(primed_) == 7 && sizeof(last_latch_ms_) == 7 * sizeof(uint32_t) &&
                sizeof(std_prev_ch_imp_) == 7 * sizeof(int64_t) &&
                sizeof(std_prev_ch_exp_) == 7 * sizeof(int64_t),
                "per-channel arrays must cover n_channels_ max (7)");
  static_assert(sizeof(voltage_) == 3 * sizeof(sensor::Sensor *) &&
                sizeof(energy_exported_) == 3 * sizeof(sensor::Sensor *) &&
                sizeof(voltage_angle_) == 3 * sizeof(sensor::Sensor *) &&
                sizeof(energy_export_wh_) == 3 * sizeof(double),
                "[3] arrays require a `< 3` clamp at every index site");
};

}  // namespace rbamp
}  // namespace esphome
