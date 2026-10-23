# 02 · Module Tiers

![The two rbAmp module lines, BASIC and STANDARD (STD), and where energy is computed on each.](images/module-lines.png)

rbAmp ships in two module lines: **BASIC** and **STANDARD (STD)**. Each tier is a complete combination of hardware revision and firmware build, not just a flag in the firmware. Moving between tiers requires physically swapping the module, not a firmware upgrade.

The ESPHome component works with both module lines from **one YAML**: the schema is shared, and the component identifies the connected module line (BASIC or STANDARD (STD)) automatically at boot. The differences are what data the module actually exports and which YAML keys are meaningful on each module line.

## Tiers

### BASIC — entry level

The firmware keeps **unidirectional consumption metering**: the average active power over a period can never be negative — reverse segments within the window are zeroed before integration. Behaviorally this resembles a classic mechanical disc meter.

- Energy is integrated on the ESP32 from the period-average power the module returns after each snapshot, and saved to the ESP32 flash (NVS).
- The `energy_exported` sensor exists in the YAML schema (for config compatibility), but on BASIC it always publishes `0`.
- The grand-total energy sensors (`energy_total`, `energy_exported_total`, `energy_net`, `reactive_energy`, `reactive_energy_exported`) validate but never publish on BASIC.
- Typical applications: apartments without on-site generation, sub-metering of specific loads (water heaters, motors, household appliances, HVAC).

### STANDARD (STD) — bidirectional, on-device energy

STANDARD (STD) is the current module line. It measures **import and export separately** and keeps its **own cumulative energy counters inside the module**.

- **Same YAML.** The component recognises a STANDARD (STD) module at boot and takes the channel count, variant and available features from the module. You do not set a tier and you do not need `topology:`.
- **Energy from the module's counters.** The component accumulates the module's counters instead of integrating power itself, so totals **survive restarts of both the ESP32 and the module**, stay **monotonic** across module restarts and counter resets, and do not drift. The values are additionally saved to the ESP32 flash (NVS) every 5 minutes and on a clean shutdown (reboot / OTA).
- **New sensors:** `energy_total`, `energy_exported_total`, `energy_net` (import − export, may decrease), `reactive_energy`, `reactive_energy_exported` — grand totals over all channels; on three-voltage variants also `voltage_angle_b`, `voltage_angle_c` and `phase_sequence`.
- **Per-channel `energy` and `energy_exported`** (with `bidirectional: true`) come from the same counters.
- **`start_current_ua:`** sets a starting current below which a channel counts no energy, so noise at no load does not creep into the counters.
- Typical applications: homes with solar/wind generation, battery storage, EV chargers, any installation where import and export must be metered separately, and installations where energy totals must survive restarts.

On an STD module energy comes from the module's own counters; `latch_interval` applies only to BASIC.

Keys that exist only for the BASIC module line (`fleet_gc_enable:`, `group_id:`, `broadcast_latch:`, `latch_interval:` for the master-side energy path) are ignored on a STANDARD module with a log line.

