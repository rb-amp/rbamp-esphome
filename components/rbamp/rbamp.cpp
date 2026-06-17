#include "rbamp.h"
#include "rbamp_const.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace rbamp {

static const char *const TAG = "rbamp";

// All wire-contract register addresses + command opcodes live in rbamp_const.h
// (manual-synced from libs/esp_idf/components/rbamp/include/rbamp_registers_v2.h —
// see source-hash comment in that header). Do NOT inline new magic constants
// here; add them to rbamp_const.h instead so cross-library drift stays
// O(1)-detectable.

// STANDARD/PRO export accumulator — addresses TBD in firmware. 0xFF means
// "skip reading; field not supported by current firmware".
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
  // Loose sanity at the I/O boundary — catches NaN/Inf (would poison the
  // energy accumulator: NaN + x = NaN) and Inf-adjacent IDF buffer-leak
  // ghost bytes. Threshold loosened in S4 from 10000 to 1e6 (per docs #7):
  // power readings can legitimately exceed 10 kW (e.g. P_ch* on a 32A
  // single-channel 230V deployment = up to ~7.3 kW per channel, fleet sums
  // can be tens of kW). 1e6 still catches obvious garbage (IDF leak typically
  // produces 1.96V-shaped floats in the ~1.96 range, well below 1e6).
  // Per-quantity tight bounds are applied at publish sites in publish_rt_block_
  // and finish_latch_phase_ via the static sanity_* helpers in rbamp.h.
  if (!std::isfinite(*out)) {
    return false;
  }
  if (std::fabs(*out) > 1e6f) {
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
  // unidirectional (BASIC tier) consumption energy (cap to 0 rather than
  // abort restore).
  //
  // TODO(STANDARD-signedness, v1.4+): on STANDARD/PRO firmware with
  // FW_TIER bit2 (bidirectional), REG_V03_PERIOD_AVG_P (0xDC) is signed —
  // net energy can legitimately be < 0 (net export). For that tier, replace
  // the `e >= 0.0` guard with `e > -kReasonableLowerBound` so net-export
  // totals survive NVS restore. Currently no-op since BASIC unidirectional
  // is the only shipping tier; the guard reads as "drop garbage" today and
  // becomes "drop garbage but keep negative net" when STANDARD lands.
  // Truth-doc §9.2 HOLD-resolved → revisit signedness handling at that point.
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

  // S4 read-compare-write: REG_CT_MODEL_CH0 (0x51) mirrors the applied value
  // for ch0, which is what the legacy single-channel path targets. Skip if
  // already matches.
  uint8_t current_ch0 = 0;
  if (this->read_u8_(REG_CT_MODEL_CH0, &current_ch0) && current_ch0 == this->ct_model_) {
    ESP_LOGD(TAG, "CT_MODEL ch0 already 0x%02X — no write needed (read-compare-write skip)", current_ch0);
    return;
  }

  ESP_LOGCONFIG(TAG, "  Writing CT_MODEL = 0x%02X to RAM + flash (was 0x%02X)",
                this->ct_model_, current_ch0);
  if (!this->write_u8_(REG_CT_MODEL, this->ct_model_)) {
    ESP_LOGW(TAG, "Write of CT_MODEL register failed");
    return;
  }
  delay(10);
  if (!this->save_user_config_()) {
    return;
  }
  ESP_LOGCONFIG(TAG, "  CT_MODEL persisted; remove `ct_model:` from YAML after verification");
}

bool RbAmpComponent::save_user_config_() {
  // v1.3 firmware ships CMD_SAVE_USER_CONFIG (0x32) as the canonical opcode
  // for persisting user-config writes (SENSOR_CLASS, CT_MODEL, ADDRESS,
  // FLEET_CONFIG, GROUP_ID). Legacy v1.0-v1.2 firmware only supports
  // CMD_SAVE_GAINS (0x26) and routes the same flash sector through it.
  // Capability-gate so v1.3 boards use the cleaner opcode without breaking
  // older deployments.
  uint8_t opcode = this->check_capability_(CAP_SAVE_USER_CONFIG)
                    ? CMD_SAVE_USER_CONFIG : CMD_SAVE_GAINS;
  if (!this->write_u8_(REG_COMMAND, opcode)) {
    ESP_LOGW(TAG, "Save opcode 0x%02X failed; config not persisted", opcode);
    return false;
  }
  App.feed_wdt();
  delay(SETTLE_MS_SAVE_USER_CONFIG);  // same 700 ms on both codepaths
  App.feed_wdt();
  return true;
}

