#include "rbamp.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace rbamp {

static const char *const TAG = "rbamp";

// Register addresses — mirror of the device's I²C register map.
// The device's firmware does NOT auto-increment the register pointer: every
// byte read requires a fresh address phase. read_float_le_ does this
// internally.
static constexpr uint8_t REG_STATUS              = 0x00;
static constexpr uint8_t REG_COMMAND             = 0x01;
static constexpr uint8_t REG_ERROR               = 0x02;
static constexpr uint8_t REG_VERSION             = 0x03;
static constexpr uint8_t REG_MODE                = 0x04;
static constexpr uint8_t REG_CT_MODEL            = 0x05;
static constexpr uint8_t REG_PHASE_SAMPLES       = 0x06;
static constexpr uint8_t REG_PERIOD_VALID        = 0x07;

static constexpr uint8_t REG_AC_FREQ             = 0x20;
static constexpr uint8_t REG_SENSOR_CLASS        = 0x25;  // v1.2+ firmware
static constexpr uint8_t REG_I2C_ADDRESS         = 0x30;

static constexpr uint8_t REG_U_RMS               = 0x86;
static constexpr uint8_t REG_U_PEAK              = 0x8A;
static constexpr uint8_t REG_I0_RMS              = 0x8E;
static constexpr uint8_t REG_I1_RMS              = 0x92;
static constexpr uint8_t REG_I2_RMS              = 0x96;
static constexpr uint8_t REG_P0_REAL             = 0xA6;
static constexpr uint8_t REG_P1_REAL             = 0xAA;
static constexpr uint8_t REG_P2_REAL             = 0xAE;
static constexpr uint8_t REG_PF0                 = 0xB2;
static constexpr uint8_t REG_PF1                 = 0xB6;
static constexpr uint8_t REG_PF2                 = 0xBA;
static constexpr uint8_t REG_PERIOD_AVG_P1       = 0xC2;
static constexpr uint8_t REG_PERIOD_AVG_P2       = 0xC6;
static constexpr uint8_t REG_DATA_VALID          = 0xCE;
static constexpr uint8_t REG_Q0                  = 0xD0;
static constexpr uint8_t REG_Q1                  = 0xD4;
static constexpr uint8_t REG_Q2                  = 0xD8;
static constexpr uint8_t REG_PERIOD_AVG_P0       = 0xDC;
static constexpr uint8_t REG_PERIOD_MAX_P        = 0xE0;
static constexpr uint8_t REG_PERIOD_LATCH_MS     = 0xEC;

static constexpr uint8_t CMD_RESET               = 0x01;
static constexpr uint8_t CMD_SAVE_GAINS          = 0x26;
static constexpr uint8_t CMD_LATCH_PERIOD        = 0x27;
// v1.2+ per-channel CT model selection commands. Write CMD_SET_CT_MODEL_CH<N>
// to REG_COMMAND, then write the model code to REG_CT_MODEL, then CMD_SAVE_GAINS.
static constexpr uint8_t CMD_SET_CT_MODEL_CH0    = 0x28;
static constexpr uint8_t CMD_SET_CT_MODEL_CH1    = 0x29;
static constexpr uint8_t CMD_SET_CT_MODEL_CH2    = 0x2A;

// STANDARD/PRO export accumulator — addresses TBD in firmware (J.10 deferred).
// 0xFF means "skip reading; field not supported by current firmware".
static constexpr uint8_t REG_PERIOD_AVG_P_NEG[3] = {0xFF, 0xFF, 0xFF};

// ============================================================================
// Low-level I2C helpers — every multi-byte field requires N separate transactions
// ============================================================================

bool RbAmpComponent::read_u8_(uint8_t reg, uint8_t *out) {
  // 3 attempts × 5 ms gap. The device intermittently NACKs reads at 100 kHz,
  // and IDF i2c_master leaks buffer state on NACK. Retry brings effective
  // failure rate to ~0.8%.
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (this->read_register(reg, out, 1) == i2c::ERROR_OK) {
      return true;
    }
    if (attempt < 2) {
      delay(5);
    }
  }
  return false;
}

