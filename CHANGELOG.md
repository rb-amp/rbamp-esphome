# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.9.0] — 2026-10-21

**Public releases resume with this version; intermediate versions were
released internally only.**

SCT rating switch without losing factory accuracy (8-bit line), plus the
guard releases 1.8.x folded into this public release.

### Added
- **SCT rating-switch correction** (modules with the SCT-013 sensor class,
  firmware ≤ 1.4.19). Every SCT module is factory-calibrated with the -030
  sensor; selecting another `ct_model:` used to leave current/power/energy
  on the -030 scale. The component now completes the model write with the
  module restart the procedure requires (save → reset → re-probe with a UID
  check), reads the gain the module actually applies and scales current,
  active/reactive power and the energy integration to the selected sensor's
  rating — the per-unit factory trim is preserved. Voltage and power factor
  are not affected. A module restart (own or external) is detected and the
  correction recomputes automatically. If the applied gain is outside ±5 %
  of the calibration-model value, the component warns and does not scale.
  No YAML change needed.
- Firmware **1.4.20+** performs the rating switch on-device and advertises
  it (capability bit 12) — the component then detects it and leaves the
  readings untouched (plain model write, no restart).
- Host test for the correction decision logic: `tests/host/test_sct_rating.cpp`
  (plain `g++`, no framework).

## [1.8.1] — 2026-10-18

### Fixed
- **Factory-calibration protection for `sensor_class:` / `ct_model:`**
  (8-bit line). The firmware clears the channel models and linearity shapes
  on any sensor-class write (even an unchanged value) and re-applies the
  generic preset on any CT-model write — the component now never writes a
  value the device already has, and refuses class/model changes from YAML on
  a module calibrated as a pair with its fitted sensor (classes `WIRED_CT` /
  `BUILTIN_CT`), with an explicit log line. New service option
  `ct_model_force: true` deliberately re-applies the preset; the schema
  rejects it without a model key so it cannot sit armed in YAML.
- **Out-of-bounds read on 5/7-channel modules (UI5/UI7)**: the boot
  restore-publish loop indexed the 3-slot export arrays up to channel 6 and
  could publish a garbage value into the adjacent sensor. Fixed with the
  same channel gate every other export site already had; array sizes are now
  pinned by compile-time asserts.
- Documentation/comments aligned to the family measurement window of
  **100 ms** (firmware v1.2+; 200 ms only on older images).

## [1.8.0] — 2026-10-17

On-device cumulative energy for the 16-bit line.

### Added
- **Energy from on-device counters** (16-bit-map modules advertising them in
  the capability bitmap): the component stops integrating Wh itself and
  accumulates the module's own counters — totals survive restarts of both
  the ESP32 and the module, with no integration drift. Micro-resolution
  (µWh/µvarh) on firmware that publishes it, automatic coarse fallback
  otherwise. Each snapshot is read in one coherent I²C transaction, and the
  published sums stay monotonic across module restarts and counter resets.
- New grand-total sensors: `energy_total`, `energy_exported_total`,
  `energy_net` (import − export, may decrease), `reactive_energy`,
  `reactive_energy_exported`.
- `start_current_ua:` config key (0 = off, 10..2540 in 10 µA steps):
  below this current a channel does not accumulate energy — at no load the
  measured power is noise of either sign that would otherwise creep into
  both import and export. Readings are unaffected. Schema-validated;
  persisted on the device with a verified save.
- Three-phase surfaces: `voltage_angle_b` / `voltage_angle_c` (degrees,
  relative to phase A) and `phase_sequence` (1 forward · 2 reverse ·
  3 abnormal), published on firmware that provides them; "unknown"
  otherwise.
- Lambda accessors: `get_std_image_version()`, `get_phase_sequence_str()`,
  `read_start_current_ua()`.

## [1.7.0] — 2026-09-30

Support for the new 16-bit register-map module generation, autodetected on
the same YAML.

### Added
- **Dual-line support**: an identity probe at boot detects 16-bit-map
  modules and switches the transport (big-endian pointer, auto-increment
  burst reads). Channel counts, variant and capabilities come from the
  device's discovery block — `topology:` and YAML slot counts are no longer
  needed to size the entity surface. 8-bit modules behave exactly as before.
- Realtime sensors on the 16-bit line are published from the device's
  fixed-point registers; energy uses the device's atomic latch snapshots
  with master-clock integration and the same NVS persistence.
- Config keys (`sensor_class:`, `ct_model:`, `new_address:`) work on the
  16-bit line through its staged config block with a synchronously verified
  save. Per-channel `ct_models:` on this line requires the device to
  advertise persistent per-channel maps (capability bit); the component
  refuses loudly instead of losing the write silently.
- Automatic recovery after a module restart mid-run: the component detects
  the dropped accumulator arm, re-arms and re-primes, skipping one window
  instead of silently stopping energy accumulation.
- NVS flush on clean shutdown (reboot/OTA no longer loses up to the last
  save interval of energy).

## [1.6.0] — 2026-09-04

Per-channel `ct_models:` for all channels (0..15), sensor-model registry, and
persist fix. Firmware: v1.4.18+ required for senior channels 3+.

## [1.5.0] — 2026-08 (internal only)

### Added
- UI5/UI7 senior-SKU support: channels 3–6 read through the channel-access
  window (realtime metrics and per-channel period energy), NVS energy slots
  extended to 7 channels.

### Added
- **Per-channel CT models on all channels.** `ct_models:` in YAML now accepts up
  to 16 entries. Module variant detection (`REG_TOPOLOGY`) determines how many
  are applied; extra entries produce a warning, not a failure.
- **Sensor-model registry** — the model list is generated from
  `libs/spec/sensor_models.yaml`. The two hardcoded lists (`CT_MODELS` dict +
  `CT_MODEL_ALLOWED_PER_CLASS`) are replaced by a single registry lookup. Adding
  a new CT SKU is now a registry edit — no library release needed.

### Fixed
- **v1.6.0 adds SAVE after the apply cycle.** Prior versions used a
  factory-gated persist path for CT model and sensor class — on a production
  module the binding applied to RAM but was lost on reboot. v1.6.0 persists via
  `CMD_SAVE_USER_CONFIG` (ungated). **Users of prior versions in production
  silently lost the CT model binding on every reboot — update before shipping
  devices.**