void RbAmpComponent::apply_sensor_class_() {
  // sensor_class_request_ default is 0xFF (codegen always sets it from YAML
  // because cv.Optional has default="SCT_013"). Skip the apply only if
  // codegen explicitly left it 0xFF, which would only happen if the YAML
  // surface evolves and this default goes away.
  if (this->sensor_class_request_ == 0xFF) {
    return;
  }

  // S4 read-compare-write: skip the 700 ms flash erase entirely if the device
  // already has the requested sensor_class. v1.3 read-back is direct (the
  // register holds the applied value); on legacy firmware where REG_SENSOR_CLASS
  // doesn't exist (returns 0 for unmapped reads), the comparison
  // requested!=0 → write proceeds (correct fallback).
  uint8_t current = 0xFF;
  if (this->read_u8_(REG_SENSOR_CLASS, &current) && current == this->sensor_class_request_) {
    ESP_LOGD(TAG, "SENSOR_CLASS already 0x%02X — no write needed (read-compare-write skip)",
             current);
    return;
  }

  // UNSET (0) is a legitimate sensor_class value (rare; user-explicit reset
  // before a model swap). Still requires develop mode + flash write.
  if (!this->check_develop_mode_("Sensor class write")) {
    return;
  }

  ESP_LOGCONFIG(TAG, "  Writing SENSOR_CLASS = 0x%02X to RAM + flash (was 0x%02X)",
                this->sensor_class_request_, current);
  if (!this->write_u8_(REG_SENSOR_CLASS, this->sensor_class_request_)) {
    ESP_LOGW(TAG, "Write of REG_SENSOR_CLASS failed");
    return;
  }
  delay(10);
  // On v1.2+ firmware this write also resets REG_CT_MODEL to 0 device-side
  // (prevents stale class/model bleed across a two-step provisioning),
  // which means apply_ct_model_() / apply_ct_models_per_channel_() MUST run
  // after this method — setup() enforces the order.
  if (!this->save_user_config_()) {
    return;
  }
  ESP_LOGCONFIG(TAG,
                "  SENSOR_CLASS persisted; remove `sensor_class:` from YAML after verification");
}

