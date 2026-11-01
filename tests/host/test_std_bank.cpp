// Host test for the STD snapshot-bank engine (rbamp_std_bank.h).
// Build (from tests/host): g++ -std=c++17 -Wall -Wextra -Werror -I ../../components/rbamp -o test_std_bank test_std_bank.cpp
// A small module model stands behind the bus: arm mask, latch command that
// snapshots + resets an armed bank, sticky PD_VALID, and a reset that clears
// the (RAM) arm mask.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "rbamp_std_bank.h"


static int g_fail = 0, g_n = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    g_n++;                                                               \
    if (!(c)) {                                                          \
      g_fail++;                                                          \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);                   \
    }                                                                    \
  } while (0)

#include "module_model.h"

using Eng = bank::Engine<ModuleModel>;

int main() {
  // --- decode + unit conversion ---
  {
    uint8_t b[0x40 + 2 * 0x10] = {0};
    put_i32(b + R::LATCH_P, 1500000);   // 1500 W
    put_i32(b + R::LATCH_Q, -250000);   // -250 var
    put_i32(b + R::LATCH_S, 1520000);
    put_i64(b + R::LATCH_E_POS, 4000000000LL);
    put_i64(b + R::LATCH_E_NEG, -1000000000LL);
    put_i32(b + R::LATCH_NSAMP, 2000);
    put_i32(b + R::LATCH_WIN_CNT, 7);
    put_i32(b + R::LATCH_MAX_I, 6500000);  // 6.5 A
    put_i32(b + R::LATCH_MAX_U, 241500);
    put_i32(b + R::LATCH_MIN_U, 228300);
    put_i64(b + R::LATCH_CHE_BASE + 0, 3000000000LL);
    put_i64(b + R::LATCH_CHE_BASE + 8, -1000000000LL);
    bank::Snapshot s;
    bank::decode(b, 2, &s);
    CHECK(std::fabs(s.p_avg - 1500.0f) < 1e-3f);
    CHECK(std::fabs(s.q_avg + 250.0f) < 1e-3f);
    CHECK(std::fabs(s.max_i - 6.5f) < 1e-5f);
    CHECK(std::fabs(s.max_u - 241.5f) < 1e-3f);
    CHECK(std::fabs(s.min_u - 228.3f) < 1e-3f);
    CHECK(s.nsamp == 2000 && s.win_count == 7 && s.channels == 2);
    CHECK(s.e_pos == 4000000000LL && s.e_neg == -1000000000LL);
    // (3e9 - 1e9) / 2000 samples = 1e6 mW = 1000 W
    CHECK(std::fabs(s.ch_p_avg[0] - 1000.0f) < 1e-2f);
    // 1000 W for 1 h → 1000 Wh, whatever the sample count
    CHECK(std::fabs(bank::energy_wh(1000000LL * 2000, 3600000, 2000) - 1000.0) < 1e-6);
    CHECK(std::fabs(bank::energy_wh(1000000LL * 777, 3600000, 777) - 1000.0) < 1e-6);
    CHECK(std::fabs(bank::energy_wh(-500000LL * 100, 1800000, 100) + 250.0) < 1e-6);
    CHECK(bank::energy_wh(123, 1000, 0) == 0.0);  // nothing accumulated
  }

  // --- channel clamp: a module reporting more channels than the component slots ---
  {
    struct Guarded {
      bank::Snapshot s;
      uint32_t canary[4];
    } g;
    for (auto &c : g.canary) c = 0xDEADBEEF;
    uint8_t b[0x40 + 7 * 0x10] = {0};
    bank::decode(b, 64, &g.s);
    CHECK(g.s.channels == bank::MAX_CHANNELS);
    for (auto c : g.canary) CHECK(c == 0xDEADBEEF);
    CHECK(bank::burst_len(64) == 0x40u + 7u * 0x10u);
  }

  // --- gating ---
  CHECK(bank::banks_available(R::CAP_MULTI_LATCH));
  CHECK(bank::banks_available(R::CAP_MULTI_LATCH | R::CAP_ONDEVICE_ENERGY));
  CHECK(!bank::banks_available(R::CAP_ONDEVICE_ENERGY));  // negative control: counters only
  CHECK(!bank::banks_available(0));
  CHECK(bank::bank_usable(0, true));
  CHECK(!bank::bank_usable(0, false));  // bank 0 is the component's own without counters
  CHECK(bank::bank_usable(7, false));
  CHECK(!bank::bank_usable(8, true));

  // --- flow: arm → primer → whole period ---
  {
    ModuleModel m;
    bank::State st;
    bank::Snapshot s;
    uint32_t dt = 0;
    CHECK(Eng::begin(m, 1, &st, 1000) == bank::Begin::OK);
    CHECK((m.img[R::REG_PERIOD_ARM] & 0x02) && m.latch_cmds == 1);  // armed + latched
    // a freshly armed bank snapshots empty (win_count 0): that latch IS the primer
    CHECK(Eng::finish(m, 1, 2, &st, 1300, &s, &dt) == bank::Finish::PRIMED);
    CHECK(st.primed && st.anchor_ms == 1000);
    m.run(40, 2000000);
    CHECK(Eng::begin(m, 1, &st, 61000) == bank::Begin::OK);
    CHECK(Eng::finish(m, 1, 2, &st, 61300, &s, &dt) == bank::Finish::PUBLISH);
    CHECK(dt == 60000);  // latch-to-latch on the master clock
    CHECK(std::fabs(s.p_avg - 2000.0f) < 1e-2f);
    CHECK(s.win_count == 40 && s.nsamp == 40u * 293u);
    // 2000 W over 60 s = 33.333 Wh, independent of how many samples the module counted
    CHECK(std::fabs(bank::energy_wh(s.e_pos, dt, s.nsamp) - 2000.0 * 60.0 / 3600.0) < 1e-3);
    CHECK(std::fabs(bank::energy_wh(s.ch_pos[0], dt, s.nsamp) - 1000.0 * 60.0 / 3600.0) < 1e-3);
  }

  // --- flow: module reset between latches → re-arm + primer, period dropped, next one whole ---
  {
    ModuleModel m;
    bank::State st;
    bank::Snapshot s;
    uint32_t dt = 0;
    Eng::begin(m, 2, &st, 0);
    m.run(5, 1000000);
    Eng::begin(m, 2, &st, 60000);  // not armed bank yet? armed in the first begin; this latch is a primer-result
    Eng::finish(m, 2, 1, &st, 60300, &s, &dt);
    m.run(5, 1000000);
    Eng::begin(m, 2, &st, 120000);
    Eng::finish(m, 2, 1, &st, 120300, &s, &dt);
    CHECK(st.primed);
    // latch requested, then the module resets before the read
    Eng::begin(m, 2, &st, 180000);
    m.module_reset();
    const unsigned cmds_before = m.latch_cmds;
    CHECK(Eng::finish(m, 2, 1, &st, 180300, &s, &dt) == bank::Finish::REARMED);
    CHECK((m.img[R::REG_PERIOD_ARM] & 0x04));                  // arm restored
    CHECK(m.latch_cmds == cmds_before + 1);                    // primer latch issued
    CHECK(st.primed && st.anchor_ms == 180300);                // dt anchor restarted
    m.run(20, 3000000);
    CHECK(Eng::begin(m, 2, &st, 240000) == bank::Begin::OK);
    CHECK(Eng::finish(m, 2, 1, &st, 240300, &s, &dt) == bank::Finish::PUBLISH);
    CHECK(dt == 240000 - 180300 + 0 || dt == 59700);           // from the primer's finish time
  }

  // --- flow: reset BEFORE the latch (begin sees the lost arm) → this latch is a primer ---
  {
    ModuleModel m;
    bank::State st;
    bank::Snapshot s;
    uint32_t dt = 0;
    Eng::begin(m, 3, &st, 0);
    m.run(4, 1000000);
    Eng::begin(m, 3, &st, 1000);
    Eng::finish(m, 3, 1, &st, 1300, &s, &dt);
    m.run(4, 1000000);
    Eng::begin(m, 3, &st, 61000);
    Eng::finish(m, 3, 1, &st, 61300, &s, &dt);
    CHECK(st.primed);
    m.module_reset();
    CHECK(Eng::begin(m, 3, &st, 121000) == bank::Begin::OK);
    CHECK(!st.primed);  // re-armed by begin: whatever it snapshots is not a whole period
    m.run(3, 1000000);
    CHECK(Eng::finish(m, 3, 1, &st, 121300, &s, &dt) != bank::Finish::PUBLISH);
  }

  // --- negative controls ---
  {
    // never-latched bank
    ModuleModel m;
    bank::State st;
    bank::Snapshot s;
    uint32_t dt = 0;
    CHECK(Eng::finish(m, 5, 1, &st, 0, &s, &dt) == bank::Finish::NOT_VALID);
    // a disarmed bank with stale data must never publish: sticky PD_VALID + empty window
    m.img[R::REG_PD_VALID] = 0x20;
    CHECK(Eng::finish(m, 5, 1, &st, 0, &s, &dt) == bank::Finish::REARMED);  // not a PUBLISH
    // bus failure
    m.fail_reads = true;
    CHECK(Eng::begin(m, 5, &st, 0) == bank::Begin::BUS_ERROR);
    CHECK(Eng::finish(m, 5, 1, &st, 0, &s, &dt) == bank::Finish::BUS_ERROR);
  }

  std::printf("test_std_bank: %d checks, %d failed\n", g_n, g_fail);
  return g_fail ? 1 : 0;
}
