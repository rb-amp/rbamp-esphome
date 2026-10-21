// Host test for the SCT rating-switch decision logic. Build + run:
//
//   g++ -std=c++17 -Wall -Wextra -Werror -I../../components/rbamp test_sct_rating.cpp -o /tmp/test_sct_rating && /tmp/test_sct_rating
//
// Plain asserts, no framework — mirrors the esp-idf host-sim convention.

#include <cassert>
#include <cmath>
#include <cstdio>

#include "rbamp_sct_rating.h"

using esphome::rbamp::sct_rating::Action;
using esphome::rbamp::sct_rating::decide;
using esphome::rbamp::sct_rating::preset_g;

static int checks = 0;

static void expect(bool cond, const char *what) {
  checks++;
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    assert(cond);
  }
}

static bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

int main() {
  // Spec test 1: flash state, X = 5, G_a = 0.6414 → s = 2.1622·0.962/0.6349 = 3.276.
  {
    auto d = decide(5, 0.6414f, false, false);
    expect(d.action == Action::APPLY, "X=5 flash state applies");
    expect(near(d.s, 3.276f), "X=5 flash state s = 3.276");
  }
  // Trim cancels: a different in-band G_a gives the same s.
  {
    auto d = decide(5, 0.6250f, false, false);
    expect(d.action == Action::APPLY && near(d.s, 3.276f), "s independent of in-band trim");
  }
  // Spec test 2: X = 3 → s = 1.
  {
    auto d = decide(3, 0.6414f, false, false);
    expect(d.action == Action::APPLY && d.s == 1.0f, "X=3 → s=1");
  }
  // Spec test 3: same-boot state, G_a bit-equal preset(5) → one CMD_RESET.
  {
    auto d = decide(5, preset_g(5), false, false);
    expect(d.action == Action::NEED_RESET, "G_a == preset(X) → NEED_RESET");
  }
  // Still the preset after the reset → uncalibrated, k = 1 → s = c(X).
  {
    auto d = decide(5, preset_g(5), false, true);
    expect(d.action == Action::WARN_UNCAL, "preset after reset → WARN_UNCAL");
    expect(near(d.s, 0.962f), "uncal -100: only c(X) remains");
    auto d2 = decide(4, preset_g(4), false, true);
    expect(d2.action == Action::WARN_UNCAL && d2.s == 1.0f, "uncal -050: s = 1");
  }
  // Spec test 4: G_a off by 8 % → warning, s = 1.
  {
    auto d = decide(5, 0.6349f * 1.08f, false, false);
    expect(d.action == Action::WARN_GAIN && d.s == 1.0f, "gain +8% → WARN_GAIN, s=1");
    auto d2 = decide(4, 0.6349f * 0.92f, false, false);
    expect(d2.action == Action::WARN_GAIN && d2.s == 1.0f, "gain -8% → WARN_GAIN, s=1");
  }
  // Spec test 5: fw handles the switch on-device (CAP_SCT_RATING_SELF,
  // bit 12 — reserved in fw 1.4.20) → s = 1, plain model write, and no
  // reset even when the gain still equals the raw preset.
  {
    auto d = decide(5, 0.6414f, true, false);
    expect(d.action == Action::OFF && d.s == 1.0f, "fw handles → s=1");
    auto d2 = decide(5, preset_g(5), true, false);
    expect(d2.action == Action::OFF && d2.s == 1.0f, "fw handles → no reset either");
  }
  // Garbage gain: NaN / zero / negative all fail the band check, never APPLY.
  {
    expect(decide(5, NAN, false, false).action == Action::WARN_GAIN, "NaN gain → WARN_GAIN");
    expect(decide(5, 0.0f, false, false).action == Action::WARN_GAIN, "zero gain → WARN_GAIN");
    expect(decide(5, -0.64f, false, false).action == Action::WARN_GAIN, "negative gain → WARN_GAIN");
  }
  // Unknown model code → correction off, s = 1.
  {
    auto d = decide(9, 0.6414f, false, false);
    expect(d.action == Action::OFF && d.s == 1.0f, "unknown model → OFF");
  }
  // Whole-table flash-state sweep: s = preset(X)·c(X)/preset(3) for every model.
  {
    const uint8_t models[] = {1, 2, 6, 4, 7, 5};
    for (uint8_t m : models) {
      auto d = decide(m, 0.6414f, false, false);
      float want = preset_g(m) * (m == 5 ? 0.962f : 1.0f) / 0.6349f;
      expect(d.action == Action::APPLY && near(d.s, want), "table sweep");
    }
  }
  // Negative control: a wrong expectation must NOT pass (the +8 % case does
  // not produce the X=5 scale — guards against decide() degenerating into
  // "always scale").
  {
    auto d = decide(5, 0.6349f * 1.08f, false, false);
    expect(!(d.action == Action::APPLY && near(d.s, 3.276f)),
           "negative control: out-of-band gain never scales");
  }

  std::printf("test_sct_rating: %d checks PASS\n", checks);
  return 0;
}