void RbAmpComponent::apply_ct_models_per_channel_() {
  // S4 read-compare-write: REG_CT_MODEL_CH0/1/2 (0x51-0x53) are v1.3 read-back
  // mirrors that report the actually-applied model per channel. Skip the
  // entire per-channel write sequence (3 × 700 ms flash erase = ~2.1 s)
  // if all requested channels already match the device-side state.
  // On legacy firmware where 0x51-0x53 are unmapped (returns 0): the
  // comparison fails for any non-zero requested code → write proceeds.
  uint8_t current_ch[3] = {0, 0, 0};
  bool readback_ok = true;
  for (uint8_t ch = 0; ch < 3; ch++) {
    if (!this->read_u8_(REG_CT_MODEL_CH0 + ch, &current_ch[ch])) {
      readback_ok = false;
      break;
    }
  }
  if (readback_ok) {
    bool all_match = true;
    for (uint8_t ch = 0; ch < 3; ch++) {
      const uint8_t requested = this->ct_models_request_[ch];
      if (requested == 0xFF) continue;  // not requested in YAML; no constraint
      if (current_ch[ch] != requested) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      ESP_LOGD(TAG, "Per-channel CT models already match device state "
               "[ch0=0x%02X ch1=0x%02X ch2=0x%02X] — no write needed (read-compare-write skip)",
               current_ch[0], current_ch[1], current_ch[2]);
      return;
    }
    ESP_LOGCONFIG(TAG,
                  "  Per-channel CT models differ from device state — applying writes "
                  "[device: ch0=0x%02X ch1=0x%02X ch2=0x%02X, requested: ch0=0x%02X ch1=0x%02X ch2=0x%02X]",
                  current_ch[0], current_ch[1], current_ch[2],
                  this->ct_models_request_[0], this->ct_models_request_[1],
                  this->ct_models_request_[2]);
  } else {
    ESP_LOGD(TAG, "REG_CT_MODEL_CH0/1/2 unreadable (legacy firmware?) — applying writes unconditionally");
  }

  if (!this->check_develop_mode_("Per-channel CT model write")) {
    return;
  }

  // Canon per-channel write sequence (truth-doc §7 + arduino + esp-idf):
  //   1. write REG_CT_MODEL = code  (scratch register)
  //   2. write REG_COMMAND  = CMD_SET_CT_MODEL_CHn  (opcode latches scratch to ch n)
  //   3. sleep SETTLE_MS_SET_CT_MODEL_CHN (5 ms — chip latches to channel storage)
  //   4. write REG_COMMAND  = CMD_SAVE_GAINS  (persist to flash; S4 will switch
  //      to CMD_SAVE_USER_CONFIG once capability-gated for v1.0-v1.2 compat)
  //   5. sleep SETTLE_MS_SAVE_GAINS (700 ms flash erase + write)
  //
  // Channel iteration order is DESCENDING (ch2 → ch1 → ch0). Rationale: the
  // CT_MODEL scratch is shared; once SAVE_GAINS fires after each
  // CMD_SET_CT_MODEL_CHn, the per-channel storage is committed and subsequent
  // iterations don't disturb it. Descending keeps ch0 as the LAST write so a
  // mid-sequence master reset would leave ch0 (the most common single-channel
  // SKU) in the operator-intended state rather than zeroed.
  //
  // Verification: REG_CT_MODEL_CH0/1/2 (0x51-0x53) expose the actually-applied
  // model per channel for post-write readback. S4 will use them for the
  // read-compare-write boot-time optimization (docs #2).
  //
  // L7 (cross-canon verify): the previous impl had the order INVERTED
  // (CMD_SET_CT_MODEL_CHn first, then REG_CT_MODEL=code, then SAVE_GAINS).
  // The descending iteration was originally justified as compensating for a
  // "ch0 clobber side-effect" — that was a misread of the chip's scratch-
  // register semantics. The actual canon is: REG_CT_MODEL is the input to
  // each CMD_SET_CT_MODEL_CHn opcode, so it must be written FIRST per channel.
  // Descending iteration is still preserved (different rationale above).
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
    ESP_LOGCONFIG(TAG, "  Writing CT_MODEL ch%d = 0x%02X (canon: REG_CT_MODEL -> CMD_SET_CT_MODEL_CH%d -> SAVE_USER_CONFIG)", ch, code, ch);
    if (!this->write_u8_(REG_CT_MODEL, code)) {
      ESP_LOGW(TAG, "Write of REG_CT_MODEL=0x%02X for ch%d failed", code, ch);
      continue;
    }
    if (!this->write_u8_(REG_COMMAND, PER_CH_CMD[ch])) {
      ESP_LOGW(TAG, "CMD_SET_CT_MODEL_CH%d failed", ch);
      continue;
    }
    delay(SETTLE_MS_SET_CT_MODEL_CHN);
    if (!this->save_user_config_()) {
      ESP_LOGW(TAG, "Save for ch%d failed; CT model not persisted", ch);
      continue;
    }
  }
  ESP_LOGCONFIG(TAG,
                "  Per-channel CT models persisted; remove `ct_models:` from YAML after verification");
}

bool RbAmpComponent::check_capability_(uint16_t bit) {
  if (!this->capability_cached_) {
    uint8_t buf[2] = {0, 0};
    if (this->read_register(REG_CAPABILITY, buf, 2) != i2c::ERROR_OK) {
      // Capability register not implemented on v1.0-v1.2 firmware; treat as
      // "no capabilities" → all caller-side feature gates fall back to legacy
      // path. ESP_LOGD because this is normal on older devices.
      ESP_LOGD(TAG, "REG_CAPABILITY unreadable — assuming v1.0-v1.2 firmware (no capabilities advertised)");
      this->cached_capability_ = 0;
      this->capability_cached_ = true;
      return false;
    }
    this->cached_capability_ = static_cast<uint16_t>(buf[0]) |
                                (static_cast<uint16_t>(buf[1]) << 8);
    this->capability_cached_ = true;
    ESP_LOGCONFIG(TAG, "  Capability bitmap: 0x%04X", this->cached_capability_);
  }
  return (this->cached_capability_ & bit) != 0;
}

