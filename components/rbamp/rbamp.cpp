#include "rbamp.h"
#include "rbamp_const.h"
#include "rbamp_sct_rating.h"
#include "rbamp_std_registers.h"
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

// v1.5 — Channel access window: select-and-read protocol for ch3+ on senior
// SKUs (UI5/UI7). Fields 0=I_RMS, 1=I_PEAK, 2=P_REAL, 3=PF, 13=PERIOD_AVG_P.
// Fields 4-12 are not used by this component.
//
// CRITICAL: re-select on EVERY read. The window data buffer holds a snapshot
// captured at select time; re-reading REG_CHANNEL_DATA without a new select
// returns the same stale snapshot. There is no way to cache the selector for
// speed — the protocol is designed to force per-value select+read atomicity.
bool RbAmpComponent::read_channel_window_float_(uint8_t field, uint8_t ch, float *out) {
  const uint8_t sel = static_cast<uint8_t>((field << 4) | (ch & 0x0F));
  if (!this->write_u8_(REG_CHANNEL_SELECT, sel)) {
    return false;
  }
  // 4-byte float32 LE at REG_CHANNEL_DATA..REG_CHANNEL_DATA+3.
  // read_float_le_ handles the 4 separate 1-byte reads under the hood
  // (this device has no auto-increment; ANY multi-byte read requires that).
  return this->read_float_le_(REG_CHANNEL_DATA, out);
}

// v1.6 — Channel access window u8 write. Used exclusively for
// CHFIELD_CT_MODEL (field 15) today; NF (field 4) + GAIN (field 5) will
// take the same path when exposed. The high 3 bytes of REG_CHANNEL_DATA
// MUST be zero-padded — firmware reads the full 4-byte word on selector
// match; a stale byte in 0x3D..0x3F would corrupt the applied value.
// Every byte is its own transaction — the device has no auto-increment.
bool RbAmpComponent::write_channel_window_u8_(uint8_t field, uint8_t ch, uint8_t val) {
  const uint8_t sel = static_cast<uint8_t>((field << 4) | (ch & 0x0F));
  if (!this->write_u8_(REG_CHANNEL_SELECT, sel)) return false;
  if (!this->write_u8_(REG_CHANNEL_DATA + 0, val)) return false;
  if (!this->write_u8_(REG_CHANNEL_DATA + 1, 0)) return false;
  if (!this->write_u8_(REG_CHANNEL_DATA + 2, 0)) return false;
  if (!this->write_u8_(REG_CHANNEL_DATA + 3, 0)) return false;
  return true;
}

bool RbAmpComponent::read_channel_window_u8_lo_(uint8_t field, uint8_t ch, uint8_t *out) {
  const uint8_t sel = static_cast<uint8_t>((field << 4) | (ch & 0x0F));
  if (!this->write_u8_(REG_CHANNEL_SELECT, sel)) return false;
  uint32_t word = 0;
  if (!this->read_u32_le_(REG_CHANNEL_DATA, &word)) return false;
  *out = static_cast<uint8_t>(word & 0xFF);
  return true;
}

// ============================================================================
// STD line (16-bit register pointer) — transport + probe + realtime poll
//
// Wire contract: rbamp_std_registers.h (codegen, schema CRC 0x794BF18D).
// Pointer is 2 bytes big-endian with 16-bit auto-increment; DATA is
// little-endian fixed-point (U=mV, I=uA, P=mW, Q=mvar, S=mVA, PF=Q10,
// FREQ=cHz). ESPHome's read_register16/write_register16 already emit the
// BE pointer + repeated-start combined transaction, so the transport maps
// 1:1 onto the I2CDevice API. Multi-byte fields are published byte-wise
// device-side with no atomic multi-register snapshot, so a burst is no less
// coherent than N single reads and costs one address phase instead of N.
// ============================================================================

static inline uint16_t std_le_u16_(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static inline int16_t std_le_i16_(const uint8_t *p) { return static_cast<int16_t>(std_le_u16_(p)); }
static inline int32_t std_le_i32_(const uint8_t *p) {
  return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                              (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24));
}
static inline uint32_t std_le_u32_(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static inline int64_t std_le_i64_(const uint8_t *p) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < 8; i++) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return static_cast<int64_t>(v);
}

// Largest burst per transaction — keeps retries cheap; longer reads are
// split with an explicit pointer per chunk (mirrors the esp-idf reference).
static constexpr size_t STD_BURST_MAX = 32;

bool RbAmpComponent::std_read_(uint16_t reg, uint8_t *buf, size_t n) {
  while (n) {
    const size_t k = (n > STD_BURST_MAX) ? STD_BURST_MAX : n;
    bool ok = false;
    // Same 3×5 ms NACK-retry discipline as the BASIC helpers: the slave
    // stalls its I²C engine briefly at window boundaries and during
    // command dispatch; a NACK there is transient, not fatal.
    for (uint8_t attempt = 0; attempt < 3 && !ok; ++attempt) {
      if (this->read_register16(reg, buf, k) == i2c::ERROR_OK) {
        ok = true;
      } else if (attempt < 2) {
        delay(5);
      }
    }
    if (!ok) return false;
    reg = static_cast<uint16_t>(reg + k);
    buf += k;
    n -= k;
  }
  return true;
}

bool RbAmpComponent::std_read_coherent_(uint16_t reg, uint8_t *buf, size_t n) {
  if (n > 128) return false;  // family canon: coherent snapshot reads are ≤128 B
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (this->read_register16(reg, buf, n) == i2c::ERROR_OK) {
      return true;
    }
    if (attempt < 2) delay(5);
  }
  return false;
}

bool RbAmpComponent::std_write_u16_(uint16_t reg, uint16_t val) {
  uint8_t b[2] = {static_cast<uint8_t>(val & 0xFF), static_cast<uint8_t>(val >> 8)};
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (this->write_register16(reg, b, 2) == i2c::ERROR_OK) {
      return true;
    }
    if (attempt < 2) delay(5);
  }
  return false;
}

bool RbAmpComponent::std_write_u8_(uint16_t reg, uint8_t val) {
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (this->write_register16(reg, &val, 1) == i2c::ERROR_OK) {
      return true;
    }
    if (attempt < 2) delay(5);
  }
  return false;
}

bool RbAmpComponent::std_cmd_verify_(uint8_t opcode, uint16_t settle_ms, const char *op_name) {
  if (!this->std_write_u8_(rbamp_std::REG_COMMAND, opcode)) {
    ESP_LOGW(TAG, "%s: COMMAND write failed", op_name);
    return false;
  }
  App.feed_wdt();
  delay(settle_ms);
  App.feed_wdt();
  if (this->std_version_ < 0x34) {
    return true;  // ERROR is sticky pre-0x34 — no reliable sync verdict
  }
  uint8_t e = 0;
  if (!this->std_read_(rbamp_std::REG_ERROR, &e, 1)) {
    ESP_LOGW(TAG, "%s: ERROR readback failed — result unverified", op_name);
    return false;
  }
  if (e == rbamp_std::DEV_ERR_OK) return true;
  if (e == rbamp_std::DEV_ERR_PARAM) {
    ESP_LOGE(TAG, "%s REJECTED by device: ERR_PARAM (invalid parameter)", op_name);
  } else if (e == rbamp_std::DEV_ERR_UNKNOWN_CMD) {
    ESP_LOGE(TAG, "%s REJECTED by device: ERR_UNKNOWN_CMD (opcode 0x%02X)", op_name, opcode);
  } else {
    ESP_LOGE(TAG, "%s failed: device ERROR=0x%02X", op_name, e);
  }
  return false;
}

bool RbAmpComponent::std_probe_() {
  uint8_t id[10];
  if (!this->std_read_(rbamp_std::REG_ID, id, sizeof(id))) {
    // Transport failure — device absent or bus fault. Let the BASIC probe
    // run and own the error reporting (it also has the address-change
    // self-healing path).
    return false;
  }
  if (id[0] != 'S' || id[1] != 'T') {
    return false;  // BASIC line
  }

  const uint8_t ver = id[rbamp_std::REG_VERSION];
  const uint16_t caps = std_le_u16_(&id[rbamp_std::REG_CAPABILITIES]);
  if (ver < 0x30 || (caps & rbamp_std::CAP_ADDR16) == 0) {
    ESP_LOGE(TAG, "STD magic at 0x%02X but map version 0x%02X / caps 0x%04X not supported",
             this->address_, ver, caps);
    this->mark_failed();
    return true;  // identified as STD — must not fall through to the BASIC path
  }
  if (ver > static_cast<uint8_t>(rbamp_std::MAP_VERSION)) {
    ESP_LOGW(TAG, "STD map version 0x%02X is newer than this component (0x%02X)",
             ver, static_cast<unsigned>(rbamp_std::MAP_VERSION));
  }

  this->std_ = true;
  this->std_version_ = ver;
  this->std_caps_ = caps;
  this->std_status_ = id[rbamp_std::REG_STATUS];
  this->n_voltage_ = id[rbamp_std::REG_N_VOLTAGE];
  this->std_n_current_ = id[rbamp_std::REG_N_CURRENT];
  this->std_variant_ = id[rbamp_std::REG_VARIANT];
  this->firmware_version_ = ver;  // get_firmware_version() surface
  return true;
}

const char *RbAmpComponent::std_variant_str_() const {
  switch (this->std_variant_) {
    case rbamp_std::VARIANT_UI1:  return "UI1";
    case rbamp_std::VARIANT_UI2:  return "UI2";
    case rbamp_std::VARIANT_UI3:  return "UI3";
    case rbamp_std::VARIANT_I1:   return "I1";
    case rbamp_std::VARIANT_I2:   return "I2";
    case rbamp_std::VARIANT_I3:   return "I3";
    case rbamp_std::VARIANT_U2I1: return "U2I1";
    case rbamp_std::VARIANT_SPLIT_PHASE: return "SPLIT_PHASE";
    case rbamp_std::VARIANT_U2I3: return "U2I3";
    case rbamp_std::VARIANT_3PH_3CH: return "3PH_3CH";
    case rbamp_std::VARIANT_3PH_6CH: return "3PH_6CH";
    default:                      return "UNK";
  }
}