bool RbAmpComponent::read_float_le_(uint8_t reg, float *out) {
  // Byte-by-byte read (the device has no register auto-increment) with
  // per-byte NACK retry. Root cause of the retries: the device intermittently
  // NACKs at ~20% rate on 100 kHz I²C; the ESP-IDF i2c_master driver leaks
  // read-buffer state on NACK, so a raw retry-less read can return a stale or
  // unrelated float bit-pattern that nonetheless passes `isfinite()`.
  // 3 attempts × 5 ms gap drops the per-byte failure rate from ~20% to ~0.8%,
  // and combined with the plausibility filter below achieves >99% clean
  // publishes.
  uint8_t buf[4];
  for (uint8_t i = 0; i < 4; i++) {
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 3 && !ok; ++attempt) {
      if (this->read_register(reg + i, &buf[i], 1) == i2c::ERROR_OK) {
        ok = true;
      } else if (attempt < 2) {
        delay(5);  // tiny gap — device needs to flush ADDR-phase state
      }
    }
    if (!ok) {
      return false;
    }
  }
  std::memcpy(out, buf, 4);
  // Loose sanity only — see SPEC §B.5 (cross-library discipline). NO physical
  // lower bounds: brownout (U=0 V on mains disconnect), voltage sag, off-grid
  // / UPS test, and breaker-trip events MUST pass through to HA so users see
  // their real state instead of stale values. NaN/Inf would poison the energy
  // accumulator permanently (NaN + x = NaN); the |val| < 10000 cap catches
  // Inf-adjacent ghost bytes that survive retry. The workhorse against I2C
  // NACK noise is the retry above + the 50 kHz bus speed in YAML, not this
  // filter. See baton 2026-05-24T19:00Z for the architectural rationale.
  if (!std::isfinite(*out)) {
    return false;
  }
  if (std::fabs(*out) > 10000.0f) {
    return false;
  }
  return true;
}

bool RbAmpComponent::read_u32_le_(uint8_t reg, uint32_t *out) {
  uint8_t buf[4];
  for (uint8_t i = 0; i < 4; i++) {
    if (this->read_register(reg + i, &buf[i], 1) != i2c::ERROR_OK) {
      return false;
    }
  }
  *out = (uint32_t) buf[0] | ((uint32_t) buf[1] << 8)
       | ((uint32_t) buf[2] << 16) | ((uint32_t) buf[3] << 24);
  return true;
}

bool RbAmpComponent::write_u8_(uint8_t reg, uint8_t val) {
  return this->write_register(reg, &val, 1) == i2c::ERROR_OK;
}

// ============================================================================
// High-level operations — stubs in m1
// ============================================================================

bool RbAmpComponent::probe_slave_() {
  uint8_t ver = 0;
  if (this->read_u8_(REG_VERSION, &ver)) {
    this->firmware_version_ = ver;
    return true;
  }

  // Self-healing: if the YAML requested an address change AND the slave is
  // silent at the current address, maybe the change has already been applied
  // in a previous boot. Try the requested target address before giving up.
  if (this->new_addr_request_ != 0 && this->new_addr_request_ != this->address_) {
    uint8_t orig_addr = this->address_;
    this->set_i2c_address(this->new_addr_request_);
    if (this->read_u8_(REG_VERSION, &ver)) {
      this->firmware_version_ = ver;
      ESP_LOGW(TAG,
               "Slave not at 0x%02X but answered at 0x%02X — address change "
               "appears to have been applied in a previous boot. "
               "Please update YAML: set `address: 0x%02X` and remove `new_address:`.",
               orig_addr, this->address_, this->address_);
      // Already on new address — skip the change-flow in setup().
      this->new_addr_request_ = 0;
      return true;
    }
    // Neither address responds — restore so the error message is informative.
    this->set_i2c_address(orig_addr);
  }

  ESP_LOGE(TAG, "Probe failed at 0x%02X — no response. Check wiring/address.",
           this->address_);
  return false;
}