void RbAmpComponent::apply_fleet_config_() {
  // Apply REG_FLEET_CONFIG.bit0 (GC_ENABLE) + REG_GROUP_ID if YAML requested.
  // Both writes are user-config (need CMD_SAVE_USER_CONFIG + CMD_RESET to
  // take effect — FLEET_CONFIG is not toggled live per the v2 spec note).
  // Capability-gated: if the device doesn't advertise CAP_GC_LATCH, skip
  // with a warning so users on older firmware get clear feedback.
  if (this->fleet_gc_enable_request_ == 0xFF && !this->group_id_request_valid_) {
    return;  // nothing to apply
  }

  if (!this->check_capability_(CAP_GC_LATCH)) {
    ESP_LOGW(TAG,
             "fleet_gc_enable / group_id requested but the device does not "
             "advertise CAP_GC_LATCH (REG_CAPABILITY 0x57 bit1). General-call "
             "broadcast LATCH requires v1.3+ firmware. Per-device sequential "
             "LATCH still runs each cycle. Remove the YAML keys to silence "
             "this warning.");
    return;
  }

  if (!this->check_develop_mode_("Fleet config write")) {
    return;
  }

  bool need_save = false;

  if (this->fleet_gc_enable_request_ != 0xFF) {
    uint8_t current = 0;
    bool have_current = this->read_u8_(REG_FLEET_CONFIG, &current);
    uint8_t desired = (this->fleet_gc_enable_request_ ? (current | 0x01)
                                                       : (current & ~0x01));
    if (!have_current || current != desired) {
      ESP_LOGCONFIG(TAG, "  Writing FLEET_CONFIG = 0x%02X (was 0x%02X)", desired,
                    have_current ? current : 0xFF);
      if (!this->write_u8_(REG_FLEET_CONFIG, desired)) {
        ESP_LOGW(TAG, "Write of REG_FLEET_CONFIG failed");
      } else {
        need_save = true;
      }
    } else {
      ESP_LOGD(TAG, "FLEET_CONFIG already 0x%02X — no write needed", current);
    }
  }

  if (this->group_id_request_valid_) {
    uint8_t current = 0;
    bool have_current = this->read_u8_(REG_GROUP_ID, &current);
    if (!have_current || current != this->group_id_request_) {
      ESP_LOGCONFIG(TAG, "  Writing GROUP_ID = 0x%02X (was 0x%02X)",
                    this->group_id_request_, have_current ? current : 0xFF);
      if (!this->write_u8_(REG_GROUP_ID, this->group_id_request_)) {
        ESP_LOGW(TAG, "Write of REG_GROUP_ID failed");
      } else {
        need_save = true;
      }
    } else {
      ESP_LOGD(TAG, "GROUP_ID already 0x%02X — no write needed", current);
    }
  }

  if (need_save) {
    // FLEET_CONFIG.bit0 only takes effect after RESET — no live toggle. Save
    // user config, then reset, then re-probe.
    if (!this->write_u8_(REG_COMMAND, CMD_SAVE_USER_CONFIG)) {
      ESP_LOGW(TAG, "CMD_SAVE_USER_CONFIG failed; fleet config not persisted");
      return;
    }
    App.feed_wdt();
    delay(SETTLE_MS_SAVE_USER_CONFIG);
    App.feed_wdt();

    // Reset the device so FLEET_CONFIG.bit0 takes effect (general-call
    // reception is configured at IDLE-state init time, not toggleable live).
    this->write_u8_(REG_COMMAND, CMD_RESET);
    delay(SETTLE_MS_RESET);
    App.feed_wdt();

    // Verify the slave woke up
    uint8_t ver = 0;
    if (!this->read_u8_(REG_VERSION, &ver)) {
      ESP_LOGW(TAG, "Device did not respond after fleet config reset");
    } else {
      ESP_LOGCONFIG(TAG, "  Fleet config applied; device returned at fw 0x%02X", ver);
    }
  }
}