![Energy accounting on BASIC (master integrates the period average power) versus STANDARD (master reads the module's cumulative counters).](images/energy-basic-vs-std.png)

### PRO

PRO is not implemented; the component has no PRO-specific keys.

## Capability Matrix

| Capability | BASIC | STANDARD |
|---|:---:|:---:|
| RMS voltage | yes | yes |
| RMS current (1..3 channels, more on higher-channel variants) | yes | yes |
| Active power (signed float) | yes | yes |
| Power factor | yes | yes |
| Reactive power (read from the module, channels 0..2; unsigned, computed as √(S² − P²), includes distortion; no accuracy stated) | yes | yes |
| Apparent power (CH0 only) | yes (`U × I[0]`, computed on the ESP32) | yes (from the module) |
| Line frequency | yes | yes |
| `energy` sensor (import Wh), saved to NVS | yes (integrated on the ESP32) | yes (from on-device counters) |
| `energy_exported` sensor (needs `bidirectional: true`) | always 0 | yes |
| Energy survives ESP32 *and* module restarts | ESP32 only | yes |
| Grand totals: `energy_total`, `energy_exported_total`, `energy_net` | no | yes |
| Reactive energy: `reactive_energy`, `reactive_energy_exported` | no | yes |
| `start_current_ua:` | no | yes |
| `voltage_angle_b/_c`, `phase_sequence` (three-voltage variants) | no | yes |
| Per-channel `ct_models:` | yes | only if the module stores per-channel models; otherwise refused with a log line |
| `fleet_gc_enable:` / `group_id:` general-call sync | yes (v1.3+ firmware) | ignored |

> The component reads only what sensors are declared for — an undeclared register is never requested.

## YAML Keys by Tier

### `bidirectional:` (default `false`)

Enables the per-channel export-energy sensors (`energy_exported`, `energy_exported_1`, `energy_exported_2`).

```yaml
rbamp:
  id: solar_meter
  address: 0x51
  bidirectional: true   # meaningful on STANDARD; on BASIC the export sensors read 0
```

On STANDARD the export energy is taken from the module's counters. On BASIC the key is accepted by the schema without error, but `energy_exported` always returns 0. The grand-total sensors `energy_exported_total` and `energy_net` do not need this key.

### `sensor_class:` — pin the sensor family

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013   # default SCT_013
```

Accepted on both module lines. On firmware v1.2+ the value is written to flash and becomes a **precondition** for writing the CT model: the module refuses to write the model if the class is not set. A value the module already has is not written again.

Enum values: `SCT_013` (split-core SCT-013 clamps, default), `WIRED_CT` and `BUILTIN_CT` (modules supplied calibrated together with their own fitted sensor). On such a factory-paired module a class or model change from YAML is refused with a log line, to protect the individual calibration.

### `ct_model:` / `ct_models:` — CT clamp model

A single global profile for all channels:

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_030   # all channels — SCT-013-030
```

Or a per-channel profile (for UI3 with mixed clamps):

```yaml
rbamp:
  id: meter_ui3
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
```

Accepted on both module lines, mutually exclusive. On firmware v1.2+ the module automatically loads the factory coefficients for the selected model — no additional calibration steps are required. On earlier firmware the value is stored as a model marker.

- Switching the SCT rating keeps the factory accuracy: the component completes the switch (a module restart in the flow is normal on firmware up to 1.4.19; newer firmware corrects on the module) and scales readings to the selected sensor.
- Write guards: an equal value is not written; on a factory-paired module the change is refused with a log line. `ct_model_force: true` (service option, only valid together with `ct_model:` / `ct_models:`) overrides this deliberately.
- Per-channel `ct_models:` on a STANDARD module needs the module to store per-channel models; otherwise the component refuses loudly (warning, keys ignored) — use `ct_model:`.

For more on selecting a model, see [03_sensor_selection.md](03_sensor_selection.md).

### `new_address:` — one-time I²C address provisioning

```yaml
rbamp:
  id: meter1
  address: 0x50
  new_address: 0x51   # one-shot; remove after a successful boot
```

Accepted on both module lines. On firmware v1.3+ the address change is **production-OK** — the component uses a two-phase commit protocol (stage → arm → commit → reboot) with no special mode required. On legacy firmware (v1.0-v1.2) the single-phase fallback requires **factory-provisioning mode**; consult the module documentation or your supplier for the procedure to enable it. Factory-provisioning mode is still required on all firmware versions for **calibration (`saveGains`) and factory-reset** operations.

Execution flow over a single boot: the component polls the current address, writes the new address and saves it to flash, resets the module, and verifies the response at the new address. After success, update the YAML to `address: 0x51` and remove `new_address:`. If the component finds that the old address is silent while the new one responds, it adapts automatically with a warning in the log (in case a previous boot already applied the change).

### `fleet_gc_enable:` / `group_id:` — synchronous snapshot for a multi-module bus

```yaml
rbamp:
  - id: meter1
    fleet_gc_enable: true   # v1.3 production path — recommended
    group_id: 1             # cluster id (modules in the same group snapshot together)
```

When enabled, the component sends a 5-byte general-call frame to the I²C broadcast address — synchronizing the period snapshot across all modules in the same `group_id` in a single transaction.

On v1.3 BASIC firmware the general-call snapshot is **opt-in** and capability-gated. The component checks at boot whether the firmware advertises it and enables the path automatically when both the YAML key and the firmware agree. On legacy firmware (v1.0..v1.2) it is not advertised; the component falls back to a sequential snapshot for each module individually and logs an informational line. On a STANDARD module these keys are ignored with a log line.

The legacy `broadcast_latch:` key is still accepted by the schema for backward compatibility but has **no effect**. Use `fleet_gc_enable:` + `group_id:`.

The skew between modules on a 50 kHz bus is on the order of ~540 µs × N, which is less than 0.003% of a 60-second period. On daily HA totals this unevenness is imperceptible.

### `topology:` — topology hint

```yaml
rbamp:
  id: meter1
  topology: SINGLE      # SINGLE / SPLIT_PHASE / THREE_PHASE
```

Optional and **not needed on STANDARD modules**, where the channel count and variant come from the module itself. On BASIC modules the component infers the channel count from the declared sensor slots (if `current_1` is declared, there are at least 2 channels). The key is cosmetic and goes into the `dump_config` log line.

### `drdy_pin:` — optional data-ready interrupt

```yaml
rbamp:
  id: meter1
  drdy_pin: GPIO15
```

Accepted on both module lines. Connects to the module's optional open-drain DRDY output. The pin is configured and logged, but the instantaneous registers are polled on `update_interval` regardless. With a typical `update_interval: 60s` the key can be omitted.

Requires a 10 kΩ pull-up on the master GPIO to 3.3 V (the output is open-drain; there is no internal pull-up on the module side). See [04_hardware.md](04_hardware.md).

## Summary Table — YAML Key × Module Line

| YAML key | BASIC | STANDARD | Notes |
|---|:---:|:---:|---|
| `address:` | yes | yes | — |
| `update_interval:` | yes | yes | Minimum `1s` |
| `latch_interval:` | yes | n/a | Decouples the snapshot/energy cadence from the read cadence; STANDARD reads its counters on every `update()` |
| `drdy_pin:` | yes | yes | Optional, needs a 10 kΩ pull-up; not used as a read trigger |
| `topology:` | yes | not needed | Informational hint |
| `sensor_class:` | yes | yes | Not rewritten when equal; refused on factory-paired modules |
| `ct_model:` / `ct_models:` | yes | yes | Mutually exclusive; per-channel list on STANDARD needs a module that stores it |
| `ct_model_force:` | yes | yes | Service option; schema requires a model key |
| `bidirectional:` | accepted (export reads 0) | works | Needed for `energy_exported*` per-channel sensors |
| `start_current_ua:` | ignored with a warning | works | `0` or `10`..`2540` in steps of `10` |
| `new_address:` | yes | yes | Production-OK on v1.3+ BASIC firmware; one-shot |
| `fleet_gc_enable:` / `group_id:` / `broadcast_latch:` | yes | ignored | General-call sync is a BASIC-line feature |
| `energy_exported` sensor | always 0 | works | Needs `bidirectional: true` |
| `energy_total`, `energy_exported_total`, `energy_net`, `reactive_energy[_exported]` | never publish | works | Grand totals from on-device counters |
| `voltage_angle_b/_c`, `phase_sequence` | never publish | works on three-voltage variants with a firmware that provides them | See [09 §2.2–2.3](09_api_reference.md) |
| Phased sensors (`voltage_a/b/c`, `current_a/b/c`, …) | no values | publish for the channels the module reports | Details in [09 §2.2](09_api_reference.md) |

## What's Next

- [03_sensor_selection.md](03_sensor_selection.md) — choosing a CT clamp and the behavior of `ct_model:` / `ct_models:`
- [04_hardware.md](04_hardware.md) — physical wiring, GPIO selection