void RbAmpComponent::std_setup_() {
  // Channel count is discovery truth on this line: registers of channels at
  // or beyond N_CURRENT are unspecified device-side (unmapped reads = 0xFF),
  // so a YAML slot cannot "over-detect harmlessly" the way it can on BASIC —
  // N_CURRENT wins, extra declared sensor slots simply never publish.
  uint8_t ch = this->std_n_current_;
  if (ch == 0) {
    ESP_LOGE(TAG, "STD module reports 0 current channels");
    this->mark_failed();
    return;
  }
  if (ch > 7) {
    ESP_LOGW(TAG, "STD module has %u current channels; this component exposes the first 7", ch);
    ch = 7;
  }
  this->n_channels_ = ch;
  this->has_voltage_ = this->n_voltage_ > 0;

  ESP_LOGCONFIG(TAG, "  STD module: map=0x%02X variant=%s V=%u I=%u caps=0x%04X status=0x%02X",
                this->std_version_, this->std_variant_str_(), this->n_voltage_,
                this->n_channels_, this->std_caps_, this->std_status_);

  if (this->fleet_gc_enable_request_ != 0xFF || this->group_id_request_valid_ || this->broadcast_latch_) {
    ESP_LOGW(TAG, "fleet/broadcast keys are BASIC-line features — ignored on this device");
  }

  // v1.8 — running image version (u32, distinct from the map VERSION byte).
  // Pre-E18 firmware leaves the register unmapped (reads 0xFF) — treat as
  // unknown. Voltage phase angles + phase sequence shipped in image 0x7A:
  // below that the bytes read 0 (angle 0 / seq "n/a"), which is
  // indistinguishable from a real zero — gate on the image version instead.
  {
    uint8_t vb[4] = {0, 0, 0, 0};
    if (this->std_read_(rbamp_std::REG_FW_VER, vb, 4)) {
      const uint32_t v = std_le_u32_(vb);
      this->std_fw_ver_ = (v == 0xFFFFFFFFUL) ? 0 : v;
    }
  }
  this->std_angles_ok_ = this->std_fw_ver_ >= 0x0001007AUL;

  // v1.8 — on-device energy counters. bit13 = counters exist (Wh/mWh blocks);
  // bit15 = µWh/µvarh blocks (fw 0x7B). With either bit the counter-delta
  // path replaces latch integration entirely; without bit15 the µ values are
  // synthesized from the coarse blocks (mWh×1000 / varh×1e6).
  this->std_ode_ = (this->std_caps_ & (rbamp_std::CAP_ONDEVICE_ENERGY | rbamp_std::CAP_ENERGY_UWH)) != 0;
  this->std_ode_fine_ = (this->std_caps_ & rbamp_std::CAP_ENERGY_UWH) != 0;
  // µvarh (+0xE8/+0xF0) shipped one image AFTER the µWh block: on the image
  // that introduced bit15 those bytes read 0, so feeding reactive_energy from
  // them would freeze the counter (or clamp forever after a coarse value).
  // Reactive therefore gates on bit15 AND the running image — same rule as
  // the sibling libraries ("fine_reactive"); active µWh needs bit15 only.
  this->std_ode_fine_rea_ = this->std_ode_fine_ && this->std_fw_ver_ >= 0x0001007BUL;

  const bool wants_grand = this->energy_grand_import_ != nullptr || this->energy_grand_export_ != nullptr ||
                           this->energy_grand_net_ != nullptr || this->reactive_energy_import_ != nullptr ||
                           this->reactive_energy_export_ != nullptr;
  if (wants_grand && !this->std_ode_) {
    ESP_LOGW(TAG, "energy_total/energy_net/reactive_energy sensors need on-device energy "
                  "counters, which this device does not advertise — they will never publish. "
                  "Per-channel `energy:` keys still work (master-side integration).");
  }
  if ((this->voltage_angle_[1] != nullptr || this->voltage_angle_[2] != nullptr ||
       this->phase_sequence_ != nullptr) &&
      !this->std_angles_ok_) {
    ESP_LOGW(TAG, "voltage_angle_*/phase_sequence sensors need a newer device firmware — "
                  "they will never publish on this device");
  }

  // Config writes (class / CT model / address) BEFORE energy restore + latch
  // primer, mirroring the BASIC setup order — the address change resets the
  // device, so everything else must be persisted first.
  this->std_apply_config_();
  if (this->new_addr_request_ != 0) {
    this->std_apply_address_change_();
    if (this->is_failed()) return;
  }

  // Restore Wh totals from NVS BEFORE publishing anything (HA Energy
  // dashboard treats a drop in total_increasing as a meter reset) — same
  // accumulator + prefs slot as the BASIC path.
  this->load_prefs_();
  for (uint8_t i = 0; i < this->n_channels_; i++) {
    if (this->energy_[i] != nullptr) {
      this->energy_[i]->publish_state((float) this->energy_total_wh_[i]);
    }
    if (i < 3 && this->energy_exported_[i] != nullptr) {
      this->energy_exported_[i]->publish_state((float) this->energy_export_wh_[i]);
    }
  }

  if (this->std_ode_) {
    // Counter path: module-persisted counters make the latch/arm pipeline
    // unnecessary — first counter tick primes the baselines, deltas flow
    // from the second tick on. Restore + publish the grand accumulators.
    this->load_std_energy_prefs_();
    if (this->energy_grand_import_ != nullptr)
      this->energy_grand_import_->publish_state((float) this->std_grand_import_wh_);
    if (this->energy_grand_export_ != nullptr)
      this->energy_grand_export_->publish_state((float) this->std_grand_export_wh_);
    if (this->energy_grand_net_ != nullptr)
      this->energy_grand_net_->publish_state((float) (this->std_grand_import_wh_ - this->std_grand_export_wh_));
    if (this->reactive_energy_import_ != nullptr)
      this->reactive_energy_import_->publish_state((float) this->std_rea_import_varh_);
    if (this->reactive_energy_export_ != nullptr)
      this->reactive_energy_export_->publish_state((float) this->std_rea_export_varh_);
    return;
  }

  // Legacy-firmware energy path. Period metering uses latch bank 0: make
  // sure it accumulates (RMW the arm mask so any user-armed banks stay
  // armed), then issue the same primer latch as BASIC so the first user
  // snapshot covers a full period anchored on the master clock. The latch
  // lands at the module's next window boundary; the finish callback treats
  // the first read as prime-only.
  uint8_t arm = 0;
  if (this->std_read_(rbamp_std::REG_PERIOD_ARM, &arm, 1)) {
    if ((arm & 0x01) == 0) {
      this->std_write_u8_(rbamp_std::REG_PERIOD_ARM, arm | 0x01);
    }
  } else {
    ESP_LOGW(TAG, "PERIOD_ARM read failed — energy accounting may stay empty");
  }
  this->std_start_latch_phase_();
}

void RbAmpComponent::std_apply_config_() {
  bool need_save = false;

  // Sensor class → CFG_CLASS (read-compare-write skip, same as BASIC).
  if (this->sensor_class_request_ != 0xFF) {
    uint8_t cur = 0xFF;
    if (this->std_read_(rbamp_std::REG_CFG_CLASS, &cur, 1) && cur == this->sensor_class_request_) {
      ESP_LOGD(TAG, "CFG_CLASS already 0x%02X — no write needed", cur);
    } else {
      ESP_LOGCONFIG(TAG, "  Writing CFG_CLASS = 0x%02X (was 0x%02X)", this->sensor_class_request_, cur);
      if (this->std_write_u8_(rbamp_std::REG_CFG_CLASS, this->sensor_class_request_)) {
        need_save = true;
      } else {
        ESP_LOGW(TAG, "CFG_CLASS write failed");
      }
    }
  }

  // Global CT model → CFG_CT_MODEL. On this line the byte is an SKU
  // reference (no gain preset is applied device-side) — calibration is
  // per-channel via the CAL blocks, owned by factory tooling.
  if (this->ct_model_ != 0xFF) {
    uint8_t cur = 0xFF;
    if (this->std_read_(rbamp_std::REG_CFG_CT_MODEL, &cur, 1) && cur == this->ct_model_) {
      ESP_LOGD(TAG, "CFG_CT_MODEL already 0x%02X — no write needed", cur);
    } else {
      ESP_LOGCONFIG(TAG, "  Writing CFG_CT_MODEL = 0x%02X (was 0x%02X; reference metadata on this line)",
                    this->ct_model_, cur);
      if (this->std_write_u8_(rbamp_std::REG_CFG_CT_MODEL, this->ct_model_)) {
        need_save = true;
      } else {
        ESP_LOGW(TAG, "CFG_CT_MODEL write failed");
      }
    }
  }

  // Per-channel CT models → CHMAP_MODEL block, gated on CAP_CHMAP_PERSIST
  // (bit 14). Contract (root dca511b): firmware advertises the bit ONLY when
  // CMD_SAVE_CONFIG actually persists the per-channel CHMAP to flash and
  // reloads it on boot — safe on a mixed fleet. Without the bit a write
  // would silently revert on device power-cycle (spec finding E9), the exact
  // failure mode the BASIC v1.6 persist-fix closed. Gate on this shipped,
  // queryable capability, NEVER on a planned map version: the earlier
  // `>= 0x35` gate here keyed on a roadmap batch that shipped without E9.
  bool any_per_channel = false;
  for (uint8_t i = 0; i < 16 && !any_per_channel; i++) {
    any_per_channel = this->ct_models_request_[i] != 0xFF;
  }
  if (any_per_channel) {
    if ((this->std_caps_ & rbamp_std::CAP_CHMAP_PERSIST) == 0) {
      ESP_LOGW(TAG, "ct_models: (per-channel) not supported by this device — it does not "
                    "advertise persistent per-channel model storage, so the write would "
                    "silently revert on device power-cycle. Use the global `ct_model:` "
                    "key. (Keys ignored.)");
    } else {
      for (uint8_t ch = 0; ch < this->n_channels_ && ch < 16; ch++) {
        const uint8_t code = this->ct_models_request_[ch];
        if (code == 0xFF) continue;
        uint8_t cur = 0xFF;
        if (this->std_read_(rbamp_std::CHMAP_MODEL(ch), &cur, 1) && cur == code) continue;
        ESP_LOGCONFIG(TAG, "  Writing CHMAP_MODEL ch%u = 0x%02X (was 0x%02X)", ch, code, cur);
        if (this->std_write_u8_(rbamp_std::CHMAP_MODEL(ch), code)) {
          need_save = true;
        } else {
          ESP_LOGW(TAG, "CHMAP_MODEL ch%u write failed", ch);
        }
      }
    }
  }

  // v1.8 — starting current for energy (CFG_START_UA, u16 µA, fw 0x7B).
  // Below this channel current the module counts no energy (at no load the
  // measured power is noise of either sign and would creep into both
  // counters); instantaneous readings are unaffected. Applies at once,
  // SAVE_CONFIG persists. Range is schema-validated (0 = off, 10..2540 in
  // 10 µA steps) so an invalid value never reaches the bus.
  if (this->start_current_ua_request_ != 0xFFFF) {
    if ((this->std_caps_ & rbamp_std::CAP_ENERGY_UWH) == 0) {
      ESP_LOGW(TAG, "start_current_ua: not supported by this device firmware — key ignored "
                    "(the device counts energy at any current)");
    } else {
      uint8_t cur[2] = {0xFF, 0xFF};
      const uint16_t want = this->start_current_ua_request_;
      if (this->std_read_(rbamp_std::REG_CFG_START_UA, cur, 2) && std_le_u16_(cur) == want) {
        ESP_LOGD(TAG, "CFG_START_UA already %u µA — no write needed", want);
      } else {
        ESP_LOGCONFIG(TAG, "  Writing CFG_START_UA = %u µA (was %u)", want, std_le_u16_(cur));
        if (this->std_write_u16_(rbamp_std::REG_CFG_START_UA, want)) {
          need_save = true;
        } else {
          ESP_LOGW(TAG, "CFG_START_UA write failed");
        }
      }
    }
  }

  if (need_save) {
    if (this->std_cmd_verify_(static_cast<uint8_t>(rbamp_std::CMD_SAVE_CONFIG),
                              rbamp_std::SETTLE_MS_SAVE_CONFIG, "SAVE_CONFIG")) {
      ESP_LOGCONFIG(TAG, "  Device config persisted (SAVE_CONFIG); remove the config keys "
                    "from YAML after verification");
    } else {
      ESP_LOGE(TAG, "  SAVE_CONFIG failed — config applied IN RAM ONLY, reverts on device power-cycle");
    }
  }
}

