#pragma once
// STD snapshot banks (image 0x7E and later, capability bit 8): eight
// independent master-timed periods. A bank accumulates while its bit is set
// in the arm mask; a latch snapshots AND resets it at the module's next
// measurement-window boundary, so a period is the interval between two
// latches of the same bank, measured by the MASTER's clock.
//
// Dependency-free on purpose (the bus is a template parameter): the whole
// arm → latch → read → judge flow compiles with plain g++ and runs against a
// byte snapshot of a module in tests/host/.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rbamp_std_registers.h"

namespace esphome {
namespace rbamp {
namespace bank {

static constexpr uint8_t MAX_CHANNELS = 7;
static constexpr uint8_t MAX_BANKS = 8;
// The latch is serviced at the module's next window boundary (~100 ms from
// image 0x7C, up to ~160 ms before); 300 ms is the safe bound for every image.
static constexpr uint32_t SETTLE_MS = 300;

struct Snapshot {
  float p_avg{0}, q_avg{0}, s_avg{0};  // W / var / VA, signed
  float max_p{0}, max_i{0}, max_u{0}, min_u{0};
  int64_t e_pos{0}, e_neg{0};  // mW·samples, import >= 0 / export <= 0
  uint32_t nsamp{0}, win_count{0};
  uint8_t channels{0};
  int64_t ch_pos[MAX_CHANNELS]{0}, ch_neg[MAX_CHANNELS]{0};
  float ch_p_avg[MAX_CHANNELS]{0};  // (E_pos + E_neg) / NSAMP, W
};

inline uint32_t le_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t le_i32(const uint8_t *p) { return static_cast<int32_t>(le_u32(p)); }
inline int64_t le_i64(const uint8_t *p) {
  return static_cast<int64_t>(static_cast<uint64_t>(le_u32(p)) |
                              (static_cast<uint64_t>(le_u32(p + 4)) << 32));
}

inline size_t burst_len(uint8_t channels) {
  if (channels > MAX_CHANNELS) channels = MAX_CHANNELS;
  return rbamp_std::LATCH_CHE_BASE + static_cast<size_t>(channels) * rbamp_std::LATCH_CHE_STRIDE;
}

// b = burst_len(channels) bytes read from LATCH(bank).
inline void decode(const uint8_t *b, uint8_t channels, Snapshot *out) {
  if (channels > MAX_CHANNELS) channels = MAX_CHANNELS;
  *out = Snapshot();
  out->channels = channels;
  out->p_avg = static_cast<float>(le_i32(b + rbamp_std::LATCH_P)) * 1e-3f;
  out->q_avg = static_cast<float>(le_i32(b + rbamp_std::LATCH_Q)) * 1e-3f;
  out->s_avg = static_cast<float>(le_i32(b + rbamp_std::LATCH_S)) * 1e-3f;
  out->e_pos = le_i64(b + rbamp_std::LATCH_E_POS);
  out->e_neg = le_i64(b + rbamp_std::LATCH_E_NEG);
  out->nsamp = le_u32(b + rbamp_std::LATCH_NSAMP);
  out->win_count = le_u32(b + rbamp_std::LATCH_WIN_CNT);
  out->max_p = static_cast<float>(le_i32(b + rbamp_std::LATCH_MAX_P)) * 1e-3f;
  out->max_i = static_cast<float>(le_i32(b + rbamp_std::LATCH_MAX_I)) * 1e-6f;
  out->max_u = static_cast<float>(le_i32(b + rbamp_std::LATCH_MAX_U)) * 1e-3f;
  out->min_u = static_cast<float>(le_i32(b + rbamp_std::LATCH_MIN_U)) * 1e-3f;
  for (uint8_t c = 0; c < channels; c++) {
    const uint8_t *p = b + rbamp_std::LATCH_CHE_BASE + c * rbamp_std::LATCH_CHE_STRIDE;
    out->ch_pos[c] = le_i64(p + rbamp_std::LATCH_CHE_E_POS);
    out->ch_neg[c] = le_i64(p + rbamp_std::LATCH_CHE_E_NEG);
    // Contract: per-channel average P over the period = (E_pos + E_neg) / NSAMP [mW].
    out->ch_p_avg[c] =
        out->nsamp ? static_cast<float>(static_cast<double>(out->ch_pos[c] + out->ch_neg[c]) /
                                        static_cast<double>(out->nsamp)) * 1e-3f
                   : 0.0f;
  }
}

// mW·samples → Wh over the master-measured period: /1000 (mW→W), /3600
// (s→h), × seconds-per-sample (dt / nsamp) — the sample count and the
// master's dt remove any dependence on the module's sample rate.
inline double energy_wh(int64_t e_mws, uint32_t dt_ms, uint32_t nsamp) {
  if (nsamp == 0) return 0.0;
  return static_cast<double>(e_mws) / 1000.0 *
         (static_cast<double>(dt_ms) / 1000.0 / static_cast<double>(nsamp) / 3600.0);
}

inline bool banks_available(uint16_t caps) { return (caps & rbamp_std::CAP_MULTI_LATCH) != 0; }

// Bank 0 is the component's own period bank on images that have banks but no
// on-device counters; with counters it is free for the user.
inline bool bank_usable(uint8_t bank, bool counters_in_use) {
  return bank < MAX_BANKS && (bank != 0 || counters_in_use);
}

struct State {
  bool primed{false};       // a latch since (re)arm has anchored the period start
  uint32_t anchor_ms{0};    // master clock at the latch that started the open period
  uint32_t latch_ms{0};     // master clock at the most recent latch request
};

enum class Begin : uint8_t { OK, BUS_ERROR };
enum class Finish : uint8_t {
  PUBLISH,    // whole period, out/dt_ms valid
  PRIMED,     // first latch after (re)arm: start-of-period unknown, dropped
  REARMED,    // module lost the arm (reset): re-armed + primer-latched, dropped
  EMPTY,      // armed but no window elapsed: dropped
  NOT_VALID,  // module has never latched this bank
  BUS_ERROR,
};

// Bus: bool read(uint16_t reg, uint8_t *buf, size_t n); bool write_u8(uint16_t reg, uint8_t v);
template <class Bus>
struct Engine {
  static bool latch_(Bus &bus, uint8_t bank) {
    return bus.write_u8(rbamp_std::REG_LATCH_BANK, bank) &&
           bus.write_u8(rbamp_std::REG_COMMAND, static_cast<uint8_t>(rbamp_std::CMD_LATCH_PERIOD));
  }