void RbAmpComponent::detect_variant_() {
  // v1 firmware (REG_VERSION == 0x01) exposes no REG_TOPOLOGY byte and never
  // NACKs unmapped reads (see libs/spec/SPEC.md §8 + src/modules/i2c_driver.c:
  // unmapped reads return 0x00, indistinguishable from float +0.0). The
  // original NACK-probe approach over-detected channels — dropped.
  //
  // Topology comes from the YAML `topology:` hint (default SINGLE — matches
  // every shipping v1 SKU). When v1.1 firmware adds REG_TOPOLOGY, this branch
  // will read it preferentially and treat the hint as fallback.
  this->topology_ = this->topology_hint_;

  // Channel count and voltage presence are derived from which sensor slots
  // the user declared in YAML. The schema validator already enforces that
  // power_N/energy_N/etc. require current_N at the same slot and a voltage if
  // applicable, so this derivation is well-defined regardless of the topology
  // hint above.
  if (this->current_[2] != nullptr) {
    this->n_channels_ = 3;
  } else if (this->current_[1] != nullptr) {
    this->n_channels_ = 2;
  } else if (this->current_[0] != nullptr) {
    this->n_channels_ = 1;
  } else {
    this->n_channels_ = 0;  // voltage-only / frequency-only deployment
  }
  this->has_voltage_ = (this->voltage_[0] != nullptr);
}

void RbAmpComponent::load_prefs_() {
  // NVS slot key: FNV1a of a stable namespace string, xor'd with the I2C
  // address (multi-module configs get distinct slots on the same MCU) and
  // RBAMP_PREF_VERSION (layout-bump invalidates stale data instead of
  // misinterpreting it). Replaces the older `get_object_id_hash()` —
  // removed from PollingComponent in ESPHome 2025.x. Component object IDs
  // are typically anonymous in ESPHome (the sensors carry the names), so
  // address+namespace is a stable replacement that survives YAML renames.
  uint32_t hash = fnv1_hash("rbamp_energy") ^ static_cast<uint32_t>(this->address_)
                ^ RBAMP_PREF_VERSION;
  this->energy_pref_ = global_preferences->make_preference<RbAmpPrefData>(hash);

  RbAmpPrefData saved{};
  if (!this->energy_pref_.load(&saved)) {
    ESP_LOGCONFIG(TAG, "  No saved energy state — starting from 0 Wh");
    return;
  }

  // Validate before restoring — NaN/Inf in NVS would poison the accumulator
  // permanently (NaN + x = NaN), and negative totals are nonsensical for
  // consumption (cap to 0 rather than abort restore).
  for (uint8_t ch = 0; ch < 3; ch++) {
    double e = saved.energy_total_wh[ch];
    if (std::isfinite(e) && e >= 0.0) {
      this->energy_total_wh_[ch] = e;
    }
    double x = saved.energy_export_wh[ch];
    if (std::isfinite(x) && x >= 0.0) {
      this->energy_export_wh_[ch] = x;
    }
  }
  ESP_LOGCONFIG(TAG,
                "  Restored Wh from NVS: total[%.3f, %.3f, %.3f] export[%.3f, %.3f, %.3f]",
                this->energy_total_wh_[0], this->energy_total_wh_[1],
                this->energy_total_wh_[2], this->energy_export_wh_[0],
                this->energy_export_wh_[1], this->energy_export_wh_[2]);
}

void RbAmpComponent::save_prefs_() {
  RbAmpPrefData data{};
  for (uint8_t ch = 0; ch < 3; ch++) {
    data.energy_total_wh[ch] = this->energy_total_wh_[ch];
    data.energy_export_wh[ch] = this->energy_export_wh_[ch];
  }
  if (this->energy_pref_.save(&data)) {
    this->last_save_ms_ = millis();
    ESP_LOGV(TAG, "Saved Wh totals to NVS");
  } else {
    ESP_LOGW(TAG, "Failed to save Wh totals to NVS — will retry next cycle");
  }
}

// Production firmware (REG_MODE = 0) refuses flash-writing operations from
// the master. The device must be in its factory-provisioning mode for
// CMD_SAVE_GAINS / address-change / sensor_class writes to take effect.
bool RbAmpComponent::check_develop_mode_(const char *op_name) {
  uint8_t mode = 0;
  if (!this->read_u8_(REG_MODE, &mode)) {
    ESP_LOGW(TAG, "Cannot read REG_MODE; skipping %s", op_name);
    return false;
  }
  if (mode == 0) {
    ESP_LOGW(TAG,
             "%s requires the device's factory-provisioning mode "
             "(REG_MODE=1, got 0). Standard production modules will refuse "
             "this operation. To enable factory mode, see the device's "
             "hardware documentation or contact the manufacturer.",
             op_name);
    return false;
  }
  return true;
}

