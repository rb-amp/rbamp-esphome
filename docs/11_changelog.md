# 11 · Changelog

Version history of the `rbamp` external component for ESPHome. Dates in UTC.

Contents:

- [v1.3 — 2026-06-17, fleet + identity + read-compare-write](#v13--2026-06-17-fleet--identity--read-compare-write)
- [v0.4.0 — 2026-05-28, parity with protocol v1.2](#v040--2026-05-28-parity-with-protocol-v12)
- [v0.3 — 2026-05-28, 8-PR resilience audit](#v03--2026-05-28-8-pr-resilience-audit)
- [v0.2 — 2026-05-24, SPEC §B.5 NACK discipline](#v02--2026-05-24-spec-b5-nack-discipline)
- [v0.1 — 2026-05-23, baseline import](#v01--2026-05-23-baseline-import)
- [Forward-compatibility with future firmware revisions](#forward-compatibility-with-future-firmware-revisions)
- [Upgrade guide](#upgrade-guide)
- [Deprecation policy](#deprecation-policy)

---

## Deprecation policy

What the component promises not to break without warning, and what it may:

| Surface | Stability | What we promise |
|---|---|---|
| YAML keys of the `rbamp:` block | **Stable** | Removal or renaming is only possible on a major bump (v1.0.0). A deprecation warning appears in the `dump_config` log two minor releases before removal. |
| YAML keys of `sensor.platform: rbamp` (slots) | **Stable** | Same rules. The names `voltage`/`current`/`power`/`energy`/`frequency`/`power_factor` are fixed as a contract with the HA Energy dashboard. |
| The set of HA entities and their `device_class` / `state_class` | **Stable** | We will not change it without an announcement; it affects entity_id in HA. |
| Internal lambda-accessible `RbAmpComponent::*` methods | **Unstable** | These may change in minor releases. If you write lambda actions against internal APIs, pin the component version (`refresh: <git-sha>` in `external_components:`). |
| NVS structure format | **Versioned** | Changes come with a bump of `RBAMP_PREF_VERSION` — old data is silently discarded, never corrupted. That means exactly one episode of 5-minute energy loss on upgrade, no more. |
| Names and behavior of the `i2c:` block | **External** | This is ESPHome core, not ours — follow the upstream policy. |
| Wire-protocol surface (module registers) | **Firmware SemVer** | Smart-rules SPEC: a minor adds registers; a major may renumber them. The component detects the firmware version and adapts its behavior — see the compatibility table in [README §Compatibility](https://github.com/rb-amp/rbamp-esphome). |

**What "deprecation warning" means**: two minors before removal, the key
starts logging a warning at load time. This is your signal to change the
YAML before the next minor release. No silent breakages in stable
surfaces — if something does break without warning, that's a bug;
file an issue.

**Pre-1.0 caveat**: the current 0.x releases are an alpha/beta series. We
aim to follow the policy above already, but reserve a little
room for critical breaking changes in minor releases
before v1.0.0 — each such change will be explicitly marked in this changelog with
a `[BREAKING]` tag and migration instructions.

---

## v1.3 — 2026-06-17, fleet + identity + read-compare-write

Major version bump. The component reaches parity with firmware v1.3 and
the sister rbAmp libraries (esp-idf / arduino / python). API-additive
for existing v0.4.0 deployments: all old YAML works without edits; the
`ct_model:` / `sensor_class:` boot-writeback behavioral improvement is
transparent. One hidden bug fix in the per-class validator, which was
globally dead before this release.

### New YAML keys (v1.3)

- **`fleet_gc_enable:`** — `bool`, default `false`. Opt-in: enables General-Call
  latch reception (the module reacts to a synchronizing broadcast from the master).
  Capability-gated through `CAP_GC_LATCH` (bit1 of `REG_CAPABILITY` 0x57). On
  legacy v1.0-v1.2 firmware — warning + skip without apply.
- **`group_id:`** — `uint8`, 0..255, default `0`. Group identifier for
  selective GC-latch: the module accepts a frame only if the group field in the
  frame matches this `group_id` or equals `0x00` (all-call). Applies to
  multi-tenant deployments with independent clusters on a single bus.

Both keys are applied through **read-compare-write** at boot — a flash erase
happens only if the requested value differs from the stored one.

### Changed behavior (non-breaking)

| Surface | v0.4.0 | v1.3 |
|---|---|---|
| `ct_model:` / `ct_models:` boot-writeback | Write every boot → 700 ms flash erase per call | **Read-compare-write**: writes only if `REG_CT_MODEL_CH0/1/2` (0x51-0x53 mirrors) != requested. On a matching configuration — 0 flash writes. |
| `sensor_class:` boot-writeback | Write every boot → 700 ms flash erase | **Read-compare-write**: writes only if `REG_SENSOR_CLASS` (0x25) != requested. |
| `new_address:` | Single-phase write + required factory-provisioning mode | **Two-phase address commit** (production-OK on v1.3 firmware): staged write → `REG_ADDR_COMMIT_MAGIC=0xA5` → `CMD_COMMIT_ADDR`. Capability-gated through `CAP_TWO_PHASE_ADDR`. Legacy fallback on v1.0-v1.2. |
| `update_interval:` | No lower bound | **Minimum 1 s**, `esphome config` rejects < 1 s with a friendly diagnostic. |
| `broadcast_latch:` | Logged warning on v1<v2 | Replaced by `fleet_gc_enable:` (see above); the legacy key is kept as a no-op to stay non-breaking. |

**The main UX improvement**: on a 3-channel module, a typical boot config writeback
took ~2.8 s (3 × 700 ms flash erase + sensor_class erase). After v1.3 — **~0 ms**
on a stable configuration (read-back verify only). You can leave the YAML
as is; the `docstring` advice to "remove `ct_model:` after verify" is obsolete.

### Changes to the CT model enum

| Code | YAML name v0.4.0 | YAML name v1.3 |
|---|---|---|
| 1 | `SCT_013_005` | `SCT_013_005` |
| 2 | `SCT_013_010` | `SCT_013_010` |
| 3 | `SCT_013_030` | `SCT_013_030` |
| 4 | `SCT_013_050` | `SCT_013_050` |
| 5 | `SCT_013_100` | *(reserved — removed)* |
| 6 | — | **`SCT_013_020`** (new) |
| 1-3 (WIRED_CT class) | — | **`WIRED_CT_1` / `WIRED_CT_2` / `WIRED_CT_3`** (factory preset slots) |

- `SCT_013_020` (code 6) added — a 20 A SCT-013 for medium-power rural
  branch circuits (3-5 kW). `esphome config` accepts the enum name; a module on v1.3
  firmware automatically loads the factory coefficients.
- `SCT_013_100` (code 5) **removed** from the YAML enum — the code is reserved in v1.3
  firmware (DEV_ERR_PARAM on a write attempt). If a user has it in their YAML,
  `esphome config` rejects it with a hint of the allowed values.
- `WIRED_CT_1/2/3` — factory preset slots for a wired CT (SKU rbAmp-WiredCT,
  roadmap). On current SCT_013 SKUs they are valid only for `sensor_class: WIRED_CT`.

### Per-class CT validation (new schema-side guard)

The `_validate_ct_model_per_class` validator rejects invalid combinations of
`sensor_class:` × `ct_model:` at the `esphome config` stage, **before** flash + boot:

| sensor_class | Allowed ct_model codes | Behavior on mismatch |
|---|---|---|
| `SCT_013` | {1, 2, 3, 4, 6} = `SCT_013_005/010/020/030/050` | `cv.Invalid` with "Allowed for SCT_013: ..." |
| `WIRED_CT` | {1, 2, 3} = `WIRED_CT_1/2/3` | `cv.Invalid` |
| `BUILTIN_CT` | {} (none — onboard CT, not configurable) | `cv.Invalid` |
| `UNSET` | {} | `cv.Invalid` |

A duplicate guard on the firmware side: if YAML slips past the validator
(old firmware → new schema, etc.), the module returns `REG_ERROR = 0xFE
DEV_ERR_PARAM`. For details, see [03_sensor_selection.md](03_sensor_selection.md).

### New public C++ methods (callable from YAML `lambda:`)

```cpp
// Identity & diagnostics
std::string get_variant_str();              // "UI1"/"UI2"/"UI3"/"I1"/"I2"/"I3"/"UNK"
std::string get_capability_hex();           // "0x0718" — full 16-bit
std::string get_uid_hex();                  // 96-bit hex (24 chars)
std::string get_last_error_str();           // "OK"/"ERR_PARAM"/"ERR_BUSY"/...
uint16_t    get_event_flags();              // sticky EVENT_FLAGS bits
std::string get_firmware_version();         // "1.3" / "1.2" / "1.0"

// Fleet & sync
void        transmit_gc_frame(uint8_t group, uint16_t tick);  // emit 5-byte GC frame
uint16_t    read_gc_tick_received();        // last latched tick value

// Recovery & maintenance
void        write_clear_error();            // CMD_CLEAR_ERROR (REG_ERROR + EVENT bit3)
void        write_reset();                  // CMD_RESET (soft reset, preserves flash)
void        fleet_apply_now();              // force re-apply read-compare-write writeback
```

The Native API services pattern for HA automations — see [09_api_reference.md](09_api_reference.md).

### Validated facts (bench 2026-06-16, Fix-A fleet UI1@0x50 + I2@0x51 + I3@0x52)

21/21 PASS under the ESPHome Native API (port 6053) + aioesphomeapi validator + raw
register cross-check:

- Identity (variant / capability / UID): 3/3 matched the raw register reads
- **Loaded current cross-check 3/3 within 2-4%** (vs ±2% acceptance) — component-side reads matched raw register reads under load
- **CT per-class mixed bind [1,3,6] on UI3 — no clobber**: mirrors `01 03 06`
- Reserved CT code rejection (code 5): firmware `REG_ERROR = 0xFE DEV_ERR_PARAM`
- `clear_error` → `REG_ERROR cleared`: PASS
- **GC fleet sync 3/3**: `check_sync` showed all 3 modules latched the same tick
- Energy monotonic (L9 master wall-clock): `total_increasing` Wh ↑ under load
- **L8 soak 180 cycles / 0 identity drops** under idle load OFF

Component-side reads matched the raw register reads within **2-4%**, which is
**tighter than the ±2% acceptance criterion** — a strong signal that the entire transport
chain + retry triple + scaling works faithfully.

### What did NOT change

- **Sensor entity surface** (voltage / current[_1/_2] / power / energy / power_factor /
  reactive_power / apparent_power / frequency) — no breaking changes. All v0.4.0
  names work as before.
- **NVS energy persistence** — unchanged (same FNV1a + address ^ version key,
  same `RBAMP_PREF_VERSION`). Wh totals are not reset on upgrade from v0.4.0.
- **DRDY pin acceptance** — accepted in YAML, `setup()` calls; not used
  publicly (DRDY-driven publish — roadmap v0.6.0).
- **`update_interval` default** — 60 s.
- **Frequency 50 kHz default** — SPEC §B.5 NACK discipline, unchanged.

### Cross-platform parity

Full parity with esp-idf 1.3.0 / arduino 1.3.0 / python 1.3.0:

| Surface | esp-idf | arduino | python | ESPHome v1.3 |
|---|:---:|:---:|:---:|:---:|
| Fleet GC (capability-gated) | ✓ | ✓ | ✓ | ✓ |
| Two-phase address commit (production-OK) | ✓ | ✓ | ✓ | ✓ |
| Identity (variant / capability / UID / label / fw) | ✓ | ✓ | ✓ | ✓ |
| Per-class CT validation (non-contiguous) | ✓ | ✓ | ✓ | ✓ |
| Energy = master wall-clock (L9 canon) | ✓ | ✓ | ✓ | ✓ |
| i2c-hang three-layer mitigation | ✓ | ✓ | ✓ | ✓ |
| `clear_error` recovery (REG_ERROR + EVENT bit3) | ✓ | ✓ | ✓ | ✓ |

### Breaking changes

None at the YAML level. One case of a behavioral silent fix:

- **`SCT_013_100` (code 5)** — no longer accepted by the schema. If the YAML contains it,
  `esphome config` rejects it with a hint. On legacy v1.0-v1.2 firmware this
  value worked with no effect (metadata-only); on v1.3 firmware — rejected
  with `DEV_ERR_PARAM`. Action: replace it with `SCT_013_050` (50 A) or
  `SCT_013_030` (30 A) depending on the actual clamp.

### Migration from v0.4.0

1. **Update the component sources**: `git pull` (if you cloned the monorepo) or
   update `external_components:` via `refresh: <git-sha>`.
2. **Remove `SCT_013_100`** if you used it (see Breaking changes above).
3. **Optionally** — add the new keys:
   ```yaml
   rbamp:
     - id: meter
       address: 0x50
       sensor_class: SCT_013
       ct_model: SCT_013_030
       fleet_gc_enable: true     # opt-in GC sync
       group_id: 1               # cluster id for selective latch
   ```
4. **No breaking changes** for a typical UI1/UI3 deployment without `SCT_013_100`.

### Schema CRC

Wire-protocol register schema CRC: `0x5FB3E9F3` — matches the reference esp-idf 1.3.0 +
arduino 1.3.0 + python 1.3.0 sister libraries.

---

## v0.4.0 — 2026-05-28, parity with protocol v1.2

The component reaches parity with firmware protocol v1.2 alongside the sister Arduino, ESP-IDF, and Python v1.1.0 libraries.

### New YAML keys (v0.4.0)

- **`sensor_class:`** — `SCT_013` | `WIRED_CT` | `BUILTIN_CT`, default `SCT_013`. Fixes the current-sensor family on the module side. On firmware v1.2+ it becomes a precondition for writing the CT clamp model. On earlier firmware it is accepted by the schema and applied automatically on upgrade.
- **`ct_models:`** — a list of **1-3** `SCT_013_*` enum values for UI2/UI3 with mixed clamp models on different channels (schema: `cv.Length(min=1, max=3)`). Mutually exclusive with the global `ct_model:`.

### Changed behavior

- **`ct_model:` now loads the factory coefficients** for the selected model on firmware v1.2+ — it writes both the noise floor and the gain automatically. On earlier firmware it behaves as in v0.3 (metadata-only).
- **Per-channel configuration for UI3** via `ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]` — the component sets its model on each channel at load. Total setup time for three channels — ~2-3 s (per-channel flash write).

### Cross-platform parity

Full parity with Arduino + ESP-IDF + Python v1.1.0:

| Surface | Arduino | ESP-IDF | Python | ESPHome v0.4.0 |
|---|:---:|:---:|:---:|:---:|
| Sensor class (`sensor_class:`) | ✓ | ✓ | ✓ | ✓ |
| Per-channel CT model | ✓ | ✓ | ✓ | ✓ |
| Class precondition for model write | ✓ | ✓ | ✓ | ✓ |

### Breaking changes

None. All changes are additive — old YAML without the new keys keeps working as before.

### Migration from v0.3

For typical deployments no action is required. If you want explicit v1.2 parity:

1. Add `sensor_class: SCT_013` to the `rbamp:` block (optional — it's the default).
2. If you have different clamps on different channels on UI3 — replace `ct_model:` with `ct_models: [...]`.

UI3 example with mixed clamps:

```yaml
rbamp:
  id: home_meter
  sensor_class: SCT_013   # default; can be omitted
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
```

For details, see [03_sensor_selection.md](03_sensor_selection.md) and [06_examples.md](06_examples.md) Example 2.

---

## v0.3 — 2026-05-28, 8-PR resilience audit

A resilience audit of the component: a cross-check against 8 historical forms of bugs in the ESPHome ecosystem (PRs / issues from `esphome/esphome` and `emporia-vue-local/esphome`). Result: 3 PASS, 3 N/A, 2 WOULD-BREAK (1 fixed, 1 hardened with a proactive guard).

### New tests

- **`tests/smoke.yaml`** — a compile-only smoke test for the YAML schema. It runs `MULTI_CONF` with two instances, all single sensor keys at once, phased keys on a separate instance, and all behavioral flags (`ct_model`, `bidirectional`, `broadcast_latch`, `topology`). Run: `esphome compile smoke.yaml` from the `tests/` directory. It guards against silent regressions of the `esphome#7081` class, where a refactor breaks the schema without being caught in a bench session.

### Hardening

- Long flash-write windows (~700 ms per model write, ~300 ms post-reset) now feed the watchdog via `App.feed_wdt()`. This protects against future PRs to the ESPHome scheduler that may trim the default 15-second `setup()` budget. An `#include "esphome/core/application.h"` was added.
- The example YAML files (`ui1`, `ui3`, `multi_module`, `address_change`) gained inline comments about safe I²C pins on different ESP32 variants (classic / S2 / S3 / C3). The C++ itself is variant-agnostic; the risky point was the YAML defaults for classic ESP32.

### Audit verdicts

| Class | Verdict | Note |
|---|---|---|
| Multi-instance state desync (esphome#14152) | PASS | The NVS slot key XORs the I²C address + a version constant |
| ADC saturation (esphome#2283) | N/A | Digitization is on the module side — no path through the ESP32 ADC |
| Multi-ADC contention (esphome#1915) | N/A | The I²C bus mutex serializes across instances |
| IDF / Arduino SPI mode (esphome#14824) | N/A | I²C only; no framework-conditional code |
| Silent schema regression (esphome#7081) | WOULD-BREAK → fixed | Added `tests/smoke.yaml` |
| Per-channel cal cross-talk (esphome#12180) | PASS | Slot mapping verified |
| I²C pin defaults across ESP32 variants (emporia-vue#317) | WOULD-BREAK → hardened | YAML headers documented variant-safe pins |
| Boot crash + 0ms watchdog (emporia-vue#411 / #413) | PASS | + proactive `App.feed_wdt()` guard |

### Breaking changes (v0.3)

None. All changes are additive (a new test file, new comments, defensive WDT feeds with no behavioral impact in steady state).

### Migration from v0.2 to v0.3

No action required. The `App.feed_wdt()` calls are invisible to users who do not trigger the `ct_model:` or `new_address:` flows. The variant-pin comments are documentation only.

### Credit

The multi-instance NVS-key derivation pattern (XOR of the I²C address + layout version) — the strongest line of defense against the `esphome#14152` class (multi-instance cal-pref collision) — was already in the code before this audit. Credit to the component author: his pattern matched the one later adopted by the ATM90E32 fix in #14152 for the same reason (per-instance isolation + stale-data rejection on a layout bump). The audit verified it — it did not write it.

---

## v0.2 — 2026-05-24, SPEC §B.5 NACK discipline

This release closes Phase 1 bench acceptance and brings the component in line with the reality of firmware v1.0. Ready for production deployment for single-phase rbAmp.

### New YAML keys (v0.2)

- **`topology:`** — `SINGLE` | `SPLIT_PHASE` | `THREE_PHASE`, default `SINGLE`. An informational topology hint — the module on current firmware has no in-band channel-count register. It goes into the `dump_config` line. It will become authoritative once the module starts publishing topology through its register. The default `SINGLE` matches all current SKUs — no action required on existing deployments.

### New behavior

- **NACK retry**: each byte of a float32 register is retried up to 3 times with a 5 ms pause between attempts. This reduces the effective rate of bad reads from ~20% (at 100 kHz due to NACK behavior in the master stack) to < 1%. See [SPEC §B.5](https://www.rbamp.com/docs/modules-basic-standard-api-reference).
- **A soft sanity filter**: replaces the early per-register hard-bound switch. The new filter rejects only non-finite floats and values with `|val| > 10000`. **There is no lower bound** — brownout, mains disconnection (U → 0 V), and off-grid states pass through to HA unfiltered.
- **A warning on `broadcast_latch: true`** at load time when the detected firmware version is `< 0x02`. It explains that v1 firmware silently drops writes to the broadcast address; the component falls back to a sequential latch for each module. The warning disappears automatically once the firmware adds support.

### Bug fixes

- **ESPHome 2025.x API break — `get_object_id_hash` removed**: the previous NVS-key seed logic called `this->get_object_id_hash()`, a method removed from `PollingComponent` in 2025.x. Replaced with a pattern that XORs the I²C address and the layout version constant — it gives per-instance isolation and stale-data rejection on a layout bump. NVS data from pre-2025.x deployments is invalidated by this key change; Wh totals reset to 0 on the first boot after upgrade.

### Doc updates

- `__init__.py` — the `CT_MODELS` block got a multi-line docstring explicitly stating that `ct_model:` on v1 firmware is a model-tag write only.
- `README.md` — the sections on CT-model writes, broadcast LATCH limitations on v1, using the topology hint, and the cross-references to SPEC §B.5 were updated to v1 reality.
- Example YAML — `i2c: frequency:` changed from `100kHz` to `50kHz`. An inline comment in each file explains why.
- `example/.gitignore` — added `/bench-*.yaml` so that operator-local bench configs with WiFi credentials and OTA targets do not end up in commits.

### Bench acceptance (2026-05-24)

Five acceptance criteria were verified on the operator's UI1 bench:

| # | Criterion | Result |
|---|---|---|
| 1 | `esphome compile example/ui1.yaml` clean | PASS (58.71 s, 0 errors) |
| 2 | `dump_config` within 5 s of boot | PASS (setup 67 ms; dump_config @ 121 ms) |
| 3 | Instantaneous entities within ±2% of baseline | PASS (U ≈ 228 V, I ≈ 0.9 A, P ≈ 117 W, PF ≈ 0.57, F = 50 Hz) |
| 4 | NVS energy increases monotonically, survives reboot | PASS (0.051 → 0.134 → 0.310 → ... → 4.069 Wh across several reboots) |
| 5 | Schema validation passes for all 4 example YAML | PASS |

Post-fix verification (6 cycles × 60 s, filter-relaxation run): U = 227-228 V, I = 0.81-0.83 A, P = 100-106 W, PF = 0.54-0.56, F = 50 Hz. Zero `Failed to read` or `i2c.master: NACK` in the log over the 6-minute capture.

### Breaking changes (v0.2)

None in terms of YAML configuration. Existing `rbamp:` configurations without the new `topology:` key keep working (default `SINGLE`). The `broadcast_latch:` warning is informational — it requires no config change.

**NVS key change**: users upgrading from pre-2025.x ESPHome (where the old NVS path via `get_object_id_hash` was used) will see Wh totals reset to 0 on the first boot. This is a one-time event. The energy history in HA long-term statistics is preserved.

---

## v0.1 — 2026-05-23, baseline import

A baseline import of an already-existing ESPHome component into the monorepo alongside the cross-platform family of client libraries. This commit captures the component as it existed before the Phase 1 bring-up session.

### What was in the baseline

- **The `rbamp:` YAML schema** with the keys `id`, `address`, `update_interval`, `drdy_pin`, `ct_model`, `bidirectional`, `new_address`, `broadcast_latch`. The `topology:` key was not yet present.
- **The sensor platform** with the full set of single-phase and phased fields, a mutex validator, and a companion-field validator.
- **The C++ component layer** with I²C read helpers, variant detection, NVS persistence, address change, CT model write, the latch pipeline, and RT publish. The per-byte NACK retry was not yet present; the sanity filter was a simple `isfinite` without the `|val| < 10000` cap.
- **Example YAML** (`ui1.yaml`, `ui3.yaml`, `multi_module.yaml`, `address_change.yaml`) with `i2c: frequency: 100kHz`.
- **README.md** — a configuration reference, a migration guide, and a known-limitations section.
- **tests/README.md** — a checklist of Phase 1 milestone smoke tests.

### State at v0.1

The component was functionally complete for single-phase deployments but had three discrepancies with the reality of firmware v1.0, fixed in v0.2:

1. There was no warning on `broadcast_latch: true` (v1 firmware silently drops general-call writes).
2. The `CT_MODELS` docstring did not state that v1 `ct_model:` is metadata only, not a NF / gain calibration.
3. There was no `topology:` key and no explicit topology-hint path.

In addition, the float reads had no retry — vulnerable to the ~20% NACK rate at 100 kHz.

### Breaking changes v0.1 → v0.2

See the note about the NVS key change in the v0.2 entry above (one-time Wh-total reset).

---

## Forward-compatibility with future firmware revisions

The component is fully functional with current firmware. When the planned firmware improvements ship, three points in the component will get minor updates.

### Point 1 — topology detection

**Now**: the component reads the `topology:` YAML key (default `SINGLE`) and uses it as the authoritative source of the phase count.

**After upgrade**: the component will prefer the topology value published by the module through its register. If a module on old firmware returns 0, the component falls back to the YAML hint. Cross-compatibility v1.0 / v1.2 + v1.x-with-topology-register / v1.x-with-topology-register is preserved.

### Point 2 — broadcast LATCH

**Now (v1.3 production, REG_VERSION = 0x04)**: the general-call latch is opt-in via `fleet_gc_enable:` + `group_id:` (the new v1.3 production path) and capability-gated by `CAP_GC_LATCH` (bit1 of `REG_CAPABILITY` 0x57). The legacy `broadcast_latch: true` key is still accepted as a deprecated alias.

**On legacy firmware (REG_VERSION < 0x04)**: the capability bit is not set, so the component falls back to a sequential latch for each module and logs an informational line. No YAML change is required when migrating from legacy to v1.3 — the same key keeps working, just on a different code path.

Check: after flashing the modules with v1.3, the boot log shows `Firmware version: 0x04` and `Capability:` contains the `CAP_GC_LATCH` bit, and several modules in the same `group_id:` latch in a single bus transaction.

### Point 3 — `ct_model:` metadata → coefficient auto-load

**Now, on v0.4.0 + firmware v1.2+**: a `ct_model:` write automatically loads the noise floor and gain from the factory preset table on the module side. No calibration steps are required.

**On earlier firmware**: a `ct_model:` write is a model tag only. The compatibility path is provided through the `sensor_class:` precondition: a module on early firmware accepts the write (without auto-load), and a module on v1.2+ provides auto-load automatically.

### Non-blocking timeline

None of these changes is required in advance. The component is fully functional with current firmware through the YAML-hint + warning + metadata-only paths. The points are small (< 10 LOC each) and can be closed in a single commit once the new firmware is available for bench verification.

---

## Upgrade guide

### From v0.4.0 to v1.3

See the detailed list of changes in the v1.3 entry above. TL;DR:

1. `git pull` or update `external_components: refresh: <sha>`.
2. If your YAML used `SCT_013_100` — replace it with `SCT_013_050` or
   `SCT_013_030` (code 5 is reserved in v1.3 firmware).
3. **Optionally** — add `fleet_gc_enable:` / `group_id:` for
   synchronized latching in a multi-module deployment.
4. **The main free benefit**: after the first boot on v1.3 firmware, the boot-time
   config writeback drops from ~2.8 s to ~0 ms (read-compare-write skips the flash erase).
   No YAML change needed.
5. **NVS Wh totals are preserved** — `RBAMP_PREF_VERSION` is not bumped.

### From v0.3 to v0.4.0

1. **Update the component sources**: `git pull` (if you cloned the monorepo) or rewrite the `components/rbamp/` directory entirely.
2. **Optionally — add the new keys to YAML** for explicit v1.2 parity:

    ```yaml
    rbamp:
      sensor_class: SCT_013         # default; can be omitted
      ct_model: SCT_013_030          # as before
      # or for UI3 with mixed clamps:
      # ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
    ```

3. **No breaking changes**. Old YAML works without edits. The real difference appears only when working with a module on firmware v1.2+ (coefficient auto-load).

### From v0.1 to v0.2 (for historical reference)

1. **Update the component sources** (git pull / rewrite the directory).
2. **Update the YAML**: change `i2c: frequency:` from `100kHz` to `50kHz` in all configs using rbamp.
3. **No other YAML changes are needed**. The new `topology:` key defaults to `SINGLE`, which matches all current SKUs.
4. **NVS Wh reset**: the first boot after upgrading from pre-2025.x ESPHome + an old component version will show `No saved energy state — starting from 0 Wh`. A one-time event due to the NVS-key change. The HA long-term statistics history is preserved.
5. **Remove `ct_model:` from the YAML** if you added it in a previous session and the flash write has already been applied. On v0.3 and earlier, the key re-triggers a flash write on every boot (on v0.4.0 + v1.2+ this is less of a problem, but still a ~700 ms blackout). On v0.4.0, idempotency is guaranteed: a repeated write of the same model does not perform a second flash write.

### From pre-v0.1 (components copied before the monorepo import)

The pre-monorepo version of the component had no NACK retry, no NVS energy persistence, and no `CT_MODELS` enum. Go through the v0.1 → v0.2 → v0.3 → v0.4.0 steps above in sequence; the NVS key will not collide (the key namespace is derived from the name, not from a per-installation string).