void RbAmpComponent::std_apply_address_change_() {
  const uint8_t new_addr = this->new_addr_request_;
  if (new_addr < 0x08 || new_addr > 0x77 || new_addr == this->address_) {
    ESP_LOGW(TAG, "Invalid new_address 0x%02X — keeping current 0x%02X", new_addr, this->address_);
    return;
  }
  ESP_LOGCONFIG(TAG, "  Changing I2C address: 0x%02X -> 0x%02X", this->address_, new_addr);

  if (!this->std_write_u8_(rbamp_std::REG_CFG_I2C_ADDR, new_addr)) {
    ESP_LOGW(TAG, "CFG_I2C_ADDR write failed; keeping 0x%02X", this->address_);
    return;
  }
  // SAVE_CONFIG validates the address range device-side (ERR_PARAM on an
  // out-of-range value; other valid CFG fields still persist).
  if (!this->std_cmd_verify_(static_cast<uint8_t>(rbamp_std::CMD_SAVE_CONFIG),
                             rbamp_std::SETTLE_MS_SAVE_CONFIG, "SAVE_CONFIG(addr)")) {
    ESP_LOGE(TAG, "Address change not persisted — keeping 0x%02X", this->address_);
    return;
  }
  // The new address applies on reset. The slave may NACK the tail of the
  // RESET write — advisory.
  this->std_write_u8_(rbamp_std::REG_COMMAND, static_cast<uint8_t>(rbamp_std::CMD_RESET));
  App.feed_wdt();
  delay(200);  // boot-to-I2C is single-digit ms on this line; generous margin
  App.feed_wdt();

  this->set_i2c_address(new_addr);
  uint8_t id[2] = {0, 0};
  if (this->std_read_(rbamp_std::REG_ID, id, 2) && id[0] == 'S' && id[1] == 'T') {
    ESP_LOGCONFIG(TAG, "  Address change confirmed at 0x%02X", new_addr);
    ESP_LOGW(TAG, "IMPORTANT: update YAML to `address: 0x%02X` and remove `new_address:` "
                  "before next boot.", new_addr);
    this->new_addr_request_ = 0;
  } else {
    ESP_LOGE(TAG, "Address change applied but device not responding at 0x%02X — "
                  "check wiring and power-cycle the device.", new_addr);
    this->mark_failed();
  }
}

void RbAmpComponent::std_start_latch_phase_() {
  const uint32_t t_latch = millis();
  if (!this->std_write_u8_(rbamp_std::REG_LATCH_BANK, 0) ||
      !this->std_write_u8_(rbamp_std::REG_COMMAND, static_cast<uint8_t>(rbamp_std::CMD_LATCH_PERIOD))) {
    ESP_LOGW(TAG, "LATCH write failed; skipping period integration this cycle");
    return;
  }
  // The latch is serviced at the module's NEXT RT-window boundary — one
  // window away at most: ~100 ms on images ≥0x7C (whole mains cycles nearest
  // 100 ms), ~160 ms @50 Hz / ~133 ms @60 Hz with a 200 ms publish on older
  // images (this latch path only runs on fw without on-device counters, i.e.
  // the older-window era). A shorter settle would read the PREVIOUS snapshot
  // as fresh; the codegen 300 ms settle is a safe upper bound for every
  // image. PD_VALID is sticky (set-once), NOT a freshness flag — wall-clock
  // wait is the contract. Non-blocking, same pattern as the BASIC latch
  // pipeline.
  this->set_timeout("rbamp_std_period", rbamp_std::SETTLE_MS_LATCH_PERIOD,
                    [this, t_latch]() { this->std_finish_latch_phase_(t_latch); });
}

void RbAmpComponent::std_finish_latch_phase_(uint32_t t_latch) {
  // Stale check: PD_VALID bit0 says bank 0 has EVER latched (sticky). A clear
  // bit this late means the primer/arm failed — surface once per cycle.
  uint8_t pd = 0;
  if (!this->std_read_(rbamp_std::REG_PD_VALID, &pd, 1) || (pd & 0x01) == 0) {
    ESP_LOGD(TAG, "Latch bank 0 has no snapshot yet (PD_VALID=0x%02X)", pd);
    return;
  }

  // One burst: bank header (incl. NSAMP + WIN_CNT) + per-channel E blocks.
  const size_t len = rbamp_std::LATCH_CHE_BASE +
                     static_cast<size_t>(this->n_channels_) * rbamp_std::LATCH_CHE_STRIDE;
  uint8_t b[rbamp_std::LATCH_CHE_BASE + 7 * rbamp_std::LATCH_CHE_STRIDE];
  if (!this->std_read_(rbamp_std::LATCH(0), b, len)) {
    ESP_LOGW(TAG, "Latch bank 0 read failed — keeping previous integration window");
    return;
  }
  const uint32_t nsamp = std_le_u32_(b + rbamp_std::LATCH_NSAMP);
  const uint32_t win_count = std_le_u32_(b + rbamp_std::LATCH_WIN_CNT);
  if (nsamp == 0 || win_count == 0) {
    // The arm mask and bank state live in device RAM: a DUT reset clears
    // them, after which every latch snapshots an un-armed bank (nsamp=0)
    // forever and energy silently freezes.
    // Detect the lost arm, re-arm, and re-prime the integration clocks —
    // the outage interval carries no data, so it is skipped, not estimated.
    uint8_t arm = 0;
    if (this->std_read_(rbamp_std::REG_PERIOD_ARM, &arm, 1) && (arm & 0x01) == 0) {
      ESP_LOGW(TAG, "Latch bank 0 lost its arm (device reset detected) — re-arming; "
                    "one integration window is skipped");
      this->std_write_u8_(rbamp_std::REG_PERIOD_ARM, arm | 0x01);
      for (uint8_t ch = 0; ch < 7; ch++) this->primed_[ch] = false;
    } else {
      ESP_LOGD(TAG, "Latch window empty (nsamp=%u win=%u) — no window elapsed yet",
               nsamp, win_count);
    }
    return;
  }

  bool any_integrated = false;
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    const bool wants_import = this->energy_[ch] != nullptr;
    const bool wants_export = ch < 3 && this->bidirectional_ && this->energy_exported_[ch] != nullptr;
    if (!wants_import && !wants_export) continue;

    // First cycle after boot: prime the clock, do NOT integrate (no dt anchor).
    if (!this->primed_[ch]) {
      this->last_latch_ms_[ch] = t_latch;
      this->primed_[ch] = true;
      continue;
    }
    const uint32_t dt_ms = t_latch - this->last_latch_ms_[ch];
    if (dt_ms == 0) continue;
    const double dt_s = dt_ms / 1000.0;

    // Contract (spec E3): average P over the period = (E_pos + E_neg) / NSAMP
    // [mW]. The device has NO clock — energy = avg P × MASTER wall-clock dt,
    // the same anti-revert canon as BASIC L9. HA total_increasing sensors get
    // the import/export split natively published by the device: E_pos
    // accumulates P≥0 samples, E_neg (≤0) the export samples.
    const uint8_t *che = b + rbamp_std::LATCH_CHE(ch);
    const int64_t e_pos = std_le_i64_(che + rbamp_std::LATCH_CHE_E_POS);
    const int64_t e_neg = std_le_i64_(che + rbamp_std::LATCH_CHE_E_NEG);
    const double avg_import_w = (static_cast<double>(e_pos) / nsamp) * 1e-3;
    const double avg_export_w = (static_cast<double>(-e_neg) / nsamp) * 1e-3;
    if (!std::isfinite(avg_import_w) || avg_import_w < 0.0 || avg_import_w > 50000.0) continue;

    if (wants_import) {
      this->energy_total_wh_[ch] += avg_import_w * dt_s / 3600.0;
      this->energy_[ch]->publish_state((float) this->energy_total_wh_[ch]);
    }
    if (wants_export && std::isfinite(avg_export_w) && avg_export_w >= 0.0 && avg_export_w <= 50000.0) {
      this->energy_export_wh_[ch] += avg_export_w * dt_s / 3600.0;
      this->energy_exported_[ch]->publish_state((float) this->energy_export_wh_[ch]);
    }
    this->last_latch_ms_[ch] = t_latch;
    any_integrated = true;
  }

  if (any_integrated && (this->last_save_ms_ == 0 || (t_latch - this->last_save_ms_) >= SAVE_INTERVAL_MS)) {
    this->save_prefs_();
  }
}

void RbAmpComponent::load_std_energy_prefs_() {
  // Same slot-key scheme as load_prefs_ (namespace ⊕ address ⊕ layout version).
  uint32_t hash = fnv1_hash("rbamp_std_energy") ^ static_cast<uint32_t>(this->address_)
                ^ RBAMP_STD_ENERGY_PREF_VERSION;
  this->std_energy_pref_ = global_preferences->make_preference<RbAmpStdEnergyPrefData>(hash);

  RbAmpStdEnergyPrefData saved{};
  if (!this->std_energy_pref_.load(&saved)) {
    ESP_LOGCONFIG(TAG, "  No saved grand-energy state — starting from 0");
    return;
  }
  if (std::isfinite(saved.grand_import_wh) && saved.grand_import_wh >= 0.0)
    this->std_grand_import_wh_ = saved.grand_import_wh;
  if (std::isfinite(saved.grand_export_wh) && saved.grand_export_wh >= 0.0)
    this->std_grand_export_wh_ = saved.grand_export_wh;
  if (std::isfinite(saved.rea_import_varh) && saved.rea_import_varh >= 0.0)
    this->std_rea_import_varh_ = saved.rea_import_varh;
  if (std::isfinite(saved.rea_export_varh) && saved.rea_export_varh >= 0.0)
    this->std_rea_export_varh_ = saved.rea_export_varh;
  ESP_LOGCONFIG(TAG, "  Restored grand energy: imp=%.3f Wh exp=%.3f Wh rea_imp=%.3f varh rea_exp=%.3f varh",
                this->std_grand_import_wh_, this->std_grand_export_wh_,
                this->std_rea_import_varh_, this->std_rea_export_varh_);
}