void RbAmpComponent::apply_ct_model_() {
  if (!this->check_develop_mode_("CT model write")) {
    return;
  }

  ESP_LOGCONFIG(TAG, "  Writing CT_MODEL = 0x%02X to RAM + flash", this->ct_model_);
  if (!this->write_u8_(REG_CT_MODEL, this->ct_model_)) {
    ESP_LOGW(TAG, "Write of CT_MODEL register failed");
    return;
  }
  delay(10);
  if (!this->write_u8_(REG_COMMAND, CMD_SAVE_GAINS)) {
    ESP_LOGW(TAG, "CMD_SAVE_GAINS failed; CT model not persisted");
    return;
  }
  // Flash erase + 32-word write window. Master must NOT issue further
  // operations during this — the slave NACKs everything mid-write. Feed
  // the watchdog so a future ESPHome tightening of setup() timing budget
  // doesn't trip on us (IP-009 audit hardening, 2026-05-28).
  App.feed_wdt();
  delay(700);
  App.feed_wdt();
  ESP_LOGCONFIG(TAG, "  CT_MODEL persisted; remove `ct_model:` from YAML after verification");
}

void RbAmpComponent::apply_sensor_class_() {
  // sensor_class_request_ default is 0xFF (codegen always sets it from YAML
  // because cv.Optional has default="SCT_013"). Skip the apply only if
  // codegen explicitly left it 0xFF, which would only happen if the YAML
  // surface evolves and this default goes away.
  if (this->sensor_class_request_ == 0xFF) {
    return;
  }

  // UNSET (0) is a legitimate sensor_class value (rare; user-explicit reset
  // before a model swap). Still requires develop mode + flash write.
  if (!this->check_develop_mode_("Sensor class write")) {
    return;
  }

  ESP_LOGCONFIG(TAG, "  Writing SENSOR_CLASS = 0x%02X to RAM + flash",
                this->sensor_class_request_);
  if (!this->write_u8_(REG_SENSOR_CLASS, this->sensor_class_request_)) {
    ESP_LOGW(TAG, "Write of REG_SENSOR_CLASS failed");
    return;
  }
  delay(10);
  if (!this->write_u8_(REG_COMMAND, CMD_SAVE_GAINS)) {
    ESP_LOGW(TAG, "CMD_SAVE_GAINS failed; sensor_class not persisted");
    return;
  }
  // Same flash-write timing as apply_ct_model_; same WDT-feed discipline.
  // On v1.2+ firmware this write also resets REG_CT_MODEL to 0 device-side
  // (prevents stale class/model bleed across a two-step provisioning),
  // which means apply_ct_model_() / apply_ct_models_per_channel_() MUST run
  // after this method — setup() enforces the order.
  App.feed_wdt();
  delay(700);
  App.feed_wdt();
  ESP_LOGCONFIG(TAG,
                "  SENSOR_CLASS persisted; remove `sensor_class:` from YAML after verification");
}

void RbAmpComponent::apply_ct_models_per_channel_() {
  if (!this->check_develop_mode_("Per-channel CT model write")) {
    return;
  }

  // Write higher channels first. Each per-channel write also clobbers ch0 as
  // a side effect of the device-side legacy direct-write callback (the chip
  // applies the most-recent REG_CT_MODEL value to channel 0 unconditionally
  // after a CMD_SET_CT_MODEL_CHn opcode), so ch0 must be written LAST to
  // settle on the operator-intended value.
  static constexpr uint8_t PER_CH_CMD[3] = {
      CMD_SET_CT_MODEL_CH0,
      CMD_SET_CT_MODEL_CH1,
      CMD_SET_CT_MODEL_CH2,
  };
  for (int8_t ch = 2; ch >= 0; --ch) {
    const uint8_t code = this->ct_models_request_[ch];
    if (code == 0xFF) {
      continue;  // channel not requested in YAML
    }
    ESP_LOGCONFIG(TAG, "  Writing CT_MODEL ch%d = 0x%02X to RAM + flash", ch, code);
    if (!this->write_u8_(REG_COMMAND, PER_CH_CMD[ch])) {
      ESP_LOGW(TAG, "Per-channel CT model CMD_SET_CT_MODEL_CH%d failed", ch);
      continue;
    }
    delay(10);
    if (!this->write_u8_(REG_CT_MODEL, code)) {
      ESP_LOGW(TAG, "Write of REG_CT_MODEL for ch%d failed", ch);
      continue;
    }
    delay(10);
    if (!this->write_u8_(REG_COMMAND, CMD_SAVE_GAINS)) {
      ESP_LOGW(TAG, "CMD_SAVE_GAINS for ch%d failed; CT model not persisted", ch);
      continue;
    }
    App.feed_wdt();
    delay(700);
    App.feed_wdt();
  }
  ESP_LOGCONFIG(TAG,
                "  Per-channel CT models persisted; remove `ct_models:` from YAML after verification");
}