bool RbAmpComponent::transmit_gc_frame() {
  if (!this->check_capability_(CAP_GC_LATCH)) {
    ESP_LOGW(TAG, "transmit_gc_frame() skipped — CAP_GC_LATCH not advertised");
    return false;
  }

  // 5-byte canonical frame to address 0x00 (general call). Group filter on
  // the slave side rejects if frame[2] != REG_GROUP_ID and != 0x00.
  uint16_t tick = ++this->gc_tick_counter_;
  if (tick == 0) tick = 1;  // skip 0xFFFF -> 0 wrap which collides with "never received" sentinel

  uint8_t frame[5] = {
      GC_FRAME_MAGIC,
      GC_FRAME_OPCODE,
      this->group_id_request_,
      static_cast<uint8_t>(tick & 0xFF),
      static_cast<uint8_t>((tick >> 8) & 0xFF),
  };

  // Raw bus-level write to address 0x00 — bypasses our own device address.
  // Uses the underlying bus_; not all I2CBus impls expose a public general-
  // call surface, so fall back to a per-byte write on i2c::ERROR_NOT_ACKNOWLEDGED
  // (general-call reads typically NACK because there's no responding device
  // on the bus to ACK the address phase — the spec allows this).
  auto err = this->bus_->write(0x00, frame, sizeof(frame), true);
  if (err != i2c::ERROR_OK && err != i2c::ERROR_NOT_ACKNOWLEDGED) {
    ESP_LOGW(TAG, "GC frame bus write failed: err=%d, tick=%u", static_cast<int>(err), tick);
    return false;
  }
  ESP_LOGD(TAG, "GC frame tx: group=0x%02X tick=%u", this->group_id_request_, tick);
  return true;
}

bool RbAmpComponent::gc_witness_check_() {
  // Post-GC verification (truth-doc §5 + OI-4): after the chip-side 300 ms
  // settle from the GC LATCH command, REG_PERIOD_VALID should show bit0=1 if
  // this slave accepted the LATCH and produced fresh snapshot data. Returns
  // true if PERIOD_VALID confirms uptake; false on any failure (read failure,
  // bit0=0). Caller decides whether to fall back to per-device LATCH.
  uint8_t valid = 0;
  if (!this->read_u8_(REG_PERIOD_VALID, &valid)) {
    ESP_LOGD(TAG, "GC witness check: PERIOD_VALID read failed");
    return false;
  }
  if ((valid & 0x01) == 0) {
    ESP_LOGD(TAG, "GC witness check: PERIOD_VALID=0 (no fresh snapshot — slave likely rejected GC)");
    return false;
  }
  return true;
}

std::string RbAmpComponent::get_variant_str() {
  uint8_t v = 0;
  if (!this->read_u8_(REG_HW_VARIANT, &v)) return "UNK";
  switch (v) {
    case 1: return "UI1";
    case 2: return "UI2";
    case 3: return "UI3";
    case 4: return "I1";
    case 5: return "I2";
    case 6: return "I3";
    default: return "UNK";
  }
}

std::string RbAmpComponent::get_capability_hex() {
  this->check_capability_(0);  // populate cached_capability_ as side-effect
  char buf[8];
  snprintf(buf, sizeof(buf), "0x%04X", this->cached_capability_);
  return std::string(buf);
}

std::string RbAmpComponent::get_uid_hex() {
  uint8_t buf[12];
  if (this->read_register(REG_UID, buf, 12) != i2c::ERROR_OK) {
    return "UNREADABLE";
  }
  char hex[2 * 12 + 1] = {0};
  int p = 0;
  for (uint8_t i = 0; i < 12; i++) {
    p += snprintf(hex + p, sizeof(hex) - p, "%02X", buf[i]);
  }
  return std::string(hex);
}

std::string RbAmpComponent::get_last_error_str() {
  uint8_t e = 0;
  if (!this->read_u8_(REG_ERROR, &e)) return "UNREADABLE";
  switch (e) {
    case DEV_ERR_OK:                return "OK";
    case DEV_ERR_CLONE:              return "ERR_CLONE";
    case DEV_ERR_LUT_BAD:            return "ERR_LUT_BAD";
    case DEV_ERR_FLASH_PARAMS_BAD:   return "ERR_FLASH_PARAMS_BAD";
    case DEV_ERR_NOT_READY:          return "ERR_NOT_READY";
    case DEV_ERR_SENSOR_OVERFLOW:    return "ERR_SENSOR_OVERFLOW";
    case DEV_ERR_PARAM:              return "ERR_PARAM";
    case DEV_ERR_UNHANDLED:          return "ERR_UNHANDLED";
    default: {
      char buf[16];
      snprintf(buf, sizeof(buf), "ERR_0x%02X", e);
      return std::string(buf);
    }
  }
}