void RbAmpComponent::std_energy_counters_update_() {
  const bool wants_grand = this->energy_grand_import_ != nullptr || this->energy_grand_export_ != nullptr ||
                           this->energy_grand_net_ != nullptr || this->reactive_energy_import_ != nullptr ||
                           this->reactive_energy_export_ != nullptr;
  bool wants_ch = false;
  for (uint8_t ch = 0; ch < this->n_channels_ && !wants_ch; ch++) {
    wants_ch = this->energy_[ch] != nullptr ||
               (ch < 3 && this->bidirectional_ && this->energy_exported_[ch] != nullptr);
  }
  if (!wants_grand && !wants_ch) return;

  const uint32_t now = millis();

  // Delta validity context: the life-time generation counter steps on a
  // deliberate counter reset/adopt (no delta may span it), and a decreasing
  // uptime means the module restarted — its counters may step back by the
  // energy accumulated since their last NVM save (delta clamps to 0 then).
  uint8_t gb[4], ub[4];
  if (!this->std_read_(rbamp_std::REG_LT_GEN, gb, 4) ||
      !this->std_read_(static_cast<uint16_t>(rbamp_std::ENERGY(0) + rbamp_std::ENERGY_UPTIME), ub, 4)) {
    ESP_LOGW(TAG, "Energy counter context read failed — skipping this window");
    return;
  }
  const uint32_t gen = std_le_u32_(gb);
  const uint32_t uptime = std_le_u32_(ub);

  // Grand snapshot — ONE coherent transaction so import/export belong to the
  // same instant. Everything is normalized to µ units on ingest.
  int64_t g_imp, g_exp, r_imp, r_exp;
  if (this->std_ode_fine_) {
    uint8_t b[40];  // IMP/EXP/NET µWh + REA IMP/EXP µvarh (0x..D0..0x..F7)
    if (!this->std_read_coherent_(
            static_cast<uint16_t>(rbamp_std::ENERGY(0) + rbamp_std::ENERGY_E_ACT_IMP_UWH), b, 40)) {
      ESP_LOGW(TAG, "Grand energy snapshot read failed — skipping this window");
      return;
    }
    g_imp = std_le_i64_(b + 0);
    g_exp = std_le_i64_(b + 8);
    // NET (b+16) is read for coherence but never delta'd: NET = IMP − EXP by
    // definition, and clamping a NET delta independently would skew it.
    if (this->std_ode_fine_rea_) {
      r_imp = std_le_i64_(b + 24);
      r_exp = std_le_i64_(b + 32);
    } else {
      // bit15 without the µvarh image: +0xE8.. read 0 — take reactive from
      // the coarse varh counters instead (their own coherent pair read).
      uint8_t rb[16];
      if (!this->std_read_coherent_(
              static_cast<uint16_t>(rbamp_std::ENERGY(0) + rbamp_std::ENERGY_E_REA_IMP), rb, 16)) {
        ESP_LOGW(TAG, "Reactive energy snapshot read failed — skipping this window");
        return;
      }
      r_imp = std_le_i64_(rb + 0) * 1000000;
      r_exp = std_le_i64_(rb + 8) * 1000000;
    }
  } else {
    uint8_t b[40];  // REA IMP/EXP varh + IMP/EXP/NET mWh (+0x18..+0x3F)
    if (!this->std_read_coherent_(
            static_cast<uint16_t>(rbamp_std::ENERGY(0) + rbamp_std::ENERGY_E_REA_IMP), b, 40)) {
      ESP_LOGW(TAG, "Grand energy snapshot read failed — skipping this window");
      return;
    }
    r_imp = std_le_i64_(b + 0) * 1000000;   // varh → µvarh
    r_exp = std_le_i64_(b + 8) * 1000000;
    g_imp = std_le_i64_(b + 16) * 1000;     // mWh → µWh
    g_exp = std_le_i64_(b + 24) * 1000;
  }

  // Per-channel snapshot — the whole 16 B/channel block in one transaction.
  int64_t ch_imp[7] = {0}, ch_exp[7] = {0};
  if (wants_ch) {
    uint8_t b[7 * 16];
    const uint16_t base = this->std_ode_fine_ ? rbamp_std::GROUP_ENERGY_UWH(0)
                                              : rbamp_std::GROUP_ENERGY_MWH(0);
    if (!this->std_read_coherent_(base, b, static_cast<size_t>(this->n_channels_) * 16)) {
      ESP_LOGW(TAG, "Per-channel energy snapshot read failed — skipping this window");
      return;
    }
    const int64_t scale = this->std_ode_fine_ ? 1 : 1000;
    for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
      ch_imp[ch] = std_le_i64_(b + ch * 16 + 0) * scale;
      ch_exp[ch] = std_le_i64_(b + ch * 16 + 8) * scale;
    }
  }

  const bool rebase_only = !this->std_cnt_primed_ || gen != this->std_prev_gen_;
  if (this->std_cnt_primed_ && gen != this->std_prev_gen_) {
    ESP_LOGW(TAG, "Energy counter generation changed (%u -> %u, counter reset/adopt) — "
                  "re-basing, one window skipped", this->std_prev_gen_, gen);
  }
  if (!rebase_only && uptime < this->std_prev_uptime_) {
    ESP_LOGW(TAG, "Module restart detected (uptime %u < %u) — counters may step back by the "
                  "unsaved tail; negative deltas clamp to 0", uptime, this->std_prev_uptime_);
  }

  const uint32_t dt_ms = now - this->std_cnt_last_ms_;
  // Glitch guard: a delta implying far more power than the hardware can meter
  // (≈72 kW/channel sustained over the window) is a corrupt read, not energy.
  const int64_t cap_ch = static_cast<int64_t>(dt_ms) * 20000;
  const int64_t cap_grand = cap_ch * 8;
  bool glitch = false;
  auto delta = [&glitch](int64_t now_v, int64_t prev_v, int64_t cap) -> int64_t {
    const int64_t d = now_v - prev_v;
    if (d < 0) return 0;
    if (d > cap) { glitch = true; return 0; }
    return d;
  };

  if (!rebase_only && dt_ms > 0) {
    const int64_t d_imp = delta(g_imp, this->std_prev_grand_imp_, cap_grand);
    const int64_t d_exp = delta(g_exp, this->std_prev_grand_exp_, cap_grand);
    const int64_t d_rimp = delta(r_imp, this->std_prev_rea_imp_, cap_grand);
    const int64_t d_rexp = delta(r_exp, this->std_prev_rea_exp_, cap_grand);
    int64_t d_ch_imp[7] = {0}, d_ch_exp[7] = {0};
    if (wants_ch) {
      for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
        d_ch_imp[ch] = delta(ch_imp[ch], this->std_prev_ch_imp_[ch], cap_ch);
        d_ch_exp[ch] = delta(ch_exp[ch], this->std_prev_ch_exp_[ch], cap_ch);
      }
    }
    if (glitch) {
      ESP_LOGW(TAG, "Implausible energy counter jump — treating as a corrupt read, re-basing");
    } else {
      this->std_grand_import_wh_ += d_imp * 1e-6;
      this->std_grand_export_wh_ += d_exp * 1e-6;
      this->std_rea_import_varh_ += d_rimp * 1e-6;
      this->std_rea_export_varh_ += d_rexp * 1e-6;
      if (this->energy_grand_import_ != nullptr)
        this->energy_grand_import_->publish_state((float) this->std_grand_import_wh_);
      if (this->energy_grand_export_ != nullptr)
        this->energy_grand_export_->publish_state((float) this->std_grand_export_wh_);
      if (this->energy_grand_net_ != nullptr)
        this->energy_grand_net_->publish_state(
            (float) (this->std_grand_import_wh_ - this->std_grand_export_wh_));
      if (this->reactive_energy_import_ != nullptr)
        this->reactive_energy_import_->publish_state((float) this->std_rea_import_varh_);
      if (this->reactive_energy_export_ != nullptr)
        this->reactive_energy_export_->publish_state((float) this->std_rea_export_varh_);
      for (uint8_t ch = 0; ch < this->n_channels_ && wants_ch; ch++) {
        if (this->energy_[ch] != nullptr) {
          this->energy_total_wh_[ch] += d_ch_imp[ch] * 1e-6;
          this->energy_[ch]->publish_state((float) this->energy_total_wh_[ch]);
        }
        if (ch < 3 && this->bidirectional_ && this->energy_exported_[ch] != nullptr) {
          this->energy_export_wh_[ch] += d_ch_exp[ch] * 1e-6;
          this->energy_exported_[ch]->publish_state((float) this->energy_export_wh_[ch]);
        }
      }
      if (this->last_save_ms_ == 0 || (now - this->last_save_ms_) >= SAVE_INTERVAL_MS) {
        this->save_prefs_();
      }
    }
  }

  this->std_prev_gen_ = gen;
  this->std_prev_uptime_ = uptime;
  this->std_prev_grand_imp_ = g_imp;
  this->std_prev_grand_exp_ = g_exp;
  this->std_prev_rea_imp_ = r_imp;
  this->std_prev_rea_exp_ = r_exp;
  for (uint8_t ch = 0; ch < 7; ch++) {
    this->std_prev_ch_imp_[ch] = ch_imp[ch];
    this->std_prev_ch_exp_[ch] = ch_exp[ch];
  }
  this->std_cnt_last_ms_ = now;
  this->std_cnt_primed_ = true;
}

std::string RbAmpComponent::get_phase_sequence_str() {
  if (!this->std_ || !this->std_angles_ok_) return "unavailable";
  uint8_t seq = 0;
  if (!this->std_read_(rbamp_std::REG_PHASE_SEQ, &seq, 1)) return "unavailable";
  switch (seq) {
    case 1: return "forward";
    case 2: return "reverse";
    case 3: return "abnormal";
    default: return "n/a";
  }
}

uint16_t RbAmpComponent::read_start_current_ua() {
  if (!this->std_ || (this->std_caps_ & rbamp_std::CAP_ENERGY_UWH) == 0) return 0xFFFF;
  uint8_t b[2];
  if (!this->std_read_(rbamp_std::REG_CFG_START_UA, b, 2)) return 0xFFFF;
  return std_le_u16_(b);
}

