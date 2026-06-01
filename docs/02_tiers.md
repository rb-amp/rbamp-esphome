# 02 · Module tiers

rbAmp ships in three product lineups: **BASIC**, **STANDARD**, and **PRO**. Each tier is a complete combination of a hardware revision and a firmware build, not just a flag in the firmware. Moving between tiers requires a physical module swap, not an upgrade.

The ESPHome component works with all three tiers the same way: the YAML schema is shared, only what data the module actually emits and which YAML keys make sense on each tier differs.

## Tiers

### BASIC — entry level

The analog front-end is optimized for cost and suits typical residential loads. The firmware performs **unidirectional consumption metering**: the average active power over a period cannot be negative — reverse segments inside the window are zeroed before integration. Behaviorally it resembles a classic mechanical disc meter.

- The `energy_exported` sensor exists in the YAML schema (for config compatibility), but on BASIC it always publishes `0`.
- Typical applications: apartments without on-site generation, sub-metering of specific consumers (water heaters, motors, household appliances, HVAC).

### STANDARD — bidirectional

An extended analog stack for precise bidirectional measurement. The firmware maintains **two separate accumulators** per period: one for consumption (positive segments), another for export (negative). The `energy_exported` sensor on this lineup carries a real signal.

- The `bidirectional: true` key on the `rbamp:` block enables reads of the export-energy register.
- NVS persistence works identically for both accumulators.
- Typical applications: homes with solar/wind generation, battery storage, V2G EV charging, any installation where import and export must be accounted separately.

### PRO — premium

Low-noise analog front-end, tighter component tolerances. Additionally available on PRO:

- Extended diagnostics (peak demand windows, harmonic indicators, drift monitoring) — to be integrated into the YAML schema in a future component revision.
- SKUs with more channels (UI5 / UI7, I6 / I8).

Typical applications: sub-metering of commercial tenants, billing-grade installations, instrument-grade laboratories, energy-intensive industrial loads, service bureaus requiring audit-quality data.

## Capability matrix

| Capability | BASIC | STANDARD | PRO |
|---|:---:|:---:|:---:|
| RMS voltage | yes | yes | yes |
| RMS current (1..3 channels) | yes | yes | yes |
| Active power (signed float) | yes | yes | yes |
| Power factor | yes | yes | yes |
| Reactive power | yes | yes | yes |
| Apparent power | yes | yes | yes |
| Mains frequency | yes | yes | yes |
| Average power per period (import) | yes | yes | yes |
| Average power per period (export) | always 0 | yes | yes |
| `energy` sensor (import Wh, NVS) | yes | yes | yes |
| `energy_exported` sensor (NVS) | always 0 | yes | yes |
| Extended diagnostic registers | no | no | yes |
| SKUs with more channels (UI5 / UI7 / I6 / I8) | no | no | yes |

> The base register block is identical on all tiers. STANDARD- and PRO-specific registers live in a separate range above the base. The component reads only what sensors are declared for — an undeclared register is never requested.

## YAML keys per tier

### `bidirectional:` (default `false`)

Enables the read branch for the export-energy register on every `update()` cycle.

```yaml
rbamp:
  id: solar_meter
  address: 0x51
  bidirectional: true   # meaningful on STANDARD / PRO; no-op on BASIC
```

On BASIC the key is accepted by the schema without error, but `energy_exported` always yields 0. The component has no way to detect the tier at runtime (no tier register exists in current firmware) — the user sets the key for their own SKU.

> The key is reserved for upcoming firmware revisions. The current build already accepts it in the schema and reserves `energy_export_wh[3]` slots in NVS, but the actual export register read will be wired up in a future component revision together with the final register layout.

### `sensor_class:` — fixing the sensor family

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013   # default SCT_013
```

Accepted on all tiers. On firmware v1.2+ the value is written to flash and becomes a **precondition** for writing the CT model: the module will refuse to write the model if the class is not set. On earlier firmware the value is accepted by the schema and will be applied automatically on a firmware upgrade — without any YAML changes.

Enum values: `SCT_013` (the current family), `WIRED_CT`, `BUILTIN_CT` (reserved for future SKUs).

### `ct_model:` / `ct_models:` — CT clamp model

One global profile for all channels:

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_030   # all channels — SCT-013-030
```

Or an individual profile per channel (for UI3 with mixed clamps):

```yaml
rbamp:
  id: meter_ui3
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
```