void RbAmpComponent::apply_address_change_() {
  uint8_t new_addr = this->new_addr_request_;

  // i2c_address sanity (already enforced by `cv.i2c_address` and the
  // `new_address != address` Python validator, but a runtime trigger could
  // bypass schema in future — defensive check).
  if (new_addr < 0x08 || new_addr > 0x77 || new_addr == this->address_) {
    ESP_LOGW(TAG, "Invalid new_address 0x%02X — keeping current 0x%02X",
             new_addr, this->address_);
    return;
  }

  if (!this->check_develop_mode_("Address change")) {
    return;
  }

  ESP_LOGCONFIG(TAG, "  Changing I2C address: 0x%02X -> 0x%02X",
                this->address_, new_addr);

  if (!this->write_u8_(REG_I2C_ADDRESS, new_addr)) {
    ESP_LOGW(TAG, "Write of REG_I2C_ADDRESS failed; keeping 0x%02X", this->address_);
    return;
  }
  delay(10);

  if (!this->write_u8_(REG_COMMAND, CMD_SAVE_GAINS)) {
    ESP_LOGW(TAG, "CMD_SAVE_GAINS failed; address change incomplete");
    return;
  }
  // Feed watchdog around long flash + reset delays — IP-009 audit hardening
  // against future ESPHome setup()-timeout tightening (2026-05-28).
  App.feed_wdt();
  delay(700);  // flash erase + write window
  App.feed_wdt();

  // CMD_RESET — chip reboots on new_addr. We do not check the write result
  // because the slave may NACK the ACK if RESET fires before the bus cycle
  // completes.
  this->write_u8_(REG_COMMAND, CMD_RESET);
  delay(300);  // boot time + AC frequency lock + first 200 ms RT window
  App.feed_wdt();

  // From now on, all I2C ops use the new address.
  this->set_i2c_address(new_addr);

  // Verify the slave woke up correctly on the new address.
  uint8_t ver = 0;
  if (this->read_u8_(REG_VERSION, &ver)) {
    ESP_LOGCONFIG(TAG, "  Address change confirmed at 0x%02X (fw 0x%02X)",
                  new_addr, ver);
    ESP_LOGW(TAG,
             "IMPORTANT: update YAML to `address: 0x%02X` and remove "
             "`new_address:` before next boot to avoid the change-flow re-running.",
             new_addr);
    // Clear so we don't accidentally re-trigger later (defensive — setup runs once).
    this->new_addr_request_ = 0;
  } else {
    ESP_LOGE(TAG, "Address change applied but slave not responding at 0x%02X — "
                  "check wiring and power-cycle the device. If the issue "
                  "persists, see the device's hardware documentation for "
                  "factory-recovery options.", new_addr);
    this->mark_failed();
  }
}

void RbAmpComponent::start_latch_phase_() {
  // Capture wall-clock BEFORE the LATCH write so the dt we eventually
  // integrate over reflects the actual period start.
  uint32_t t_latch = millis();

  if (!this->write_u8_(REG_COMMAND, CMD_LATCH_PERIOD)) {
    ESP_LOGW(TAG, "LATCH write failed; skipping period integration this cycle");
    return;
  }

  // The device needs ~5 ms to atomically swap the period accumulator and republish
  // the snapshot block. 50 ms is comfortable headroom and matches the bench
  // Python reference. We do NOT block here — set_timeout returns immediately
  // and the callback runs from ESPHome's main loop without starving WiFi/API.
  this->set_timeout("rbamp_period", 50,
                    [this, t_latch]() { this->finish_latch_phase_(t_latch); });
}

