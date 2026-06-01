# 11 · Changelog

Version history of the `rbamp` ESPHome external component. Dates in UTC.

Contents:

- [v0.4.0 — 2026-05-28, protocol v1.2 parity](#v040--2026-05-28-protocol-v12-parity)
- [v0.3 — 2026-05-28, 8-PR resilience audit](#v03--2026-05-28-8-pr-resilience-audit)
- [v0.2 — 2026-05-24, SPEC §B.5 NACK discipline](#v02--2026-05-24-spec-b5-nack-discipline)
- [v0.1 — 2026-05-23, baseline import](#v01--2026-05-23-baseline-import)
- [Forward compatibility with future firmware revisions](#forward-compatibility-with-future-firmware-revisions)
- [Upgrade guide](#upgrade-guide)
- [Deprecation policy](#deprecation-policy)

---

## Deprecation policy

What the component promises not to break without warning, and what it may:

| Surface | Stability | What we promise |
|---|---|---|
| YAML keys of the `rbamp:` block | **Stable** | Removal or rename is possible only on a major bump (v1.0.0). Two minor releases before removal — a deprecation warning in the `dump_config` log. |
| YAML keys of `sensor.platform: rbamp` (slots) | **Stable** | Same rules. The names `voltage`/`current`/`power`/`energy`/`frequency`/`power_factor` are pinned as a contract with the HA Energy dashboard. |
| Composition of HA entities and their `device_class` / `state_class` | **Stable** | We will not change these without announcement; it affects entity_id in HA. |
| Internal lambda-accessible methods `RbAmpComponent::*` | **Unstable** | May change in minors. If you write lambda actions against internal APIs — pin the component version (`refresh: <git-sha>` in `external_components:`). |
| NVS-struct format | **Versioned** | Changes are accompanied by a bump of `RBAMP_PREF_VERSION` — old data is silently discarded, not corrupted. That means exactly one 5-minute energy-loss episode on upgrade, no more. |
| Names and behavior of the `i2c:` block | **External** | This is ESPHome core, not ours — follow upstream policy. |
| Wire-protocol surface (module registers) | **Firmware SemVer** | Smart-rules SPEC: minor adds registers; major may renumber. The component detects the firmware version and adapts behavior; see the compatibility table in [README §Compatibility](README.md). |

**What "deprecation warning" means**: two minors before removal, the key
starts logging a warning at boot. That's the signal to change the YAML
before the next minor release. No silent breakage in stable surfaces —
if something does break without warning, that's a bug, file an issue.

**Pre-1.0 caveat**: current 0.x releases are an alpha/beta series. We
strive to follow the policy above already, but we reserve a small amount
of room for critical breaking changes in minors before v1.0.0 — every
such change will be explicitly flagged in this changelog with a
`[BREAKING]` tag and migration instructions.

---

## v0.4.0 — 2026-05-28, protocol v1.2 parity

The component reaches parity with firmware protocol v1.2 in step with sibling Arduino, ESP-IDF and Python v1.1.0 libraries.

### New YAML keys (v0.4.0)

- **`sensor_class:`** — `SCT_013` | `WIRED_CT` | `BUILTIN_CT`, default `SCT_013`. Pins the current sensor family on the module side. On firmware v1.2+ becomes a precondition for writing a CT-clamp model. On earlier firmware, accepted by the schema and applied automatically on upgrade.
- **`ct_models:`** — list of **1–3** `SCT_013_*` enum values for UI2/UI3 with mixed clamp models on different channels (schema: `cv.Length(min=1, max=3)`). Mutually exclusive with global `ct_model:`.

### Changed behavior

- **`ct_model:` now loads factory coefficients** for the chosen model on firmware v1.2+ — writes both noise floor and gain automatically. On earlier firmware it works as in v0.3 (metadata-only).
- **Per-channel configuration for UI3** via `ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]` — the component sets its own model on each channel at boot. Total setup time for three channels is ~2–3 s (per-channel flash write).

### Cross-platform parity

Full parity with Arduino + ESP-IDF + Python v1.1.0:

| Surface | Arduino | ESP-IDF | Python | ESPHome v0.4.0 |
|---|:---:|:---:|:---:|:---:|
| Sensor class (`sensor_class:`) | ✓ | ✓ | ✓ | ✓ |
| Per-channel CT model | ✓ | ✓ | ✓ | ✓ |
| Class precondition for model write | ✓ | ✓ | ✓ | ✓ |

### Breaking changes

None. All changes are additive — old YAML without the new keys continues to work as before.

### Migration from v0.3

For typical deployments no action is required. For explicit v1.2 parity:

1. Add `sensor_class: SCT_013` to the `rbamp:` block (optional — this is the default).
2. If on UI3 you have different clamps on different channels — replace `ct_model:` with `ct_models: [...]`.

Example UI3 with mixed clamps:

```yaml
rbamp:
  id: home_meter
  sensor_class: SCT_013   # default; can be omitted
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
```

More in [03_sensor_selection.md](03_sensor_selection.md) and [06_examples.md](06_examples.md) Example 2.

---

## v0.3 — 2026-05-28, 8-PR resilience audit

Component resilience audit: cross-checked against 8 historical bug shapes in the ESPHome ecosystem (PRs / issues from `esphome/esphome` and `emporia-vue-local/esphome`). Result: 3 PASS, 3 N/A, 2 WOULD-BREAK (1 fixed, 1 hardened with a proactive guard).

### New tests

- **`tests/smoke.yaml`** — a compile-only smoke test for the YAML schema. Exercises `MULTI_CONF` with two instances, all single-sensor keys at once, phased keys on a separate instance, and all behavioral flags (`ct_model`, `bidirectional`, `broadcast_latch`, `topology`). Run: `esphome compile smoke.yaml` from the `tests/` directory. Protects against silent regressions of the `esphome#7081` class, where a refactor breaks the schema without being caught through a bench session.

### Hardening

- Long flash-write windows (~700 ms per model write, ~300 ms post-reset) now feed the watchdog via `App.feed_wdt()`. Protection against future ESPHome scheduler PRs that might shrink the default 15-second `setup()` budget. Added `#include "esphome/core/application.h"`.
- In the example YAMLs (`ui1`, `ui3`, `multi_module`, `address_change`), inline comments were added about safe I²C pins on different ESP32 variants (classic / S2 / S3 / C3). The C++ itself is variant-agnostic; the risky point was YAML defaults targeting classic ESP32.

### Audit verdicts

| Class | Verdict | Note |
|---|---|---|
| Multi-instance state desync (esphome#14152) | PASS | NVS slot key XORs the I²C address + a version constant |
| ADC saturation (esphome#2283) | N/A | Digitization is on the module side — no ESP32 ADC path |
| Multi-ADC contention (esphome#1915) | N/A | I²C bus mutex serializes between instances |
| IDF / Arduino SPI mode (esphome#14824) | N/A | I²C only; no framework-conditional code |
| Silent schema regression (esphome#7081) | WOULD-BREAK → fixed | Added `tests/smoke.yaml` |
| Per-channel cal cross-talk (esphome#12180) | PASS | Slot mapping verified |
| I²C pin defaults across ESP32 variants (emporia-vue#317) | WOULD-BREAK → hardened | YAML headers documented variant-safe pins |
| Boot crash + 0ms watchdog (emporia-vue#411 / #413) | PASS | + proactive `App.feed_wdt()` guard |

### Breaking changes (v0.3)

None. All changes are additive (a new test file, new comments, defensive WDT feeds without behavioral influence in steady state).

### Migration from v0.2 to v0.3

No action required. The `App.feed_wdt()` calls are invisible to users not triggering `ct_model:` or `new_address:` flows. Variant-pin comments are documentation only.

### Credit

The pattern of multi-instance NVS-key derivation (XOR of I²C address + layout version) — the strongest line of defense against the `esphome#14152` class (multi-instance cal-pref collision) — was already in the code before this audit. Credit to the component author: their pattern matched the one later adopted by the ATM90E32 fix in #14152 for the same reason (per-instance isolation + stale-data rejection on layout bump). The audit verified it — did not write it.

---

## v0.2 — 2026-05-24, SPEC §B.5 NACK discipline

This release closes Phase 1 bench acceptance and brings the component in line with firmware v1.0 reality. Ready for production deployment for single-phase rbAmp.

### New YAML keys (v0.2)

- **`topology:`** — `SINGLE` | `SPLIT_PHASE` | `THREE_PHASE`, default `SINGLE`. Informational topology hint — the module on current firmware has no in-band channel-count register. Goes into a `dump_config` line. Will become authoritative once the module starts publishing topology via its register. The `SINGLE` default matches every current SKU — no action required for existing deployments.

### New behavior

- **NACK retry**: each byte of a float32 register is retried up to 3 times with a 5 ms pause between attempts. Reduces the effective bad-read rate from ~20% (at 100 kHz due to NACK behavior in the master stack) to < 1%. See [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference).
- **Weak sanity filter**: replaces the early per-register hard-bound switch. The new filter discards only non-finite floats and values with `|val| > 10000`. **No lower bound** — brownout, mains drop-out (U → 0 V) and off-grid states pass through to HA unfiltered.
- **Warning on `broadcast_latch: true`** at boot when the detected firmware version is `< 0x02`. Explains that v1 firmware silently drops writes to the broadcast address; the component falls back to a sequential latch for each module. The warning disappears automatically when firmware adds support.

### Bug fixes

- **ESPHome 2025.x API break — `get_object_id_hash` removed**: previous NVS-key seed logic called `this->get_object_id_hash()`, a method removed from `PollingComponent` in 2025.x. Replaced with a pattern that XORs the I²C address and a layout-version constant — gives per-instance isolation and stale-data rejection on layout bump. NVS data from pre-2025.x deployments is invalidated by this key change; Wh totals reset to 0 on the first boot after upgrade.

### Doc updates

- `__init__.py` — the `CT_MODELS` block got a multi-line docstring explicitly stating that `ct_model:` on v1 firmware is only writing a model marker.
- `README.md` — sections on CT-model writing, broadcast LATCH limitations on v1, using the topology hint, and cross-references to SPEC §B.5 updated for v1 reality.
- Example YAMLs — `i2c: frequency:` changed from `100kHz` to `50kHz`. An inline comment in each file explains why.
- `example/.gitignore` — added `/bench-*.yaml` so that operator-local bench configs with WiFi credentials and OTA targets do not land in commits.

### Bench acceptance (2026-05-24)

Five acceptance criteria verified on the operator's UI1 bench:

| # | Criterion | Result |
|---|---|---|
| 1 | `esphome compile example/ui1.yaml` clean | PASS (58.71 s, 0 errors) |
| 2 | `dump_config` within 5 s of boot | PASS (setup 67 ms; dump_config @ 121 ms) |
| 3 | Instantaneous entities within ±2% of baseline | PASS (U ≈ 228 V, I ≈ 0.9 A, P ≈ 117 W, PF ≈ 0.57, F = 50 Hz) |
| 4 | NVS energy rises monotonically, survives reboot | PASS (0.051 → 0.134 → 0.310 → ... → 4.069 Wh through several reboots) |
| 5 | Schema validation passes for all 4 example YAMLs | PASS |

Post-fix verification (6 cycles × 60 s, filter-relaxation run): U = 227–228 V, I = 0.81–0.83 A, P = 100–106 W, PF = 0.54–0.56, F = 50 Hz. Zero `Failed to read` or `i2c.master: NACK` in the log over a 6-minute capture.

### Breaking changes (v0.2)

None in terms of YAML configuration. Existing `rbamp:` configurations without the new `topology:` key continue to work (default `SINGLE`). The `broadcast_latch:` warning is informational — no config changes required.

**NVS key change**: users upgrading from pre-2025.x ESPHome (where the old NVS path through `get_object_id_hash` was used) will see Wh totals reset to 0 on the first boot. This is a one-time event. HA long-term statistics energy history is preserved.

---

## v0.1 — 2026-05-23, baseline import

Baseline import of an already-existing ESPHome component into the monorepo alongside the cross-platform family of client libraries. This commit pins the component as it existed before the Phase 1 bring-up session.

### What was in the baseline

- **`rbamp:` YAML schema** with keys `id`, `address`, `update_interval`, `drdy_pin`, `ct_model`, `bidirectional`, `new_address`, `broadcast_latch`. The `topology:` key was not yet present.
- **Sensor platform** with a full set of single-phase and phased fields, the mutex validator, and the companion-field validator.
- **C++ component layer** with I²C read helpers, variant detection, NVS persistence, address change, CT-model write, latch pipeline, RT publish. Per-byte NACK retry was not yet present; the sanity filter was a simple `isfinite` without a `|val| < 10000` cap.
- **Example YAMLs** (`ui1.yaml`, `ui3.yaml`, `multi_module.yaml`, `address_change.yaml`) with `i2c: frequency: 100kHz`.
- **README.md** — configuration reference, migration guide, and known-limitations section.
- **tests/README.md** — checklist of Phase 1 milestone smoke tests.

### State at v0.1

The component was functionally complete for single-phase deployments, but had three divergences from firmware v1.0 reality, fixed in v0.2:

1. No warning on `broadcast_latch: true` (v1 firmware silently drops general-call writes).
2. The `CT_MODELS` docstring did not state that v1 `ct_model:` is metadata-only, does not calibrate NF / gain.
3. No `topology:` key, no explicit topology-hint path.

Additionally, float reads had no retry — vulnerable to the ~20% NACK rate at 100 kHz.

### Breaking changes v0.1 → v0.2

See the NVS key change note in the v0.2 entry above (one-time Wh totals reset).

---

## Forward compatibility with future firmware revisions

The component is fully functional with current firmware. When the planned firmware improvements ship, three points in the component will get minor updates.

### Point 1 — topology detection

**Now**: the component reads the `topology:` YAML key (default `SINGLE`) and uses it as the authoritative source for the number of phases.

**After upgrade**: the component will prefer the topology value published by the module via its register. If the module on old firmware returns 0 — the component falls back to the YAML hint. Cross-compatibility v1.0 / v1.2 + v1.x-with-topology-register / v1.x-with-topology-register is preserved.

### Point 2 — broadcast LATCH

**Now**: with `broadcast_latch: true` and a detected firmware version `< 0x02`, the component logs a warning that broadcast writes are dropped by the module, and falls back to a sequential latch for each module.

**After upgrade**: no code changes required — the gate condition `firmware_version_ < 0x02` already handles this. Once firmware lifts the version above 0x02, the warning stops printing and broadcast writes go through normally.

Verification: after flashing modules with the new version, no "general-call DISABLED" warning should appear in the boot log, and multiple modules should latch in a single bus transaction.

### Point 3 — `ct_model:` metadata → coefficient auto-load

**Now (v0.4.0 + firmware v1.2+)**: writing `ct_model:` automatically loads the noise floor and gain from the factory preset table on the module side. No calibration steps required.

**On earlier firmware**: writing `ct_model:` is only a model marker. The compatibility path is ensured via the `sensor_class:` precondition: a module on early firmware accepts the write (without auto-load), a module on v1.2+ provides auto-load automatically.

### Non-blocking timeline

None of these changes is required in advance. The component is fully functional with current firmware through the YAML hint + warning + metadata-only paths. The points are small (< 10 LOC each) and can be closed in a single commit when new firmware becomes available for bench verification.

---

## Upgrade guide

### From v0.3 to v0.4.0

1. **Update the component sources**: `git pull` (if you cloned the monorepo) or rewrite the `components/rbamp/` directory entirely.
2. **Optional — add new keys to the YAML** for explicit v1.2 parity:

    ```yaml
    rbamp:
      sensor_class: SCT_013         # default; can be omitted
      ct_model: SCT_013_030          # as before
      # or for UI3 with mixed clamps:
      # ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
    ```

3. **No breaking changes**. Old YAMLs work without edits. The real difference shows up only when working with a module on firmware v1.2+ (coefficient auto-load).

### From v0.1 to v0.2 (for historical reference)

1. **Update the component sources** (git pull / rewrite the directory).
2. **Update the YAML**: `i2c: frequency:` from `100kHz` to `50kHz` in every config using rbamp.
3. **No other YAML changes needed**. The new `topology:` key defaults to `SINGLE`, which matches every current SKU.
4. **NVS Wh reset**: the first boot after upgrading from pre-2025.x ESPHome + an older component version will show `No saved energy state — starting from 0 Wh`. A one-time event due to the NVS-key change. HA long-term statistics history is preserved.
5. **Remove `ct_model:` from the YAML** if you added it in a previous session and the flash write has already been applied. On v0.3- the key triggers a flash write again on every boot (on v0.4.0 + v1.2+ this is less of a problem, but it's still a ~700 ms blackout). On v0.4.0 idempotency is guaranteed: repeating the same model write does not produce a second flash write.

### From pre-v0.1 (components copied before the monorepo import)

The pre-monorepo version of the component had no NACK retry, no energy NVS persistence, and no `CT_MODELS` enum. Walk through the v0.1 → v0.2 → v0.3 → v0.4.0 steps above sequentially; the NVS key will not collide (the key namespace is derived from the name, not from a per-installation string).


---

← [Troubleshooting](10_troubleshooting.md) · [Docs index](README.md)