Accepted on all tiers, mutually exclusive. On firmware v1.2+ the module automatically loads factory coefficients for the chosen model — no additional calibration steps required. On earlier firmware the value is stored as a model marker; full auto-loading of coefficients comes with a firmware upgrade.

More on model selection in [03_sensor_selection.md](03_sensor_selection.md).

### `new_address:` — one-shot I²C address provisioning

```yaml
rbamp:
  id: meter1
  address: 0x50
  new_address: 0x51   # one-shot; remove after a successful boot
```

Accepted on all tiers. Requires **factory provisioning mode** on the module side — this is the mode the module is placed in for one-off operations (address change, factory reset). Standard production modules ship with provisioning mode disabled — consult the module documentation or supplier for the procedure to enable it.

Execution flow within a single boot: the component polls the current address, writes the new address and saves it to flash, resets the module, verifies a response at the new address. After success, update YAML to `address: 0x51` and remove `new_address:`. If the component finds that the old address is silent while the new one responds, it adapts automatically with a warning in the log (in case a previous boot already applied the change).

### `broadcast_latch:` — synchronous latch for a multi-module bus

```yaml
rbamp:
  - id: meter1
    broadcast_latch: true   # no-op on v1 firmware; safe for forward-compat
```

When `true`, the component sends the latch command to the I²C broadcast address — synchronizing the period snapshot across all modules on the bus in a single transaction. **On v1 firmware the general-call is disabled** — a write to the broadcast address is silently ignored. The component sees the firmware version and logs an `ESP_LOGW` warning, then falls back to a sequential latch for each module individually.

Skew between modules on a 50 kHz bus is on the order of ~540 µs × N, which is less than 0.003% of a 60-second period. On daily totals in HA this irregularity is invisible.

Leave the key in YAML when upgrading from v1 to v1.1 firmware — the warning will disappear and the component will switch to the real broadcast on its own.

### `topology:` — topology hint

```yaml
rbamp:
  id: meter1
  topology: SINGLE      # SINGLE / SPLIT_PHASE / THREE_PHASE
```

Accepted on all tiers. On current firmware the component derives the channel count itself from the declared sensor slots (if `current_1` is declared — there are at least 2 channels). The key is cosmetic, it goes into the `dump_config` log line. It will become authoritative once the module publishes the topology through its own register in a future firmware revision.

### `drdy_pin:` — optional data-ready interrupt

```yaml
rbamp:
  id: meter1
  drdy_pin: GPIO15
```

Accepted on all tiers. Connects to the module's optional open-drain DRDY output. When declared, the component uses the pin as a hint that the instantaneous registers are fresh before reading them. With a typical `update_interval: 60s` the key can be omitted: the component skips instantaneous reads when the status register is not ready, independently of DRDY.

Requires a 10 kΩ pull-up on the master's GPIO to 3.3 V (the output is open-drain, with no internal pull-up on the module side). See [04_hardware.md](04_hardware.md).

## Summary table — YAML key × tier

| YAML key | BASIC | STANDARD | PRO | Notes |
|---|:---:|:---:|:---:|---|
| `address:` | yes | yes | yes | — |
| `update_interval:` | yes | yes | yes | — |
| `drdy_pin:` | yes | yes | yes | Optional, needs a 10 kΩ pull-up |
| `topology:` | yes | yes | yes | Informational hint on current firmware |
| `sensor_class:` | yes | yes | yes | Accepted by the schema; on v1.2+ — precondition for writing the CT model |
| `ct_model:` / `ct_models:` | yes | yes | yes | Mutually exclusive; coefficient auto-load on v1.2+ |
| `bidirectional:` | accepted | works | works | No-op on BASIC |
| `new_address:` | yes | yes | yes | Requires factory provisioning mode; one-shot |
| `broadcast_latch:` | accepted | accepted | accepted | No-op on v1 firmware |
| `energy_exported` sensor | always 0 | works | works | Requires STANDARD / PRO hardware |
| Phased sensors (`voltage_a/b/c`, `current_a/b/c`, …) | accepted by schema | accepted by schema | accepted by schema | Reserved for upcoming rbAmp-U2I2 (split-phase) and rbAmp-U3I3 (three-phase) SKUs — current firmware does not publish values. See [09 §6](09_api_reference.md). |

## What next

- [03_sensor_selection.md](03_sensor_selection.md) — choosing a CT clamp and behavior of `ct_model:` / `ct_models:`
- [04_hardware.md](04_hardware.md) — physical wiring, GPIO selection



---

← [Overview](01_overview.md) · [Docs index](README.md) · [Sensor Selection](03_sensor_selection.md) →