void RbAmpComponent::std_update_() {
  // Period metering — same `latch_interval:` cadence decoupling as BASIC:
  // LATCH every latch_interval_ms_ (or every update() when unset), RT reads
  // below every update(). Wh stays correct at any ratio (master wall-clock dt).
  const uint32_t now = millis();
  const uint32_t latch_gate = this->latch_interval_ms_ > 0
                                  ? this->latch_interval_ms_
                                  : this->get_update_interval();
  if (this->last_latch_wall_ms_ == 0 || (now - this->last_latch_wall_ms_) >= latch_gate) {
    this->last_latch_wall_ms_ = now;
    if (this->std_ode_) {
      this->std_energy_counters_update_();  // v1.8: counter deltas, no latch
    } else {
      this->std_start_latch_phase_();
    }
  }

  // Health gate: STATUS.sensor_ok — the metering front-end is producing frames.
  // (There is no atomic data-ready flag on this line; metrology publishes once
  // per measurement window — ~100 ms on images ≥0x7C, every 200 ms on older
  // images — so any poll interval ≥1 s always sees fresh data when the sensor
  // path is alive.)
  uint8_t status = 0;
  if (!this->std_read_(rbamp_std::REG_STATUS, &status, 1)) {
    ESP_LOGD(TAG, "STD STATUS read failed — skipping publish this cycle");
    return;
  }
  this->std_status_ = status;
  if ((status & rbamp_std::ST_SENSOR_OK) == 0) {
    ESP_LOGD(TAG, "STD sensor path not ready (STATUS=0x%02X) — skipping publish", status);
    return;
  }

  // Mains frequency: u16 cHz at 0x0010, wall-time ZC based, updates ~2 s.
  if (this->frequency_ != nullptr) {
    uint8_t fb[2];
    if (this->std_read_(rbamp_std::REG_FREQ, fb, 2)) {
      const float hz = std_le_u16_(fb) / 100.0f;
      if (hz >= 40.0f && hz <= 70.0f) {
        this->frequency_->publish_state(hz);
      }
    }
  }

  // Voltage channels: i32 mV at VOLTAGE(v). Component exposes up to 3 slots
  // (voltage / voltage_a/b/c map onto voltage_[0..2]).
  const uint8_t nv = this->n_voltage_ > 3 ? 3 : this->n_voltage_;
  for (uint8_t v = 0; v < nv; v++) {
    // v1.8: when the phase angle is wanted (and the firmware computes it),
    // one 12-byte burst covers U_RMS (+0x00, i32 mV) through ANGLE (+0x0A,
    // i16 centi-degree vs phase A; 0x8000 = no angle yet).
    const bool want_angle = this->std_angles_ok_ && this->voltage_angle_[v] != nullptr;
    if (this->voltage_[v] == nullptr && !want_angle) continue;
    uint8_t b[12];
    const size_t len = want_angle ? 12 : 4;
    if (!this->std_read_(static_cast<uint16_t>(rbamp_std::VOLTAGE(v) + rbamp_std::VOLTAGE_U_RMS), b, len)) {
      ESP_LOGW(TAG, "STD U_rms[%u] read failed", v);
      continue;
    }
    const float u = std_le_i32_(b) * 1e-3f;
    if (this->voltage_[v] != nullptr && sanity_voltage_(u)) {
      this->voltage_[v]->publish_state(u);
    }
    if (want_angle) {
      const int16_t raw = std_le_i16_(b + rbamp_std::VOLTAGE_ANGLE);
      if (raw != INT16_MIN) {  // 0x8000 = angle not available (e.g. phase below 50 V)
        this->voltage_angle_[v]->publish_state(raw / 100.0f);
      } else {
        this->voltage_angle_[v]->publish_state(NAN);
      }
    }
  }

  // v1.8 — phase sequence (u8: 0 n/a · 1 forward · 2 reverse · 3 abnormal).
  // 0 means "cannot be determined" (fewer than 3 live voltage inputs) —
  // published as NAN so HA shows "unknown" rather than a fake reading.
  if (this->phase_sequence_ != nullptr && this->std_angles_ok_) {
    uint8_t seq = 0;
    if (this->std_read_(rbamp_std::REG_PHASE_SEQ, &seq, 1)) {
      this->phase_sequence_->publish_state(seq == 0 ? NAN : (float) seq);
    }
  }

  // Current channels: one 18-byte burst per channel covers I_RMS(i32 uA),
  // P(i32 mW), Q(i32 mvar), S(i32 mVA), PF(i16 Q10) — offsets 0x00..0x11
  // within the CURRENT block.
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    const bool wants_ch = this->current_[ch] != nullptr || this->power_[ch] != nullptr ||
                          this->pf_[ch] != nullptr || this->reactive_power_[ch] != nullptr ||
                          (ch == 0 && this->apparent_power_ != nullptr);
    if (!wants_ch) continue;

    uint8_t b[18];
    if (!this->std_read_(rbamp_std::CURRENT(ch), b, sizeof(b))) {
      ESP_LOGW(TAG, "STD channel %u block read failed", ch);
      continue;
    }
    const float i_rms = std_le_i32_(b + rbamp_std::CURRENT_I_RMS) * 1e-6f;
    const float p     = std_le_i32_(b + rbamp_std::CURRENT_P) * 1e-3f;
    const float q     = std_le_i32_(b + rbamp_std::CURRENT_Q) * 1e-3f;
    const float s     = std_le_i32_(b + rbamp_std::CURRENT_S) * 1e-3f;
    const float pf    = std_le_i16_(b + rbamp_std::CURRENT_PF) / 1024.0f;

    if (this->current_[ch] != nullptr && sanity_current_(i_rms)) {
      this->current_[ch]->publish_state(i_rms);
    }
    // P/Q/S/PF need a sampled voltage; on I-only variants they publish 0.
    if (this->has_voltage_) {
      if (this->power_[ch] != nullptr && sanity_power_(p)) {
        this->power_[ch]->publish_state(p);
      }
      if (this->reactive_power_[ch] != nullptr && sanity_power_(q)) {
        this->reactive_power_[ch]->publish_state(q);
      }
      if (this->pf_[ch] != nullptr && sanity_pf_(pf)) {
        this->pf_[ch]->publish_state(pf);
      }
      if (ch == 0 && this->apparent_power_ != nullptr && sanity_power_(s) && s >= 0.0f) {
        this->apparent_power_->publish_state(s);
      }
    }
  }

  // Grand-total active power (TOTALS block, ch-sum on multi-channel
  // variants, mirror of ch0 on single-channel). Only read when declared.
  if (this->power_total_ != nullptr && this->has_voltage_) {
    uint8_t b[4];
    if (this->std_read_(static_cast<uint16_t>(rbamp_std::TOTALS(0) + rbamp_std::TOTALS_P), b, 4)) {
      const float p_tot = std_le_i32_(b) * 1e-3f;
      if (sanity_power_(p_tot)) {
        this->power_total_->publish_state(p_tot);
      }
    }
  }
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
  // Topology hint (YAML `topology:` — SINGLE/SPLIT/THREE) — informational only
  // for the SINGLE/SPLIT/THREE_PHASE enum. Actual channel count now comes from
  // REG_TOPOLOGY on v1.3+ firmware (see below).
  this->topology_ = this->topology_hint_;

  // v1.5 (fw bd2ad8e+): REG_TOPOLOGY 0x24 = number of I-channels {1,2,3,5,7}.
  // 0x00 = v1.0 firmware fallback — derive from YAML sensor slots as before.
  uint8_t reg_topo = 0;
  bool topo_ok = this->read_u8_(REG_TOPOLOGY, &reg_topo);
  if (topo_ok && (reg_topo == 1 || reg_topo == 2 || reg_topo == 3 ||
                  reg_topo == 5 || reg_topo == 7)) {
    // Device-reported truth wins over YAML-slot count.
    this->n_channels_ = reg_topo;
  } else {
    // v1.0 fallback OR unexpected value → derive from declared YAML slots.
    // Walk highest-to-lowest to find topmost populated current_ slot.
    this->n_channels_ = 0;
    for (int8_t ch = 6; ch >= 0; ch--) {
      if (this->current_[ch] != nullptr) {
        this->n_channels_ = ch + 1;
        break;
      }
    }
  }

  // v1.5 has_voltage_ derivation: fw bd2ad8e HW_VARIANT 0x55 codes with U
  // present = {1,2,3,7,8}; without U = {4,5,6}. If HW_VARIANT read fails or
  // v1.0 firmware, fall back to YAML `voltage:` declaration.
  uint8_t hw_variant = 0;
  if (this->read_u8_(REG_HW_VARIANT, &hw_variant)) {
    switch (hw_variant) {
      case 1: case 2: case 3: case 7: case 8:
        this->has_voltage_ = true;
        break;
      case 4: case 5: case 6:
        this->has_voltage_ = false;
        break;
      default:
        // Unknown variant (0x00 v1.0 fallback, or reserved 9+) → YAML truth.
        this->has_voltage_ = (this->voltage_[0] != nullptr);
        break;
    }
  } else {
    this->has_voltage_ = (this->voltage_[0] != nullptr);
  }
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
  // FW_TIER bit2 (bidirectional), REG_PERIOD_AVG_P0 (0xDC) is signed —
  // net energy can legitimately be < 0 (net export). For that tier, replace
  // the `e >= 0.0` guard with `e > -kReasonableLowerBound` so net-export
  // totals survive NVS restore. Currently no-op since BASIC unidirectional
  // is the only shipping tier; the guard reads as "drop garbage" today and
  // becomes "drop garbage but keep negative net" when STANDARD lands.
  // Truth-doc §9.2 HOLD-resolved → revisit signedness handling at that point.
  // v1.5: restore all 7 slots; ch3-6 will be 0.0 on fresh v1.5 prefs, or
  // preserved from prior v1.5+ boots if migrating in-place.
  for (uint8_t ch = 0; ch < 7; ch++) {
    double e = saved.energy_total_wh[ch];
    if (std::isfinite(e) && e >= 0.0) {
      this->energy_total_wh_[ch] = e;
    }
  }
  for (uint8_t ch = 0; ch < 3; ch++) {
    double x = saved.energy_export_wh[ch];
    if (std::isfinite(x) && x >= 0.0) {
      this->energy_export_wh_[ch] = x;
    }
  }
  ESP_LOGCONFIG(TAG,
                "  Restored Wh from NVS: total[%.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f] export[%.3f, %.3f, %.3f]",
                this->energy_total_wh_[0], this->energy_total_wh_[1],
                this->energy_total_wh_[2], this->energy_total_wh_[3],
                this->energy_total_wh_[4], this->energy_total_wh_[5],
                this->energy_total_wh_[6], this->energy_export_wh_[0],
                this->energy_export_wh_[1], this->energy_export_wh_[2]);
}

void RbAmpComponent::save_prefs_() {
  RbAmpPrefData data{};
  for (uint8_t ch = 0; ch < 7; ch++) {
    data.energy_total_wh[ch] = this->energy_total_wh_[ch];
  }
  for (uint8_t ch = 0; ch < 3; ch++) {
    data.energy_export_wh[ch] = this->energy_export_wh_[ch];
  }
  if (this->energy_pref_.save(&data)) {
    this->last_save_ms_ = millis();
    ESP_LOGV(TAG, "Saved Wh totals to NVS");
  } else {
    ESP_LOGW(TAG, "Failed to save Wh totals to NVS — will retry next cycle");
  }

  // v1.8 — grand/reactive accumulators ride the same save cadence (their
  // pref object only exists once load_std_energy_prefs_ ran, i.e. std_ode_).
  if (this->std_ode_) {
    RbAmpStdEnergyPrefData g{};
    g.grand_import_wh = this->std_grand_import_wh_;
    g.grand_export_wh = this->std_grand_export_wh_;
    g.rea_import_varh = this->std_rea_import_varh_;
    g.rea_export_varh = this->std_rea_export_varh_;
    if (!this->std_energy_pref_.save(&g)) {
      ESP_LOGW(TAG, "Failed to save grand-energy totals to NVS — will retry next cycle");
    }
  }
}

// Production firmware refuses flash-writing operations from
// the master. The device must be in its factory-provisioning mode for
// CMD_SAVE_GAINS / address-change / sensor_class writes to take effect.
bool RbAmpComponent::check_factory_mode_(const char *op_name) {
  uint8_t mode = 0;
  if (!this->read_u8_(REG_MODE, &mode)) {
    ESP_LOGW(TAG, "Cannot read REG_MODE; skipping %s", op_name);
    return false;
  }
  if (mode == 0) {
    ESP_LOGW(TAG,
             "%s requires the device's factory-provisioning mode "
             "Standard production modules will refuse "
             "this operation. To enable factory mode, see the device's "
             "hardware documentation or contact the manufacturer.",
             op_name);
    return false;
  }
  return true;
}

