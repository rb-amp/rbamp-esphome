#pragma once

/* ============================================================================
 * rbAmp v1.3 wire-contract constants — MANUAL SYNC
 *
 * Source-of-truth: libs/esp_idf/components/rbamp/include/rbamp_registers_v2.h
 *   source SHA-256: 68071f9f29b0c78ea97ed66e7d26193a742dd0d04a5a3cd020fde65848d731d7
 *   schema CRC32:   0x5FB3E9F3 (RBAMP_V2_REG_SCHEMA_CRC32_V2)
 *   protocol:       0x0103 (v1.3)
 *
 * This header is a HAND-WRITTEN SUBSET tailored to the ESPHome external
 * component's needs. The ESPHome component is NOT a codegen target (native
 * I²C via i2c::I2CDevice, NOT a wrapper around libs/esp_idf/components/rbamp/);
 * keeping the table small + named per ESPHome conventions instead of mirroring
 * RBAMP_V2_* prefixes.
 *
 * To re-sync after upstream schema bump:
 *   1. Run: sha256sum libs/esp_idf/components/rbamp/include/rbamp_registers_v2.h
 *   2. Compare to the SHA above. If different, audit the diff and update
 *      this file + bump the SHA comment.
 *   3. If the upstream schema CRC also changed, that's the API-BREAK signal —
 *      revisit codegen-tier coordination via baton to root.
 *
 * If a register/command needed by ESPHome is missing from this file, ADD IT
 * here from upstream — do NOT inline magic constants in rbamp.cpp.
 * ============================================================================ */

#include <cstdint>