  // Arms the bank if needed (a reset clears the mask — it is RAM on the
  // module) and requests the latch. Read the result SETTLE_MS later.
  static Begin begin(Bus &bus, uint8_t bank, State *st, uint32_t now_ms) {
    uint8_t arm = 0;
    if (!bus.read(rbamp_std::REG_PERIOD_ARM, &arm, 1)) return Begin::BUS_ERROR;
    const uint8_t bit = static_cast<uint8_t>(1u << bank);
    if (!(arm & bit)) {
      if (!bus.write_u8(rbamp_std::REG_PERIOD_ARM, static_cast<uint8_t>(arm | bit)))
        return Begin::BUS_ERROR;
      st->primed = false;  // newly armed bank starts clean: this latch is the primer
    }
    if (!latch_(bus, bank)) return Begin::BUS_ERROR;
    st->latch_ms = now_ms;
    return Begin::OK;
  }

  static Finish finish(Bus &bus, uint8_t bank, uint8_t channels, State *st, uint32_t now_ms,
                       Snapshot *out, uint32_t *dt_ms) {
    uint8_t pd = 0;
    if (!bus.read(rbamp_std::REG_PD_VALID, &pd, 1)) return Finish::BUS_ERROR;
    if (!((pd >> bank) & 1u)) return Finish::NOT_VALID;

    uint8_t b[rbamp_std::LATCH_CHE_BASE + MAX_CHANNELS * rbamp_std::LATCH_CHE_STRIDE];
    if (!bus.read(rbamp_std::LATCH(bank), b, burst_len(channels))) return Finish::BUS_ERROR;
    decode(b, channels, out);

    if (out->win_count == 0 || out->nsamp == 0) {
      // An empty snapshot from a bank that is no longer armed means the
      // module reset: re-arm, primer-latch so the NEXT period is whole, and
      // restart the dt anchor — the outage carries no data.
      uint8_t arm = 0;
      if (!bus.read(rbamp_std::REG_PERIOD_ARM, &arm, 1)) return Finish::BUS_ERROR;
      const uint8_t bit = static_cast<uint8_t>(1u << bank);
      if (!(arm & bit)) {
        if (!bus.write_u8(rbamp_std::REG_PERIOD_ARM, static_cast<uint8_t>(arm | bit)) ||
            !latch_(bus, bank))
          return Finish::BUS_ERROR;
        st->primed = true;
        st->anchor_ms = now_ms;
        return Finish::REARMED;
      }
      if (!st->primed) {
        // Armed by this very begin(): the latch IS the primer (the bank was
        // empty at that instant) — the period now starts at the latch.
        st->primed = true;
        st->anchor_ms = st->latch_ms;
        return Finish::PRIMED;
      }
      return Finish::EMPTY;
    }

    if (!st->primed) {
      st->primed = true;
      st->anchor_ms = st->latch_ms;
      return Finish::PRIMED;
    }
    *dt_ms = st->latch_ms - st->anchor_ms;
    st->anchor_ms = st->latch_ms;
    return *dt_ms == 0 ? Finish::EMPTY : Finish::PUBLISH;
  }
};

}  // namespace bank
}  // namespace rbamp
}  // namespace esphome
