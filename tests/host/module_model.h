#pragma once
// Module model behind the bank engine tests: arm mask, latch command that
// snapshots + resets an armed bank, sticky PD_VALID, and a reset that clears
// the (RAM) arm mask. Used by test_std_bank.cpp.
#include <cstdio>
#include <cstring>

#include "rbamp_std_bank.h"

namespace R = rbamp_std;
using namespace esphome::rbamp;

static void put_i32(uint8_t *p, int32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}
static void put_i64(uint8_t *p, int64_t v) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
}

struct ModuleModel {
  uint8_t img[0x4800];
  // what each bank has accumulated since its last latch
  uint32_t acc_win[8]{}, acc_nsamp[8]{};
  int64_t acc_epos[8]{}, acc_eneg[8]{};
  int64_t acc_ch_pos[8][bank::MAX_CHANNELS]{}, acc_ch_neg[8][bank::MAX_CHANNELS]{};
  unsigned latch_cmds{0}, arm_writes{0};
  bool fail_reads{false};

  ModuleModel() { std::memset(img, 0, sizeof img); }

  // simulate elapsed measurement windows on every armed bank
  void run(uint32_t windows, int32_t p_mw, uint32_t nsamp_per_win = 293) {
    for (int b = 0; b < 8; b++) {
      if (!((img[R::REG_PERIOD_ARM] >> b) & 1)) continue;
      acc_win[b] += windows;
      acc_nsamp[b] += windows * nsamp_per_win;
      const int64_t e = static_cast<int64_t>(p_mw) * windows * nsamp_per_win;
      acc_epos[b] += e;
      for (int c = 0; c < bank::MAX_CHANNELS; c++) acc_ch_pos[b][c] += e / 2;
    }
  }
  void module_reset() {  // arm mask is RAM; banks restart empty; PD_VALID is not cleared here
    img[R::REG_PERIOD_ARM] = 0;
    std::memset(acc_win, 0, sizeof acc_win);
    std::memset(acc_nsamp, 0, sizeof acc_nsamp);
    std::memset(acc_epos, 0, sizeof acc_epos);
    std::memset(acc_eneg, 0, sizeof acc_eneg);
    std::memset(acc_ch_pos, 0, sizeof acc_ch_pos);
    std::memset(acc_ch_neg, 0, sizeof acc_ch_neg);
    for (int b = 0; b < 8; b++) std::memset(img + R::LATCH(b), 0, 0x100);
  }

  // Bus interface
  bool read(uint16_t reg, uint8_t *buf, size_t n) {
    if (fail_reads) return false;
    std::memcpy(buf, img + reg, n);
    return true;
  }
  bool write_u8(uint16_t reg, uint8_t v) {
    if (reg == R::REG_PERIOD_ARM) arm_writes++;
    img[reg] = v;
    if (reg == R::REG_COMMAND && v == R::CMD_LATCH_PERIOD) {
      latch_cmds++;
      const uint8_t b = img[R::REG_LATCH_BANK];
      if (b < 8 && ((img[R::REG_PERIOD_ARM] >> b) & 1)) {
        uint8_t *p = img + R::LATCH(b);
        put_i32(p + R::LATCH_P, acc_nsamp[b] ? static_cast<int32_t>(acc_epos[b] / acc_nsamp[b]) : 0);
        put_i64(p + R::LATCH_E_POS, acc_epos[b]);
        put_i64(p + R::LATCH_E_NEG, acc_eneg[b]);
        put_i32(p + R::LATCH_NSAMP, static_cast<int32_t>(acc_nsamp[b]));
        put_i32(p + R::LATCH_WIN_CNT, static_cast<int32_t>(acc_win[b]));
        put_i32(p + R::LATCH_MAX_P, 2500000);
        put_i32(p + R::LATCH_MAX_I, 11500000);
        put_i32(p + R::LATCH_MAX_U, 241500);
        put_i32(p + R::LATCH_MIN_U, 228300);
        for (int c = 0; c < bank::MAX_CHANNELS; c++) {
          put_i64(p + R::LATCH_CHE_BASE + c * R::LATCH_CHE_STRIDE + R::LATCH_CHE_E_POS, acc_ch_pos[b][c]);
          put_i64(p + R::LATCH_CHE_BASE + c * R::LATCH_CHE_STRIDE + R::LATCH_CHE_E_NEG, acc_ch_neg[b][c]);
        }
        img[R::REG_PD_VALID] |= static_cast<uint8_t>(1u << b);
        acc_win[b] = acc_nsamp[b] = 0;
        acc_epos[b] = acc_eneg[b] = 0;
        std::memset(acc_ch_pos[b], 0, sizeof acc_ch_pos[b]);
        std::memset(acc_ch_neg[b], 0, sizeof acc_ch_neg[b]);
      } else if (b < 8) {
        // un-armed bank: the module answers with an empty snapshot
        std::memset(img + R::LATCH(b), 0, 0x100);
        img[R::REG_PD_VALID] |= static_cast<uint8_t>(1u << b);
      }
    }
    return true;
  }
};