namespace esphome {
namespace rbamp {

// ---- Status / control / version ---------------------------------------------
static constexpr uint8_t REG_STATUS              = 0x00;  // bit0=READY, bit1=ERROR, bit2=EVENTS_PENDING
static constexpr uint8_t REG_COMMAND             = 0x01;  // Write CMD_* opcode
static constexpr uint8_t REG_ERROR               = 0x02;  // 0x00=OK; 0xFA..0xFF error classes (ERR_CLONE 0xF9 v1.3)
static constexpr uint8_t REG_VERSION             = 0x03;  // 0x01=v1.0 .. 0x04=v1.3
static constexpr uint8_t REG_MODE                = 0x04;  // 0=production, 1=develop (factory-provisioning mode)

// ---- CT model / sensor class -----------------------------------------------
static constexpr uint8_t REG_CT_MODEL            = 0x05;  // Scratch: write code then CMD_SET_CT_MODEL_CHn
static constexpr uint8_t REG_PHASE_SAMPLES       = 0x06;  // U-vs-I sample advance (develop-gated write)
static constexpr uint8_t REG_PERIOD_VALID        = 0x07;  // Level flag (NOT cleared-on-read)
static constexpr uint8_t REG_SENSOR_CLASS        = 0x25;  // 0=UNSET, 1=SCT_013, 2=WIRED_CT, 3=BUILTIN_CT
static constexpr uint8_t REG_CT_MODEL_CH0        = 0x51;  // v1.3 read-back: applied model per channel
static constexpr uint8_t REG_CT_MODEL_CH1        = 0x52;
static constexpr uint8_t REG_CT_MODEL_CH2        = 0x53;

// ---- AC / topology ----------------------------------------------------------
static constexpr uint8_t REG_AC_FREQ             = 0x20;  // 50 or 60
static constexpr uint8_t REG_TOPOLOGY            = 0x24;  // 1=SINGLE, 2=SPLIT_PHASE, 3=THREE_PHASE

// ---- Fleet / multi-module --------------------------------------------------
static constexpr uint8_t REG_FLEET_CONFIG        = 0x27;  // bit0=GC_ENABLE (effective after reset)
static constexpr uint8_t REG_GROUP_ID            = 0x28;  // GC latch group filter (0 = all-call only)
static constexpr uint8_t REG_GC_TICK             = 0x59;  // Master tick from last GC-latch (u16); 0xFFFF=never

// ---- Digest / events / thresholds ------------------------------------------
static constexpr uint8_t REG_DIGEST_CONFIG       = 0x29;  // Window composition bitmask (DIGEST_* bits)
static constexpr uint8_t REG_EVENT_FLAGS         = 0x2A;  // Sticky, W1C
static constexpr uint8_t REG_EVENT_MASK          = 0x2B;  // Which EVENT_FLAGS bits assert DRDY low
static constexpr uint8_t REG_THRESH_I_HI         = 0x2C;  // u16; 0xFFFF=disabled
static constexpr uint8_t REG_THRESH_P_HI         = 0x2E;  // u16; 0xFFFF=disabled

// ---- Two-phase address commit ----------------------------------------------
static constexpr uint8_t REG_I2C_ADDRESS         = 0x30;  // v1.3: write candidate -> staged (reads return staged)
static constexpr uint8_t REG_ADDR_COMMIT_MAGIC   = 0x31;  // Write 0xA5 to arm CMD_COMMIT_ADDR
static constexpr uint8_t ADDR_COMMIT_MAGIC_VAL   = 0xA5;

// ---- Diagnostics / lifecycle -----------------------------------------------
static constexpr uint8_t REG_UPTIME_S            = 0x46;  // u32
static constexpr uint8_t REG_RESET_CAUSE         = 0x4A;  // RCC_CSR flag mirror
static constexpr uint8_t REG_I2C_ERR_COUNT       = 0x4B;  // u16 saturating
static constexpr uint8_t REG_I2C_REINIT_COUNT    = 0x4D;  // u8 saturating
static constexpr uint8_t REG_ZC_OFFSET           = 0x4E;  // u16 (U-variants only)

// ---- Identity / capability --------------------------------------------------
static constexpr uint8_t REG_PRODUCT_ID          = 0x54;  // 0x01=rbAmp sensor, 0x02=rbDimmer
static constexpr uint8_t REG_HW_VARIANT          = 0x55;  // 1=UI1, 2=UI2, 3=UI3, 4=I1, 5=I2, 6=I3
static constexpr uint8_t REG_FW_TIER             = 0x56;  // bits0-1: 0=BASIC,1=STANDARD,2=PRO; bit2=bidir; bit3=LUT-cal
static constexpr uint8_t REG_CAPABILITY          = 0x57;  // u16 feature bitmap
static constexpr uint8_t REG_UID                 = 0x5C;  // 96-bit (12 bytes)
static constexpr uint8_t REG_LABEL               = 0x68;  // 8 bytes ASCII zero-padded

// ---- Digest burst -----------------------------------------------------------
static constexpr uint8_t REG_DIGEST              = 0x70;  // 22-byte burst block

// ---- Real-time / period block (float LE) ------------------------------------
static constexpr uint8_t REG_U_RMS               = 0x86;
static constexpr uint8_t REG_U_PEAK              = 0x8A;
static constexpr uint8_t REG_I0_RMS              = 0x8E;
static constexpr uint8_t REG_I1_RMS              = 0x92;
static constexpr uint8_t REG_I2_RMS              = 0x96;
static constexpr uint8_t REG_I0_PEAK             = 0x9A;
static constexpr uint8_t REG_I1_PEAK             = 0x9E;
static constexpr uint8_t REG_I2_PEAK             = 0xA2;
static constexpr uint8_t REG_P0_REAL             = 0xA6;
static constexpr uint8_t REG_P1_REAL             = 0xAA;
static constexpr uint8_t REG_P2_REAL             = 0xAE;
static constexpr uint8_t REG_PF0                 = 0xB2;
static constexpr uint8_t REG_PF1                 = 0xB6;
static constexpr uint8_t REG_PF2                 = 0xBA;
static constexpr uint8_t REG_PERIOD_COMMIT_CNT   = 0xBE;
static constexpr uint8_t REG_PERIOD_AVG_P1       = 0xC2;
static constexpr uint8_t REG_PERIOD_AVG_P2       = 0xC6;
static constexpr uint8_t REG_PERIOD_MS_FW        = 0xCA;  // chip-reported (diagnostic; see L9 below)
static constexpr uint8_t REG_DATA_VALID          = 0xCE;
static constexpr uint8_t REG_Q0                  = 0xD0;
static constexpr uint8_t REG_Q1                  = 0xD4;
static constexpr uint8_t REG_Q2                  = 0xD8;
static constexpr uint8_t REG_PERIOD_AVG_P0       = 0xDC;  // PRODUCTION energy primitive ch0
static constexpr uint8_t REG_PERIOD_MAX_P        = 0xE0;
static constexpr uint8_t REG_PERIOD_LATCH_MS     = 0xEC;  // chip-side dt (diagnostic; see L9 below)

// ---- Command opcodes --------------------------------------------------------
static constexpr uint8_t CMD_NOP                 = 0x00;
static constexpr uint8_t CMD_RESET               = 0x01;
static constexpr uint8_t CMD_SAVE_GAINS          = 0x26;
static constexpr uint8_t CMD_LATCH_PERIOD        = 0x27;
static constexpr uint8_t CMD_SET_CT_MODEL_CH0    = 0x28;
static constexpr uint8_t CMD_SET_CT_MODEL_CH1    = 0x29;
static constexpr uint8_t CMD_SET_CT_MODEL_CH2    = 0x2A;
static constexpr uint8_t CMD_COMMIT_ADDR         = 0x30;  // v1.3 two-phase commit
static constexpr uint8_t CMD_CLEAR_ERROR         = 0x31;  // v1.3
static constexpr uint8_t CMD_SAVE_USER_CONFIG    = 0x32;  // v1.3 — supersedes SAVE_GAINS for user-config writes
static constexpr uint8_t CMD_SEAL                = 0x33;  // v1.3
static constexpr uint8_t CMD_UID_ARBITRATE       = 0x34;
static constexpr uint8_t CMD_UID_PRESENT         = 0x35;
static constexpr uint8_t CMD_UID_MUTE_RESET      = 0x36;
static constexpr uint8_t CMD_FACTORY_RESET       = 0xAA;

// ---- Command settle times (ms) — non-zero ones used by ESPHome -------------
static constexpr uint32_t SETTLE_MS_RESET            = 300;
static constexpr uint32_t SETTLE_MS_SAVE_GAINS       = 700;
static constexpr uint32_t SETTLE_MS_LATCH_PERIOD     = 50;
static constexpr uint32_t SETTLE_MS_SET_CT_MODEL_CHN = 5;
static constexpr uint32_t SETTLE_MS_COMMIT_ADDR      = 700;
static constexpr uint32_t SETTLE_MS_SAVE_USER_CONFIG = 700;
static constexpr uint32_t SETTLE_MS_FACTORY_RESET    = 1500;

// ---- Device error codes (REG_ERROR 0x02) ------------------------------------
static constexpr uint8_t DEV_ERR_OK              = 0x00;
static constexpr uint8_t DEV_ERR_CLONE           = 0xF9;  // v1.3
static constexpr uint8_t DEV_ERR_LUT_BAD         = 0xFA;
static constexpr uint8_t DEV_ERR_FLASH_PARAMS_BAD = 0xFB;
static constexpr uint8_t DEV_ERR_NOT_READY       = 0xFC;
static constexpr uint8_t DEV_ERR_SENSOR_OVERFLOW = 0xFD;
static constexpr uint8_t DEV_ERR_PARAM           = 0xFE;
static constexpr uint8_t DEV_ERR_UNHANDLED       = 0xFF;

// ---- CAPABILITY register (0x57) bits ----------------------------------------
static constexpr uint16_t CAP_EXT_ADDRESSING     = (1u << 0);
static constexpr uint16_t CAP_GC_LATCH           = (1u << 1);
static constexpr uint16_t CAP_GC_GROUP_FILTER    = (1u << 2);
static constexpr uint16_t CAP_DIGEST             = (1u << 3);
static constexpr uint16_t CAP_EVENTS             = (1u << 4);
static constexpr uint16_t CAP_UID_ARBITRATION    = (1u << 5);
static constexpr uint16_t CAP_SEAL               = (1u << 6);
static constexpr uint16_t CAP_TWO_PHASE_ADDR     = (1u << 7);
static constexpr uint16_t CAP_ZC_PHASE_OFFSET    = (1u << 8);
static constexpr uint16_t CAP_SAVE_USER_CONFIG   = (1u << 9);
static constexpr uint16_t CAP_CLEAR_ERROR        = (1u << 10);
static constexpr uint16_t CAP_IAP                = (1u << 11);

// ---- EVENT_FLAGS (0x2A) / EVENT_MASK (0x2B) bits ----------------------------
static constexpr uint8_t EVENT_PERIOD_READY      = (1u << 0);
static constexpr uint8_t EVENT_THRESH_I          = (1u << 1);
static constexpr uint8_t EVENT_THRESH_P          = (1u << 2);
static constexpr uint8_t EVENT_ERROR             = (1u << 3);
static constexpr uint8_t EVENT_CONFIG_CHANGED    = (1u << 4);
static constexpr uint8_t EVENT_RESET_OCCURRED    = (1u << 5);

// ---- DIGEST_CONFIG (0x29) mask bits ----------------------------------------
static constexpr uint8_t DIGEST_I_RMS            = (1u << 0);
static constexpr uint8_t DIGEST_U_RMS            = (1u << 1);
static constexpr uint8_t DIGEST_P_REAL           = (1u << 2);
static constexpr uint8_t DIGEST_PF               = (1u << 3);

// ---- General-call (broadcast LATCH) frame format ----------------------------
// 5-byte frame transmitted to I²C address 0x00 (general-call) when
// FLEET_CONFIG.bit0 is set device-side. Group byte must match REG_GROUP_ID
// or be 0x00 (all-call) for the device to accept the latch.
static constexpr uint8_t GC_FRAME_MAGIC          = 0xA5;  // byte 0
static constexpr uint8_t GC_FRAME_OPCODE         = 0x27;  // byte 1 — same value as CMD_LATCH_PERIOD
// bytes 2..4 = <group> <tick_lo> <tick_hi>

// ---- L9 anti-revert note ----------------------------------------------------
// REG_PERIOD_LATCH_MS (0xEC) and REG_PERIOD_MS_FW (0xCA) are CHIP-SIDE timing
// reports. The chip's SysTick starves under interrupt load and these values
// under-count by ~26% (HW-validated). They are DIAGNOSTIC-ONLY. Energy
// integration MUST use master wall-clock (millis() / esp_timer_get_time())
// across consecutive consumed reads. See rbamp.cpp::finish_latch_phase_ and
// libs/esp_idf/components/rbamp/include/rbamp_energy.h for the canonical
// pattern.

}  // namespace rbamp
}  // namespace esphome