// Sensor classes 2 (WIRED_CT) and 3 (BUILTIN_CT) mean the module is
// calibrated as a pair with its fitted sensor. On this firmware line a
// SENSOR_CLASS write (even with the same value) zeroes the per-channel
// models and linearity shapes, and a CT model write replaces the paired
// gain/NF with the generic preset — so class/model writes to such a module
// are refused (write-policy parity with the esp-idf/arduino libraries).
static constexpr uint8_t SENSOR_CLASS_WIRED_CT = 2;
static constexpr uint8_t SENSOR_CLASS_BUILTIN_CT = 3;
static bool sensor_class_is_paired_(uint8_t cls) {
  return cls == SENSOR_CLASS_WIRED_CT || cls == SENSOR_CLASS_BUILTIN_CT;
}

void RbAmpComponent::apply_ct_model_() {
  // S4 read-compare-write: REG_CT_MODEL_CH0 (0x51) mirrors the applied value
  // for ch0, which is what the legacy single-channel path targets. Skip if
  // already matches — firmware re-applies the generic preset on ANY model
  // write, including an unchanged value, so an equal write is NOT harmless.
  // `ct_model_force:` (service) bypasses both the skip and the paired-class
  // refusal: a deliberate preset re-apply.
  uint8_t current_ch0 = 0;
  const bool rb_ok = this->read_u8_(REG_CT_MODEL_CH0, &current_ch0);
  if (!this->ct_model_force_) {
    if (rb_ok && current_ch0 == this->ct_model_) {
      ESP_LOGD(TAG, "CT_MODEL ch0 already 0x%02X — no write needed (read-compare-write skip)", current_ch0);
      return;
    }
    uint8_t cls = 0;
    if (this->read_u8_(REG_SENSOR_CLASS, &cls) && sensor_class_is_paired_(cls)) {
      ESP_LOGW(TAG,
               "ct_model refused: module is calibrated with its fitted sensor (class %u) — "
               "a CT model write would replace that calibration with the generic preset. "
               "Service path: `ct_model_force: true`.",
               cls);
      return;
    }
  } else {
    ESP_LOGCONFIG(TAG, "  ct_model_force: service re-apply — skipping read-compare and paired-class guard");
  }

  if (!this->check_factory_mode_("CT model write")) {
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
  this->ct_model_written_this_boot_ = true;
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
  // already has the requested sensor_class. Firmware zeroes the ch0-2 models
  // and clears the linearity shapes on ANY class write, including an unchanged
  // value — the equal-skip is load-bearing, not an optimization. v1.3
  // read-back is direct (the register holds the applied value); on legacy
  // firmware where REG_SENSOR_CLASS doesn't exist the read succeeds with 0
  // (unmapped reads return 0x00) → requested!=0 proceeds (correct fallback).
  uint8_t current = 0xFF;
  if (!this->read_u8_(REG_SENSOR_CLASS, &current)) {
    ESP_LOGW(TAG, "Cannot read SENSOR_CLASS — skipping sensor_class apply "
                  "(a blind class write could erase a factory-paired calibration)");
    return;
  }
  if (current == this->sensor_class_request_) {
    ESP_LOGD(TAG, "SENSOR_CLASS already 0x%02X — no write needed (read-compare-write skip)",
             current);
    return;
  }

  // A factory-paired module (class WIRED_CT/BUILTIN_CT) never takes a class
  // change from YAML — the write erases its individual calibration. No force
  // path here (parity with esp-idf 1.7.2: re-classing a paired module is a
  // factory operation, not a service one). Note the schema default
  // `sensor_class: SCT_013` makes this line fire on every boot of a paired
  // module — intended visibility, the request is simply ignored.
  if (sensor_class_is_paired_(current)) {
    ESP_LOGW(TAG,
             "sensor_class refused: module is calibrated with its fitted sensor (class %u); "
             "a SENSOR_CLASS write would erase that calibration. Remove `sensor_class:` from "
             "YAML (or keep the default) — re-classing a paired module is a factory operation.",
             current);
    return;
  }

  // UNSET (0) is a legitimate sensor_class value (rare; user-explicit reset
  // before a model swap). Still requires factory mode + flash write.
  if (!this->check_factory_mode_("Sensor class write")) {
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

// v1.6 route-fanout single-site (LESSONS L9): all per-channel CT_MODEL
// I/O passes through these two helpers, so adding future window fields
// (NF, GAIN) reuses the flat-vs-window dispatch instead of copy-pasting a
// guard at each publisher. Both helpers take the FULL channel index (0..15)
// and internally pick the transport by ch<3 vs ch>=3.
//
// v1.4.18+ firmware (root contract 2026-09-04T15:40Z):
//   - Write of CT_MODEL (both paths) applies the preset IN RAM ONLY.
//     Persistence requires one CMD_SAVE_USER_CONFIG after the apply cycle.
//   - Firmware persists the entire ct_model_ch page atomically — a single
//     SAVE at the end covers ch0-15 regardless of which paths were used.
//   - ch<3 : legacy flat path — writes go via REG_CT_MODEL scratch +
//     CMD_SET_CT_MODEL_CH0/1/2 opcode; reads go via REG_CT_MODEL_CH0..2
//     (0x51-0x53). Backwards-compat with v1.0/v1.3 firmware where the
//     window's field 15 does not exist.
//   - ch>=3 : window path — writes go via CHFIELD_CT_MODEL (field 15),
//     reads via same field. RAM-only until SAVE.
//
// Read-back verify (root RELEASE contract §6): every write is confirmed by
// a readback of the same channel. Mismatch → surface ERR_PARAM to the user
// via ESP_LOGE, do NOT mark_failed() (so the rest of the fleet keeps
// booting even if one CT-model line is wrong).
static constexpr uint8_t CT_MODEL_PER_CH_CMD_[3] = {
    CMD_SET_CT_MODEL_CH0,
    CMD_SET_CT_MODEL_CH1,
    CMD_SET_CT_MODEL_CH2,
};

void RbAmpComponent::apply_ct_models_per_channel_() {
  // Read-compare-write skip: if every requested channel already matches the
  // device-side state, avoid the multi-second flash-erase sequence entirely.
  // Uses the same ct_model_read_ router as post-write verify, so the flat
  // vs window dispatch is single-site.
  const uint8_t n = this->n_channels_ > 0 ? this->n_channels_ : 1;
  if (!this->ct_model_force_) {
    uint8_t current[16] = {0};
    bool readback_ok = true;
    for (uint8_t ch = 0; ch < n && ch < 16; ch++) {
      if (!this->ct_model_read_(ch, &current[ch])) {
        readback_ok = false;
        break;
      }
    }
    if (readback_ok) {
      bool all_match = true;
      for (uint8_t ch = 0; ch < n && ch < 16; ch++) {
        const uint8_t requested = this->ct_models_request_[ch];
        if (requested == 0xFF) continue;
        if (current[ch] != requested) { all_match = false; break; }
      }
      if (all_match) {
        ESP_LOGD(TAG, "Per-channel CT models already match device state "
                 "(n_channels=%u) — no write needed (read-compare-write skip)", n);
        return;
      }
      ESP_LOGCONFIG(TAG, "  Per-channel CT models differ from device state — applying writes (n_channels=%u)", n);
    } else {
      ESP_LOGD(TAG, "CT model readback unavailable (legacy firmware or bus error) — applying writes unconditionally");
    }

    // Same paired-class refusal as the global ct_model path: a model write
    // replaces the per-unit/paired gain+NF with the generic preset.
    uint8_t cls = 0;
    if (this->read_u8_(REG_SENSOR_CLASS, &cls) && sensor_class_is_paired_(cls)) {
      ESP_LOGW(TAG,
               "ct_models refused: module is calibrated with its fitted sensor (class %u) — "
               "per-channel CT model writes would replace that calibration with generic "
               "presets. Service path: `ct_model_force: true`.",
               cls);
      return;
    }
  } else {
    ESP_LOGCONFIG(TAG, "  ct_model_force: service re-apply — skipping read-compare and paired-class guard (n_channels=%u)", n);
  }

  if (!this->check_factory_mode_("Per-channel CT model write")) return;

  // Warn on stale YAML slots past the detected channel count. Non-fatal —
  // a UI7 YAML applied to a UI1 board should still boot with ch0 written
  // and slots ch1..ch6 politely ignored.
  for (uint8_t ch = n; ch < 16; ch++) {
    if (this->ct_models_request_[ch] != 0xFF) {
      ESP_LOGW(TAG,
               "  ct_models[%u] = 0x%02X set in YAML but variant has only %u channel(s) — slot ignored",
               ch, this->ct_models_request_[ch], n);
    }
  }

  // Descending order is preserved from the pre-v1.6 impl (protects ch0's
  // final-write state against mid-sequence master reset on multi-channel
  // parts). Router itself picks flat vs window; the loop is unaware.
  bool any_write_applied = false;
  for (int16_t ch = static_cast<int16_t>(n) - 1; ch >= 0; --ch) {
    const uint8_t code = this->ct_models_request_[ch];
    if (code == 0xFF) continue;

    const bool via_window = (ch >= 3);
    ESP_LOGCONFIG(TAG, "  Writing CT_MODEL ch%d = 0x%02X (%s path)", ch, code,
                  via_window ? "window field 15" : "flat 0x51+CMD");

    const bool write_ok = via_window
        ? this->ct_model_write_via_window_(static_cast<uint8_t>(ch), code)
        : this->ct_model_write_flat_(static_cast<uint8_t>(ch), code);
    if (!write_ok) {
      ESP_LOGW(TAG, "  CT_MODEL ch%d write returned bus error — skipping verify", ch);
      continue;
    }

    // Read-back verify per root contract §6. Mismatch = ERR_PARAM 0xFE
    // (firmware kept prior model). Surface without mark_failed — one bad
    // CT-model row shouldn't stop the rest of the fleet.
    uint8_t applied = 0;
    if (!this->ct_model_read_(static_cast<uint8_t>(ch), &applied)) {
      ESP_LOGW(TAG, "  CT_MODEL ch%d readback failed — cannot confirm apply", ch);
      continue;
    }
    if (applied != code) {
      ESP_LOGE(TAG,
               "  CT_MODEL ch%d write REJECTED by firmware: requested 0x%02X, "
               "applied 0x%02X (unchanged) — ERR_PARAM. Check sensor_class + "
               "model code against libs/spec/sensor_models.yaml.",
               ch, code, applied);
      continue;
    }
    ESP_LOGCONFIG(TAG, "  CT_MODEL ch%d = 0x%02X confirmed by readback", ch, code);
    any_write_applied = true;
  }

  // Persist the whole ct_model_ch page in one shot after the apply cycle.
  // Firmware serializes the write; a per-channel SAVE would be 3× 700 ms
  // flash erase for no benefit, and the window path (ch>=3) is RAM-only
  // without this step — omitting SAVE causes silent zeroed models on the
  // senior channels after any DUT power cycle (root contract 2026-09-04T15:40Z).
  if (any_write_applied) {
    if (this->save_user_config_()) {
      ESP_LOGCONFIG(TAG, "  Per-channel CT models persisted (single SAVE_USER_CONFIG); "
                    "remove `ct_models:` from YAML after verification");
      this->ct_model_written_this_boot_ = true;
    } else {
      ESP_LOGE(TAG, "  SAVE_USER_CONFIG failed after apply cycle — CT models APPLIED "
               "IN RAM ONLY and will revert on DUT power-cycle. Investigate.");
    }
  }
}

// Flat-path CT_MODEL write for ch0..2. RAM-only apply (REG_CT_MODEL
// scratch → CMD_SET_CT_MODEL_CHn → settle); persistence is caller's
// responsibility via a single trailing save_user_config_() covering the
// whole ct_model_ch page. Only used from apply_ct_models_per_channel_.
bool RbAmpComponent::ct_model_write_flat_(uint8_t ch, uint8_t code) {
  if (ch >= 3) return false;
  if (!this->write_u8_(REG_CT_MODEL, code)) {
    ESP_LOGW(TAG, "  Write of REG_CT_MODEL=0x%02X for ch%u failed", code, ch);
    return false;
  }
  if (!this->write_u8_(REG_COMMAND, CT_MODEL_PER_CH_CMD_[ch])) {
    ESP_LOGW(TAG, "  CMD_SET_CT_MODEL_CH%u failed", ch);
    return false;
  }
  delay(SETTLE_MS_SET_CT_MODEL_CHN);
  return true;
}

// Window-path CT_MODEL write for ch3..15 (v1.6, requires fw v1.4.18+).
// RAM-only apply — persistence is caller's responsibility.
bool RbAmpComponent::ct_model_write_via_window_(uint8_t ch, uint8_t code) {
  return this->write_channel_window_u8_(CHFIELD_CT_MODEL, ch, code);
}

// Internal router: read the currently-applied CT model for `ch`. ch<3 uses
// the flat mirrors 0x51-0x53 (legacy-firmware compatible), ch>=3 uses the
// window field 15.
bool RbAmpComponent::ct_model_read_(uint8_t ch, uint8_t *out) {
  if (ch < 3) {
    return this->read_u8_(REG_CT_MODEL_CH0 + ch, out);
  }
  return this->read_channel_window_u8_lo_(CHFIELD_CT_MODEL, ch, out);
}

bool RbAmpComponent::read_applied_gain_(uint8_t ch, float *out) {
  static constexpr uint8_t GAIN_ADDR[3] = {REG_I0_GAIN, REG_I1_GAIN, REG_I2_GAIN};
  if (ch < 3) {
    return this->read_float_le_(GAIN_ADDR[ch], out);
  }
  return this->read_channel_window_float_(CHFIELD_GAIN, ch, out);
}

bool RbAmpComponent::basic_reset_verify_uid_() {
  // UID (12 bytes) before the reset. On v1.0-v1.2 firmware the block is
  // unmapped and reads zeros — the compare then passes trivially, which is
  // fine: single-module buses are the norm there.
  uint8_t uid_before[12] = {0}, uid_after[12] = {0};
  for (uint8_t i = 0; i < 12; i++) {
    if (!this->read_u8_(REG_UID + i, &uid_before[i])) {
      ESP_LOGW(TAG, "UID read before reset failed — skipping reset flow");
      return false;
    }
  }
  if (!this->write_u8_(REG_COMMAND, CMD_RESET)) {
    ESP_LOGW(TAG, "CMD_RESET write failed");
    return false;
  }
  App.feed_wdt();
  delay(SETTLE_MS_RESET);
  App.feed_wdt();
  // Re-probe: the module must answer with a sane version byte again.
  uint8_t ver = 0;
  bool alive = false;
  for (uint8_t attempt = 0; attempt < 5 && !alive; attempt++) {
    alive = this->read_u8_(REG_VERSION, &ver) && ver != 0;
    if (!alive)
      delay(100);
  }
  if (!alive) {
    ESP_LOGE(TAG, "Module did not come back after reset");
    return false;
  }
  for (uint8_t i = 0; i < 12; i++) {
    if (!this->read_u8_(REG_UID + i, &uid_after[i])) {
      ESP_LOGW(TAG, "UID read after reset failed");
      return false;
    }
  }
  if (std::memcmp(uid_before, uid_after, 12) != 0) {
    ESP_LOGE(TAG, "A DIFFERENT module answered after the reset (UID changed) — "
                  "address collision on the bus?");
    return false;
  }
  return true;
}

void RbAmpComponent::sct_rating_update_() {
  namespace sct = sct_rating;
  for (uint8_t ch = 0; ch < 7; ch++)
    this->sct_scale_[ch] = 1.0f;
  this->sct_enabled_ = false;
  if (this->std_)
    return;

  // SENSOR_CLASS 1 (SCT-013) only. Class 2/3 modules are calibrated as a
  // pair with their fitted sensor — the model byte is metadata, nothing to
  // correct. Unreadable register (legacy fw, class implicitly SCT) → the
  // model table still decides: unknown model codes come back s = 1.
  uint8_t cls = 1;
  if (this->read_u8_(REG_SENSOR_CLASS, &cls) && cls != 1) {
    ESP_LOGD(TAG, "SCT rating correction: sensor class %u — not applicable", cls);
    return;
  }
  this->sct_enabled_ = true;

  // Firmware that performs the rating switch on-device advertises
  // CAP_SCT_RATING_SELF (firmware 1.4.20 and later) — then s = 1
  // and a plain model write is enough. Shipped images ≤ 1.4.19 don't set it,
  // so the master-side correction applies there.
  const bool fw_handles = this->check_capability_(CAP_SCT_RATING_SELF);

  bool reset_done = false;
  for (uint8_t ch = 0; ch < this->n_channels_ && ch < 7; ch++) {
    uint8_t model = 0;
    float g_a = 0.0f;
    if (!this->ct_model_read_(ch, &model) || !this->read_applied_gain_(ch, &g_a)) {
      ESP_LOGW(TAG, "SCT rating ch%u: model/gain read failed — no correction (x1)", ch);
      continue;
    }
    auto d = sct::decide(model, g_a, fw_handles, reset_done);
    if (d.action == sct::Action::NEED_RESET) {
      // The module applies the raw preset: the channel was switched in this
      // boot (by us or by another master), so the factory trim is not in
      // play. One reset restores the flash state; then re-run every channel.
      ESP_LOGW(TAG, "SCT rating ch%u: model %u applies its raw preset (switched this "
                    "boot) — resetting the module once to restore the factory trim",
               ch, model);
      if (!this->basic_reset_verify_uid_()) {
        ESP_LOGE(TAG, "SCT rating: reset flow failed — corrections stay at x1");
        return;
      }
      reset_done = true;
      for (uint8_t c = 0; c < 7; c++)
        this->sct_scale_[c] = 1.0f;
      ch = static_cast<uint8_t>(-1);  // restart the scan from ch0
      continue;
    }
    switch (d.action) {
      case sct::Action::WARN_UNCAL:
        ESP_LOGW(TAG, "SCT rating ch%u: gain is still the raw preset after a reset — "
                      "uncalibrated module, applying rating correction only (x%.4f)",
                 ch, d.s);
        break;
      case sct::Action::WARN_GAIN:
        ESP_LOGW(TAG, "SCT rating ch%u: unexpected factory gain %.4f (not within 5%% of "
                      "the cal-model value) — no correction (x1)", ch, g_a);
        break;
      case sct::Action::APPLY:
        if (d.s != 1.0f) {
          ESP_LOGCONFIG(TAG, "  SCT rating ch%u: model %u on a cal-model-calibrated "
                             "module — readings scaled x%.4f", ch, model, d.s);
        }
        break;
      default:
        break;
    }
    this->sct_scale_[ch] = d.s;
  }
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

  if (!this->check_factory_mode_("Fleet config write")) {
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
  if (this->std_) {
    ESP_LOGW(TAG, "transmit_gc_frame() is a BASIC-line feature — skipped on this device");
    return false;
  }
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
  if (this->std_) return this->std_variant_str_();
  uint8_t v = 0;
  if (!this->read_u8_(REG_HW_VARIANT, &v)) return "UNK";
  switch (v) {
    case 1: return "UI1";
    case 2: return "UI2";
    case 3: return "UI3";
    case 4: return "I1";
    case 5: return "I2";
    case 6: return "I3";
    case 7: return "UI5";
    case 8: return "UI7";
    default: return "UNK";
  }
}

std::string RbAmpComponent::get_capability_hex() {
  char buf[8];
  if (this->std_) {
    snprintf(buf, sizeof(buf), "0x%04X", this->std_caps_);
    return std::string(buf);
  }
  this->check_capability_(0);  // populate cached_capability_ as side-effect
  snprintf(buf, sizeof(buf), "0x%04X", this->cached_capability_);
  return std::string(buf);
}

std::string RbAmpComponent::get_uid_hex() {
  uint8_t buf[12];
  const bool ok = this->std_
      ? this->std_read_(rbamp_std::REG_UID, buf, 12)
      : this->read_register(REG_UID, buf, 12) == i2c::ERROR_OK;
  if (!ok) {
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
  if (this->std_) {
    if (!this->std_read_(rbamp_std::REG_ERROR, &e, 1)) return "UNREADABLE";
    switch (e) {
      case rbamp_std::DEV_ERR_OK:          return "OK";
      case rbamp_std::DEV_ERR_PARAM:       return "ERR_PARAM";
      case rbamp_std::DEV_ERR_UNKNOWN_CMD: return "ERR_UNKNOWN_CMD";
      default: {
        char buf[16];
        snprintf(buf, sizeof(buf), "ERR_0x%02X", e);
        return std::string(buf);
      }
    }
  }
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
  if (this->std_) return 0;  // no EVENT_FLAGS register on the 16-bit line
  uint8_t f = 0;
  if (!this->read_u8_(REG_EVENT_FLAGS, &f)) return 0;
  return f;
}

bool RbAmpComponent::write_clear_error() {
  if (this->std_) {
    // Map 0x34+ auto-clears ERROR on each command dispatch — nothing to do.
    ESP_LOGD(TAG, "clear_error is a no-op on the 16-bit line (ERROR auto-clears per command)");
    return false;
  }
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
  if (this->std_) {
    const uint8_t op = static_cast<uint8_t>(rbamp_std::CMD_RESET);
    this->write_register16(rbamp_std::REG_COMMAND, &op, 1);
    return true;
  }
  this->write_u8_(REG_COMMAND, CMD_RESET);
  return true;
}

bool RbAmpComponent::fleet_apply_now() {
  if (this->std_) {
    ESP_LOGW(TAG, "fleet_apply_now() is a BASIC-line feature — skipped on this device");
    return false;
  }
  this->apply_fleet_config_();
  return true;
}

uint16_t RbAmpComponent::read_gc_tick_received() {
  if (this->std_) return 0xFFFF;  // BASIC-line feature
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
  if (!two_phase && !this->check_factory_mode_("Address change (legacy single-phase)")) {
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
    // SCT rating-switch correction — the frozen average power is current-
    // gain-proportional, so the energy integration scales with it.
    const float sct_s = this->sct_scale_[ch];
    float avg_p = 0.0f;
    bool avg_p_ok;
    if (ch < 3) {
      // Flat block path (backwards compat)
      avg_p_ok = this->read_float_le_(AVG_P_ADDR[ch], &avg_p) && sanity_power_(avg_p * sct_s);
      if (!avg_p_ok) {
        ESP_LOGW(TAG, "Failed to read or validate PERIOD_AVG_P[%u] at 0x%02X — channel %u stays at previous integration window",
                 ch, AVG_P_ADDR[ch], ch);
        continue;
      }
    } else {
      // v1.5 — Channel access window: field 13 = CHFIELD_PERIOD_AVG_P holds
      // the CMD_LATCH_PERIOD-frozen average power per channel (senior SKUs
      // UI5/UI7 ch3-6). Chip-side atomic latch on same instant as flat ch0-2.
      avg_p_ok = this->read_channel_window_float_(CHFIELD_PERIOD_AVG_P, ch, &avg_p)
                 && sanity_power_(avg_p * sct_s);
      if (!avg_p_ok) {
        ESP_LOGW(TAG, "Failed to read or validate PERIOD_AVG_P via window field=13 ch=%u — channel stays at previous integration window", ch);
        continue;
      }
    }
    avg_p *= sct_s;

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

    // Bidirectional export — flat-block-only path (senior SKUs UI5/UI7 do not
    // expose per-channel export in the current firmware; feature scope stays
    // on primary channel ch0-2).
    if (ch < 3 && this->bidirectional_ && REG_PERIOD_AVG_P_NEG[ch] != 0xFF) {
      float avg_p_neg = 0.0f;
      if (this->read_float_le_(REG_PERIOD_AVG_P_NEG[ch], &avg_p_neg) && sanity_power_(avg_p_neg * sct_s)) {
        avg_p_neg *= sct_s;
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

  // STD identity probe FIRST — order is mandatory (see the STD block comment
  // in rbamp.h): a lone 8-bit pointer byte is taken by an STD slave as the
  // pointer HIGH byte, so the BASIC probe would false-pass on an STD module.
  if (this->std_probe_()) {
    if (this->is_failed()) return;  // STD magic but unsupported map version
    this->std_setup_();
    return;  // BASIC setup path (appliers/prefs/latch primer) is 8-bit-only
  }

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
    // energy_exported_ / energy_export_wh_ are [3] (export is a ch0-2
    // feature) while n_channels_ reaches 7 on UI5/UI7 — the ch<3 gate is a
    // bounds guard, same as every other export index site.
    if (ch < 3 && this->energy_exported_[ch] != nullptr) {
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

  // SCT rating switch (family spec): a model write leaves the module in the
  // same-boot preset state (factory trim lost, seeded noise floor), so the
  // canonical flow is SAVE → RESET → probe + same UID — do it whenever a
  // model was actually written this boot (covers switching back to the cal
  // model too, which the gain probe below cannot distinguish). Then compute
  // the per-channel correction from the gain the module actually applies.
  if (this->ct_model_written_this_boot_ && !this->check_capability_(CAP_SCT_RATING_SELF)) {
    ESP_LOGCONFIG(TAG, "  CT model changed this boot — resetting the module to its flash state");
    if (!this->basic_reset_verify_uid_()) {
      ESP_LOGE(TAG, "Reset after the CT model write failed — the module stays in the "
                    "same-boot state until its next power cycle");
    }
  }
  this->sct_rating_update_();

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

  if (this->std_) {
    this->std_update_();
    return;  // BASIC latch/RT path below is 8-bit-only
  }

  // --- Period metering (LATCH cadence) ---
  // Master-side integration: E_Wh[ch] += avg_p_W[ch] × master_dt_s / 3600.
  // start_latch_phase_() issues the LATCH and schedules a non-blocking
  // 50 ms timeout to read the snapshot — the rest of update() proceeds
  // immediately so the cooperative scheduler is not starved.
  //
  // v1.4: gate LATCH on `latch_interval_ms_` if the user set it (else fall
  // back to per-update() LATCH, v1.3 behaviour). RT-block read below is
  // NOT gated — it runs every update() regardless, so V/I/P/PF/Q stay fresh
  // even when LATCH is slow. Wh integration remains correct because
  // finish_latch_phase_ uses the actual wall-clock dt (last_latch_ms_[ch])
  // — long LATCH intervals just mean coarser integration windows, not lost
  // energy.
  // SCT rating correction: a module restart reverts the applied gain to the
  // flash state (and may carry a model change made by another master), so a
  // backwards uptime recomputes the scales. UPTIME_S is unmapped on older
  // firmware (reads 0 constantly — never "decreases"), which degrades to
  // compute-at-boot-only there.
  if (this->sct_enabled_) {
    uint8_t ub[4];
    bool up_ok = true;
    for (uint8_t i = 0; i < 4 && up_ok; i++)
      up_ok = this->read_u8_(REG_UPTIME_S + i, &ub[i]);
    if (up_ok) {
      const uint32_t up = static_cast<uint32_t>(ub[0]) | (static_cast<uint32_t>(ub[1]) << 8) |
                          (static_cast<uint32_t>(ub[2]) << 16) | (static_cast<uint32_t>(ub[3]) << 24);
      // up == 0 is "unmapped register" (legacy fw streams zeros) — never prime
      // on it, or every later zero would read as a restart.
      if (up != 0) {
        if (this->basic_prev_uptime_s_ != 0 && up < this->basic_prev_uptime_s_) {
          ESP_LOGW(TAG, "Module restart detected (uptime %u -> %u s) — recomputing SCT "
                        "rating correction", this->basic_prev_uptime_s_, up);
          this->sct_rating_update_();
        }
        this->basic_prev_uptime_s_ = up;
      }
    }
  }

  const uint32_t now = millis();
  const uint32_t latch_gate = this->latch_interval_ms_ > 0
                                  ? this->latch_interval_ms_
                                  : this->get_update_interval();
  if (this->last_latch_wall_ms_ == 0 ||
      (now - this->last_latch_wall_ms_) >= latch_gate) {
    this->last_latch_wall_ms_ = now;
    this->start_latch_phase_();
  }

  // --- Real-time block (0x86..0xCF, refreshed every ~100 ms by the device
  // on fw v1.2+ — all shipped batches; ~200 ms on pre-v1.2 images) ---
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

  // Flat block covers ch0-2 (senior SKUs ch3-6 read through the channel window
  // below — see read_channel_window_float_ + rbamp_const.h Channel access
  // window block).
  static constexpr uint8_t I_RMS_ADDR[3] = {REG_I0_RMS, REG_I1_RMS, REG_I2_RMS};
  static constexpr uint8_t P_REAL_ADDR[3] = {REG_P0_REAL, REG_P1_REAL, REG_P2_REAL};
  static constexpr uint8_t PF_ADDR[3] = {REG_PF0, REG_PF1, REG_PF2};
  static constexpr uint8_t Q_ADDR[3] = {REG_Q0, REG_Q1, REG_Q2};

  float i_rms_ch0 = 0.0f;
  bool i_rms_ch0_ok = false;
  for (uint8_t ch = 0; ch < this->n_channels_; ch++) {
    // SCT rating-switch correction: scale BEFORE the sanity bounds so the
    // bounds compare true amps of the fitted sensor (x1 unless the model was
    // switched away from the cal model). U and PF never scale.
    const float sct_s = this->sct_scale_[ch];
    float i_rms = 0.0f;
    bool i_rms_ok;
    if (ch < 3) {
      // Flat block path (backwards compat — single fast burst on shared bus)
      i_rms_ok = this->read_float_le_(I_RMS_ADDR[ch], &i_rms);
    } else {
      // Channel access window (senior SKU ch3-6): select + read, re-select every call
      i_rms_ok = this->read_channel_window_float_(CHFIELD_I_RMS, ch, &i_rms);
    }
    if (i_rms_ok) {
      i_rms *= sct_s;
      i_rms_ok = sanity_current_(i_rms);
    }
    if (i_rms_ok) {
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
      bool p_ok = (ch < 3)
                  ? this->read_float_le_(P_REAL_ADDR[ch], &p_real)
                  : this->read_channel_window_float_(CHFIELD_P_REAL, ch, &p_real);
      p_real *= sct_s;
      if (p_ok && sanity_power_(p_real) && this->power_[ch] != nullptr) {
        this->power_[ch]->publish_state(p_real);
      }

      float pf = 0.0f;
      bool pf_ok = (ch < 3)
                   ? this->read_float_le_(PF_ADDR[ch], &pf)
                   : this->read_channel_window_float_(CHFIELD_PF, ch, &pf);
      if (pf_ok && sanity_pf_(pf) && this->pf_[ch] != nullptr) {
        this->pf_[ch]->publish_state(pf);
      }

      // Reactive power (Q) — only exposed in the flat block (ch0-2). Not
      // available via the public channel window (fields 4-12 are not used
      // here). Skip Q publish on ch3+ — schema-declared sensors on
      // those slots stay at NaN (HA renders "unknown"). Documented behaviour.
      if (ch < 3) {
        float q = 0.0f;
        if (this->read_float_le_(Q_ADDR[ch], &q) && sanity_power_(q * sct_s)
            && this->reactive_power_[ch] != nullptr) {
          this->reactive_power_[ch]->publish_state(q * sct_s);
        }
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
  if (this->std_) {
    ESP_LOGCONFIG(TAG, "  Line: 16-bit register map (map 0x%02X)", this->std_version_);
    ESP_LOGCONFIG(TAG, "  Variant: %s, voltage channels: %u, current channels: %u",
                  this->std_variant_str_(), this->n_voltage_, this->n_channels_);
    ESP_LOGCONFIG(TAG, "  Capabilities: 0x%04X, status: 0x%02X", this->std_caps_, this->std_status_);
    if (this->std_fw_ver_ != 0) {
      ESP_LOGCONFIG(TAG, "  Image: 0x%08X", (unsigned) this->std_fw_ver_);
    }
    ESP_LOGCONFIG(TAG, "  Energy source: %s",
                  this->std_ode_ ? (this->std_ode_fine_ ? "on-device counters (uWh)"
                                                        : "on-device counters (mWh, coarse)")
                                 : "master-side integration (latch)");
    if (this->std_ode_fine_ && !this->std_ode_fine_rea_) {
      ESP_LOGCONFIG(TAG, "  Reactive energy: coarse (varh) — this image has no uvarh counters");
    }
    if (this->start_current_ua_request_ != 0xFFFF) {
      ESP_LOGCONFIG(TAG, "  Start current: %u uA", this->start_current_ua_request_);
    }
    LOG_UPDATE_INTERVAL(this);
    return;
  }
  ESP_LOGCONFIG(TAG, "  Firmware version: 0x%02X", this->firmware_version_);
  for (uint8_t ch = 0; ch < this->n_channels_ && ch < 7; ch++) {
    if (this->sct_scale_[ch] != 1.0f) {
      ESP_LOGCONFIG(TAG, "  SCT rating correction ch%u: x%.4f", ch, this->sct_scale_[ch]);
    }
  }
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
  if (this->latch_interval_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  LATCH interval: %ums (RT reads stay at update_interval)",
                  this->latch_interval_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  LATCH interval: (tied to update_interval — v1.3 default)");
  }
}

}  // namespace rbamp
}  // namespace esphome