void RbAmpComponent::finish_latch_phase_(uint32_t t_latch) {
  // Validity check — if the chip latched onto an empty accumulator (e.g. boot
  // warm-up not yet finished, or NACK race during flash write), discard.
  uint8_t valid = 0;
  if (!this->read_u8_(REG_PERIOD_VALID, &valid) || (valid & 0x01) == 0) {
    ESP_LOGD(TAG, "Period snapshot not yet valid; keeping previous integration window");
    return;
  }

  // First successful LATCH after boot: prime the clock, do NOT integrate.
  // Subsequent calls integrate over t_latch - last_latch_ms_ (unsigned wrap
  // arithmetic — works correctly across the 49-day millis() rollover).
  if (!this->primed_) {
    this->last_latch_ms_ = t_latch;
    this->primed_ = true;
    return;
  }
  uint32_t dt_ms = t_latch - this->last_latch_ms_;
  if (dt_ms == 0) {
    return;  // pathological — no elapsed time between latches
  }
  double dt_s = dt_ms / 1000.0;

  static constexpr uint8_t AVG_P_ADDR[3] = {
      REG_PERIOD_AVG_P0, REG_PERIOD_AVG_P1, REG_PERIOD_AVG_P2,
  };
  // Read all channels first; only commit last_latch_ms_ if every active
  // channel succeeded (otherwise next cycle should still cover this period).
  bool all_ok = true;
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    float avg_p = 0.0f;
    if (!this->read_float_le_(AVG_P_ADDR[ch], &avg_p)) {
      ESP_LOGW(TAG, "Failed to read PERIOD_AVG_P[%u] at 0x%02X", ch, AVG_P_ADDR[ch]);
      all_ok = false;
      continue;
    }
    this->energy_total_wh_[ch] += (double) avg_p * dt_s / 3600.0;
    if (this->energy_[ch] != nullptr) {
      this->energy_[ch]->publish_state((float) this->energy_total_wh_[ch]);
    }

    if (this->bidirectional_ && REG_PERIOD_AVG_P_NEG[ch] != 0xFF) {
      float avg_p_neg = 0.0f;
      if (this->read_float_le_(REG_PERIOD_AVG_P_NEG[ch], &avg_p_neg)) {
        this->energy_export_wh_[ch] += (double) avg_p_neg * dt_s / 3600.0;
        if (this->energy_exported_[ch] != nullptr) {
          this->energy_exported_[ch]->publish_state((float) this->energy_export_wh_[ch]);
        }
      }
    }
  }

  if (all_ok) {
    this->last_latch_ms_ = t_latch;

    // Throttled NVS save — flash wear budget. NVS sector erase ~10 ms;
    // saving every 5 min gives ~100k writes/year per device, well inside
    // ESP32 NVS wear-leveling reserves. Worst-case data loss on unexpected
    // power cut = up to SAVE_INTERVAL_MS of energy (~5 Wh at 60 W average).
    if (this->last_save_ms_ == 0
        || (t_latch - this->last_save_ms_) >= SAVE_INTERVAL_MS) {
      this->save_prefs_();
    }
  }
  // else: leave last_latch_ms_ at previous successful latch; next cycle's dt
  // will span the missed window and recover (assumes load is roughly stable
  // across the gap — a known small inaccuracy).
}

// ============================================================================
// Component lifecycle — m1 stubs
// ============================================================================

void RbAmpComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up rbAmp at 0x%02X ...", this->address_);

  if (!this->probe_slave_()) {
    this->mark_failed();
    return;
  }
  ESP_LOGCONFIG(TAG, "  Firmware version: 0x%02X", this->firmware_version_);

  // Firmware v1 (REG_VERSION == 0x01) ships with I²C general-call disabled —
  // writes to address 0x00 are silently dropped by the slave. Warn the user
  // instead of silently doing nothing. Future firmware (REG_VERSION >= 0x02)
  // will flip this; we treat anything >= 0x02 as the broadcast-capable build.
  if (this->broadcast_latch_ && this->firmware_version_ < 0x02) {
    ESP_LOGW(TAG,
             "broadcast_latch: true requested but firmware v0x%02X has "
             "I2C general-call DISABLED — broadcasts will be dropped by the "
             "slave. Per-device sequential LATCH still runs each cycle "
             "(skew at 100 kHz ≈ 270 µs × N modules, well below the 60 s "
             "period cadence). Remove the key after v1.1 firmware ships.",
             this->firmware_version_);
  }

  this->detect_variant_();

  // Restore Wh totals from NVS BEFORE publishing anything — HA Energy
  // dashboard treats a drop in `total_increasing` as a meter reset and
  // discards prior delta, so we must avoid publishing 0 even momentarily.
  this->load_prefs_();
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    if (this->energy_[ch] != nullptr) {
      this->energy_[ch]->publish_state((float) this->energy_total_wh_[ch]);
    }
    if (this->energy_exported_[ch] != nullptr) {
      this->energy_exported_[ch]->publish_state((float) this->energy_export_wh_[ch]);
    }
  }

  // Wire requested flash-side configuration in this order:
  //   1. sensor_class — required precondition for any CT model write on
  //      v1.2+ firmware (writing it also resets device-side REG_CT_MODEL=0,
  //      so any CT model write MUST come after).
  //   2. CT model — either global `ct_model:` (legacy path, single value
  //      applied to all channels) or per-channel `ct_models:` array
  //      (v1.2+ surface; the YAML schema validator enforces they're not
  //      both set).
  //   3. Address change — chip RESET at the end; everything else must
  //      already be persisted to flash.
  this->apply_sensor_class_();
  if (this->ct_model_ != 0xFF) {
    this->apply_ct_model_();
  }
  // Any non-0xFF entry in the per-channel array triggers the per-channel path.
  bool any_per_channel =
      (this->ct_models_request_[0] != 0xFF) ||
      (this->ct_models_request_[1] != 0xFF) ||
      (this->ct_models_request_[2] != 0xFF);
  if (any_per_channel) {
    this->apply_ct_models_per_channel_();
  }
  if (this->new_addr_request_ != 0) {
    this->apply_address_change_();
  }

  if (this->drdy_pin_ != nullptr) {
    this->drdy_pin_->setup();
  }

  // Primer LATCH: first LATCH after boot resets the chip's period accumulator.
  // The two-phase pipeline in start_latch_phase_/finish_latch_phase_ treats
  // the first successful read as "prime the clock, do not integrate".
  this->start_latch_phase_();
}

void RbAmpComponent::update() {
  if (this->is_failed()) {
    return;
  }

  // --- Period metering ---
  // Master-side integration: E_Wh[ch] += avg_p_W[ch] × master_dt_s / 3600.
  // start_latch_phase_() issues the LATCH and schedules a non-blocking
  // 50 ms timeout to read the snapshot — the rest of update() proceeds
  // immediately so the cooperative scheduler is not starved.
  this->start_latch_phase_();

  // --- Real-time block (0x86..0xCF, refreshed every ~200 ms by the device) ---
  // Gate on STATUS bit0 (ready) — skip publish if the chip is mid-reset, in
  // a flash-write blackout, or has not yet committed its first window.
  uint8_t status = 0;
  if (!this->read_u8_(REG_STATUS, &status) || (status & 0x01) == 0) {
    ESP_LOGD(TAG, "RT block not ready (STATUS=0x%02X) — skipping live publish", status);
    return;
  }
  this->publish_rt_block_();
}

