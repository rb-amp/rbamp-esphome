#pragma once
// SCT-013 rating switch — master-side correction relative to the gain the
// module actually applies (family spec SCT_RATING_SWITCH.md, BASIC line).
//
// Every SCT batch is factory-calibrated on the -030 sensor (model 3). When
// the user switches the channel's model to the sensor they own, firmware
// without on-device rating handling keeps applying either the factory gain
// (after a restart: scale of the -030) or the raw preset (same boot: per-unit
// trim lost). The library therefore multiplies everything proportional to
// the current gain by s = preset(X) × c(X) / preset(3) — the per-unit trim k
// cancels because the module applies G_cal = preset(3) × k.
//
// This header is dependency-free on purpose: tests/host/test_sct_rating.cpp
// compiles it with plain g++ and asserts the spec's test vectors.

#include <cstdint>

namespace esphome {
namespace rbamp {
namespace sct_rating {

// Preset gains from the firmware preset table (op-amp revision), per spec.
inline float preset_g(uint8_t model) {
  switch (model) {
    case 1: return 0.1058f;  // -005
    case 2: return 0.2116f;  // -010
    case 6: return 0.4233f;  // -020
    case 3: return 0.6349f;  // -030 (the cal model)
    case 4: return 1.0582f;  // -050
    case 7: return 1.2698f;  // -060
    case 5: return 2.1622f;  // -100
    default: return 0.0f;    // unknown / not an SCT-013 code
  }
}

// Rating correction c(X): the -100 preset reads +4 % high on the bench
// baseline (both module families and the hub), so it carries 0.962.
inline float rating_c(uint8_t model) { return model == 5 ? 0.962f : 1.0f; }

inline constexpr float PRESET_CAL = 0.6349f;      // preset(3)
inline constexpr float GAIN_SANITY_TOL = 0.05f;   // |G_a/preset(3) − 1| bound

enum class Action : uint8_t {
  APPLY,       // s computed from the applied gain — use it
  NEED_RESET,  // same-boot preset state: issue ONE CMD_RESET and re-run
  WARN_UNCAL,  // still the raw preset after the reset: uncalibrated module,
               // k = 1 → only the rating correction c(X) remains
  WARN_GAIN,   // applied gain is not within ±5 % of preset(3): do not guess
  OFF,         // correction not applicable (fw handles it / unknown model)
};

struct Decision {
  float s;
  Action action;
};

// Pure decision per spec §Algorithm. `g_a` is the gain the module applies
// right now (readable in user mode); `fw_handles` is true on firmware that
// performs the rating switch on-device (then the plain model write is
// enough); `after_reset` marks the re-read following the single CMD_RESET.
inline Decision decide(uint8_t model, float g_a, bool fw_handles, bool after_reset) {
  if (fw_handles)
    return {1.0f, Action::OFF};
  const float pg = preset_g(model);
  if (pg == 0.0f)
    return {1.0f, Action::OFF};
  if (model == 3)
    return {1.0f, Action::APPLY};  // calibrated model: module scale is right
  if (g_a == pg) {
    // Bit-equal to the raw preset: the channel was switched in this boot, so
    // the per-unit trim k is unknown — a restart brings back G_cal.
    if (!after_reset)
      return {1.0f, Action::NEED_RESET};
    // Still the preset after the reset: uncalibrated module, k = 1, and the
    // module already applies preset(X) itself — only c(X) remains.
    return {rating_c(model), Action::WARN_UNCAL};
  }
  // Sanity: the factory gain must be the -030 cal value within the unit-trim
  // band. NaN/zero/negative fail the range check too.
  const bool in_band = (g_a > PRESET_CAL * (1.0f - GAIN_SANITY_TOL)) &&
                       (g_a < PRESET_CAL * (1.0f + GAIN_SANITY_TOL));
  if (!in_band)
    return {1.0f, Action::WARN_GAIN};
  // s = preset(X)·c(X)·k / G_a with k = G_a/preset(3): the trim cancels.
  return {pg * rating_c(model) / PRESET_CAL, Action::APPLY};
}

}  // namespace sct_rating
}  // namespace rbamp
}  // namespace esphome