uint8_t RbAmpComponent::get_event_flags() {
  uint8_t f = 0;
  if (!this->read_u8_(REG_EVENT_FLAGS, &f)) return 0;
  return f;
}

bool RbAmpComponent::write_clear_error() {
  // Capability-gated: CAP_CLEAR_ERROR (bit10). On legacy firmware where the
  // opcode is unimplemented, the write may be silently dropped; caller
  // shouldn't rely on it without verifying capability.
  if (!this->check_capability_(CAP_CLEAR_ERROR)) {
    ESP_LOGW(TAG, "CMD_CLEAR_ERROR skipped — CAP_CLEAR_ERROR not advertised");
    return false;
  }
  return this->write_u8_(REG_COMMAND, CMD_CLEAR_ERROR);
}

bool RbAmpComponent::write_reset() {
  // RESET — the slave may NACK the ACK because RESET fires before the bus
  // cycle completes; treat the write result as advisory.
  this->write_u8_(REG_COMMAND, CMD_RESET);
  return true;
}

bool RbAmpComponent::fleet_apply_now() {
  this->apply_fleet_config_();
  return true;
}

uint16_t RbAmpComponent::read_gc_tick_received() {
  uint8_t lo = 0, hi = 0;
  if (!this->read_u8_(REG_GC_TICK, &lo) || !this->read_u8_(REG_GC_TICK + 1, &hi)) {
    return 0xFFFF;  // "never received" sentinel
  }
  return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
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

  ESP_LOGCONFIG(TAG, "  Changing I2C address: 0x%02X -> 0x%02X",
                this->address_, new_addr);

  // S4: v1.3 two-phase commit (capability-gated). Per truth-doc §6 +
  // CAP_TWO_PHASE_ADDR (REG_CAPABILITY bit7), v1.3 firmware supports a
  // safer address-change flow that does NOT require factory-provisioning
  // mode:
  //   1. Write REG_I2C_ADDRESS = candidate (staged in RAM; subsequent reads
  //      of 0x30 return the staged value)
  //   2. Write REG_ADDR_COMMIT_MAGIC = 0xA5 (arms the commit — magic is
  //      consumed on commit attempt regardless of success)
  //   3. Write REG_COMMAND = CMD_COMMIT_ADDR (opcode 0x30) — atomically
  //      moves staged → live and saves to flash
  //   4. Wait SETTLE_MS_COMMIT_ADDR (700 ms — flash erase + write window)
  //   5. Slave starts ACKing on new address; verify by REG_VERSION read
  //
  // Legacy fallback (v1.0-v1.2): single-phase REG_I2C_ADDRESS + SAVE +
  // RESET, gated on factory-provisioning mode (REG_MODE=1) per the old
  // contract.
  bool two_phase = this->check_capability_(CAP_TWO_PHASE_ADDR);
  if (!two_phase && !this->check_develop_mode_("Address change (legacy single-phase)")) {
    return;
  }

  if (two_phase) {
    ESP_LOGCONFIG(TAG, "  Using v1.3 two-phase commit (production-OK)");

    if (!this->write_u8_(REG_I2C_ADDRESS, new_addr)) {
      ESP_LOGW(TAG, "Stage write of REG_I2C_ADDRESS failed; keeping 0x%02X", this->address_);
      return;
    }
    delay(10);
    if (!this->write_u8_(REG_ADDR_COMMIT_MAGIC, ADDR_COMMIT_MAGIC_VAL)) {
      ESP_LOGW(TAG, "Arm write of REG_ADDR_COMMIT_MAGIC failed");
      return;
    }
    delay(10);
    if (!this->write_u8_(REG_COMMAND, CMD_COMMIT_ADDR)) {
      ESP_LOGW(TAG, "CMD_COMMIT_ADDR failed; address change incomplete");
      return;
    }
    App.feed_wdt();
    delay(SETTLE_MS_COMMIT_ADDR);
    App.feed_wdt();
  } else {
    ESP_LOGCONFIG(TAG, "  Using legacy single-phase commit (requires factory mode)");
    if (!this->write_u8_(REG_I2C_ADDRESS, new_addr)) {
      ESP_LOGW(TAG, "Write of REG_I2C_ADDRESS failed; keeping 0x%02X", this->address_);
      return;
    }
    delay(10);
    if (!this->save_user_config_()) {
      ESP_LOGW(TAG, "Save failed; address change incomplete");
      return;
    }
    // CMD_RESET — chip reboots on new_addr. We do not check the write result
    // because the slave may NACK the ACK if RESET fires before the bus cycle
    // completes.
    this->write_u8_(REG_COMMAND, CMD_RESET);
    delay(SETTLE_MS_RESET);
    App.feed_wdt();
  }

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
  // REG_PERIOD_VALID is a level flag (NOT cleared-on-read) — bit0 = 1 means
  // the most recent latch produced fresh data; 0 means the chip's accumulator
  // was empty at latch time. Single shared bit across channels (no per-channel
  // validity flag in the wire contract).
  uint8_t valid = 0;
  if (!this->read_u8_(REG_PERIOD_VALID, &valid) || (valid & 0x01) == 0) {
    ESP_LOGD(TAG, "Period snapshot not yet valid; keeping previous integration window");
    return;
  }

  static constexpr uint8_t AVG_P_ADDR[3] = {
      REG_PERIOD_AVG_P0, REG_PERIOD_AVG_P1, REG_PERIOD_AVG_P2,
  };
  // Per-channel state (rbamp.h::primed_[ch] / last_latch_ms_[ch]) lets healthy
  // channels keep integrating when one channel's CT clock-stretches beyond
  // timeout. UI3 mixed-CT (small SCT + large SCT + WCT) is the canonical case:
  // a single per-CT glitch must not freeze energy on the other two.
  //
  // L9 anti-revert: dt_s comes from MASTER wall-clock (millis() captured at
  // start_latch_phase_), NOT from REG_PERIOD_LATCH_MS (0xEC) nor REG_PERIOD_MS_FW
  // (0xCA). Chip SysTick starves under interrupt load → those values under-
  // count by ~26% (HW-validated). Future porters reading this code: do NOT
  // switch to chip-period — see rbamp_const.h L9 note and
  // libs/esp_idf/components/rbamp/include/rbamp_energy.h for the canonical
  // rationale (anti-revert doc baked into the upstream header @param period_ms).
  bool any_integrated = false;
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    float avg_p = 0.0f;
    if (!this->read_float_le_(AVG_P_ADDR[ch], &avg_p) || !sanity_power_(avg_p)) {
      ESP_LOGW(TAG, "Failed to read or validate PERIOD_AVG_P[%u] at 0x%02X — channel %u stays at previous integration window",
               ch, AVG_P_ADDR[ch], ch);
      continue;  // skip ch — next cycle's dt spans the gap
    }

    // First successful read for this channel after boot: prime the clock,
    // do NOT integrate (no prior dt anchor).
    if (!this->primed_[ch]) {
      this->last_latch_ms_[ch] = t_latch;
      this->primed_[ch] = true;
      continue;
    }

    uint32_t dt_ms = t_latch - this->last_latch_ms_[ch];
    if (dt_ms == 0) {
      continue;  // pathological — same-instant retry
    }
    double dt_s = dt_ms / 1000.0;

    this->energy_total_wh_[ch] += (double) avg_p * dt_s / 3600.0;
    if (this->energy_[ch] != nullptr) {
      this->energy_[ch]->publish_state((float) this->energy_total_wh_[ch]);
    }

    if (this->bidirectional_ && REG_PERIOD_AVG_P_NEG[ch] != 0xFF) {
      float avg_p_neg = 0.0f;
      if (this->read_float_le_(REG_PERIOD_AVG_P_NEG[ch], &avg_p_neg) && sanity_power_(avg_p_neg)) {
        this->energy_export_wh_[ch] += (double) avg_p_neg * dt_s / 3600.0;
        if (this->energy_exported_[ch] != nullptr) {
          this->energy_exported_[ch]->publish_state((float) this->energy_export_wh_[ch]);
        }
      }
    }

    this->last_latch_ms_[ch] = t_latch;
    any_integrated = true;
  }

  // Throttled NVS save — flash wear budget. NVS sector erase ~10 ms; saving
  // every 5 min gives ~100k writes/year per device, well inside ESP32 NVS
  // wear-leveling reserves. Save only when at least one channel integrated
  // this cycle (avoid burning NVS writes on all-channels-failed cycles).
  if (any_integrated && (this->last_save_ms_ == 0
      || (t_latch - this->last_save_ms_) >= SAVE_INTERVAL_MS)) {
    this->save_prefs_();
  }
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
  // Fleet config (REG_FLEET_CONFIG.bit0 + REG_GROUP_ID) — written before
  // address change because the device reset triggered by apply_fleet_config_
  // would otherwise come between the fleet writes and apply_address_change_'s
  // own reset. Both fleet config and address change require flash + reset;
  // separating them is cleaner than coalescing (~1.4 s additional boot time
  // on a fleet-config + address-change cycle, but YAML changes happen
  // rarely).
  this->apply_fleet_config_();
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
    if (this->read_float_le_(REG_U_RMS, &u_rms_now) && sanity_voltage_(u_rms_now)) {
      u_rms_ok = true;
      if (this->voltage_[0] != nullptr) {
        this->voltage_[0]->publish_state(u_rms_now);
      }
    } else {
      ESP_LOGW(TAG, "Failed to read or validate U_rms at 0x%02X", REG_U_RMS);
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
    if (this->read_float_le_(I_RMS_ADDR[ch], &i_rms) && sanity_current_(i_rms)) {
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
      if (this->read_float_le_(P_REAL_ADDR[ch], &p_real) && sanity_power_(p_real)
          && this->power_[ch] != nullptr) {
        this->power_[ch]->publish_state(p_real);
      }

      float pf = 0.0f;
      if (this->read_float_le_(PF_ADDR[ch], &pf) && sanity_pf_(pf)
          && this->pf_[ch] != nullptr) {
        this->pf_[ch]->publish_state(pf);
      }

      float q = 0.0f;
      if (this->read_float_le_(Q_ADDR[ch], &q) && sanity_power_(q)
          && this->reactive_power_[ch] != nullptr) {
        this->reactive_power_[ch]->publish_state(q);
      }
    }
  }

  if (this->frequency_ != nullptr) {
    uint8_t freq = 0;
    // Only publish when the device has locked onto mains. sanity_freq_ allows
    // 40-70 Hz to cover both EU (50) and US (60) plus reasonable hold-around
    // for inverter-fed mains; freq == 0 means "no zero-cross detected yet"
    // (rejected by sanity_freq_).
    if (this->read_u8_(REG_AC_FREQ, &freq) && sanity_freq_(freq)) {
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
  if (this->capability_cached_) {
    ESP_LOGCONFIG(TAG, "  CAPABILITY bitmap: 0x%04X (GC=%s GROUP_FILTER=%s "
                  "DIGEST=%s EVENTS=%s TWO_PHASE_ADDR=%s SAVE_USER_CONFIG=%s)",
                  this->cached_capability_,
                  YESNO((this->cached_capability_ & CAP_GC_LATCH) != 0),
                  YESNO((this->cached_capability_ & CAP_GC_GROUP_FILTER) != 0),
                  YESNO((this->cached_capability_ & CAP_DIGEST) != 0),
                  YESNO((this->cached_capability_ & CAP_EVENTS) != 0),
                  YESNO((this->cached_capability_ & CAP_TWO_PHASE_ADDR) != 0),
                  YESNO((this->cached_capability_ & CAP_SAVE_USER_CONFIG) != 0));
  }
  if (this->fleet_gc_enable_request_ != 0xFF) {
    ESP_LOGCONFIG(TAG, "  Fleet GC enable requested: %s",
                  YESNO(this->fleet_gc_enable_request_ != 0));
  }
  if (this->group_id_request_valid_) {
    ESP_LOGCONFIG(TAG, "  Group ID requested: 0x%02X", this->group_id_request_);
  }
  ESP_LOGCONFIG(TAG, "  Wh persistence: NVS every %us", SAVE_INTERVAL_MS / 1000);
  if (this->drdy_pin_ != nullptr) {
    LOG_PIN("  DRDY pin: ", this->drdy_pin_);
  }
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace rbamp
}  // namespace esphome