void RbAmpComponent::publish_rt_block_() {
  // Track the freshest U_rms locally so apparent_power uses *this* cycle's
  // value, not a stale published get_state() (which is NaN on the first run).
  float u_rms_now = 0.0f;
  bool u_rms_ok = false;
  if (this->has_voltage_) {
    if (this->read_float_le_(REG_U_RMS, &u_rms_now)) {
      u_rms_ok = true;
      if (this->voltage_[0] != nullptr) {
        this->voltage_[0]->publish_state(u_rms_now);
      }
    } else {
      ESP_LOGW(TAG, "Failed to read U_rms at 0x%02X", REG_U_RMS);
    }
  }

  static constexpr uint8_t I_RMS_ADDR[3] = {REG_I0_RMS, REG_I1_RMS, REG_I2_RMS};
  static constexpr uint8_t P_REAL_ADDR[3] = {REG_P0_REAL, REG_P1_REAL, REG_P2_REAL};
  static constexpr uint8_t PF_ADDR[3] = {REG_PF0, REG_PF1, REG_PF2};
  static constexpr uint8_t Q_ADDR[3] = {REG_Q0, REG_Q1, REG_Q2};

  float i_rms_ch0 = 0.0f;
  bool i_rms_ch0_ok = false;
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    float i_rms = 0.0f;
    if (this->read_float_le_(I_RMS_ADDR[ch], &i_rms)) {
      if (this->current_[ch] != nullptr) {
        this->current_[ch]->publish_state(i_rms);
      }
      if (ch == 0) {
        i_rms_ch0 = i_rms;
        i_rms_ch0_ok = true;
      }
    }

    if (this->has_voltage_) {
      // P/PF/Q only meaningful when voltage is sampled
      float p_real = 0.0f;
      if (this->read_float_le_(P_REAL_ADDR[ch], &p_real)
          && this->power_[ch] != nullptr) {
        this->power_[ch]->publish_state(p_real);
      }

      float pf = 0.0f;
      if (this->read_float_le_(PF_ADDR[ch], &pf)
          && this->pf_[ch] != nullptr) {
        this->pf_[ch]->publish_state(pf);
      }

      float q = 0.0f;
      if (this->read_float_le_(Q_ADDR[ch], &q)
          && this->reactive_power_[ch] != nullptr) {
        this->reactive_power_[ch]->publish_state(q);
      }
    }
  }

  if (this->frequency_ != nullptr) {
    uint8_t freq = 0;
    // Only publish when the device has locked onto mains (50 Hz EU / 60 Hz US).
    // freq == 0 means "no zero-cross detected yet" and would be a misleading
    // value in HA. Other values would also be implausible — discard.
    if (this->read_u8_(REG_AC_FREQ, &freq) && (freq == 50 || freq == 60)) {
      this->frequency_->publish_state(freq);
    }
  }

  if (this->apparent_power_ != nullptr && u_rms_ok && i_rms_ch0_ok) {
    // S = V_rms × I_rms (primary channel) — only when both reads succeeded
    // this cycle. Avoid publishing NaN from stale get_state() at boot.
    this->apparent_power_->publish_state(u_rms_now * i_rms_ch0);
  }
}

void RbAmpComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "rbAmp:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  Firmware version: 0x%02X", this->firmware_version_);
  const char *topo_str = "SINGLE";
  switch (this->topology_) {
    case Topology::SPLIT_PHASE: topo_str = "SPLIT_PHASE"; break;
    case Topology::THREE_PHASE: topo_str = "THREE_PHASE"; break;
    default: break;
  }
  ESP_LOGCONFIG(TAG, "  Topology: %s, channels: %u, voltage: %s",
                topo_str, this->n_channels_, this->has_voltage_ ? "yes" : "no");
  if (this->sensor_class_request_ != 0xFF) {
    const char *cls_str = "UNSET";
    switch (this->sensor_class_request_) {
      case 1: cls_str = "SCT_013";    break;
      case 2: cls_str = "WIRED_CT";   break;
      case 3: cls_str = "BUILTIN_CT"; break;
      default: break;
    }
    ESP_LOGCONFIG(TAG, "  Sensor class: %s (0x%02X)", cls_str,
                  this->sensor_class_request_);
  }
  ESP_LOGCONFIG(TAG, "  Bidirectional: %s", YESNO(this->bidirectional_));
  ESP_LOGCONFIG(TAG, "  Broadcast LATCH: %s", YESNO(this->broadcast_latch_));
  ESP_LOGCONFIG(TAG, "  Wh persistence: NVS every %us", SAVE_INTERVAL_MS / 1000);
  if (this->drdy_pin_ != nullptr) {
    LOG_PIN("  DRDY pin: ", this->drdy_pin_);
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace rbamp
}  // namespace esphome
