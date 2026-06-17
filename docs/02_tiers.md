# 02 · Module Tiers

![rbAmp product tiers: BASIC / STANDARD / PRO capability ladder](images/tier-ladder.png)

rbAmp ships in three product lines: **BASIC**, **STANDARD**, and **PRO**. Each tier is a complete combination of hardware revision and firmware build, not just a flag in the firmware. Moving between tiers requires physically swapping the module, not a firmware upgrade.

The ESPHome component works with all three tiers identically: the YAML schema is shared, and the only differences are what data the module actually exports and which YAML keys are meaningful on each tier.

## Tiers

### BASIC — entry level

The analog front-end is cost-optimized and suited to typical residential loads. The firmware keeps **unidirectional consumption metering**: the average active power over a period can never be negative — reverse segments within the window are zeroed before integration. Behaviorally this resembles a classic mechanical disc meter.

- The `energy_exported` sensor exists in the YAML schema (for config compatibility), but on BASIC it always publishes `0`.
- Typical applications: apartments without on-site generation, sub-metering of specific loads (water heaters, motors, household appliances, HVAC).

### STANDARD — bidirectional

An extended analog stage for accurate bidirectional measurement. The firmware keeps **two separate accumulators** per period: one for consumption (positive segments) and one for export (negative segments). On this line the `energy_exported` sensor carries a real signal.

- The `bidirectional: true` key on the `rbamp:` block enables reading the export-energy register.
- NVS persistence works the same way for both accumulators.
- Typical applications: homes with solar/wind generation, battery storage, V2G EV chargers, any installation where import and export must be metered separately.

### PRO — premium

A low-noise analog front-end with tighter component tolerances. PRO additionally offers:

- Extended diagnostics (peak demand windows, harmonic indicators, drift monitoring) — to be integrated into the YAML schema in an upcoming component revision.
- Higher channel-count SKUs (UI5 / UI7, I6 / I8).

Typical applications: sub-metering of commercial tenants, billing-grade installations, instrumentation labs, energy-intensive industrial loads, service bureaus that require audit-quality data.

## Capability Matrix

| Capability | BASIC | STANDARD | PRO |
|---|:---:|:---:|:---:|
| RMS voltage | yes | yes | yes |
| RMS current (1..3 channels) | yes | yes | yes |
| Active power (signed float) | yes | yes | yes |
| Power factor | yes | yes | yes |
| Reactive power | yes | yes | yes |
| Apparent power | yes | yes | yes |
| Line frequency | yes | yes | yes |
| Average power per period (import) | yes | yes | yes |
| Average power per period (export) | always 0 | yes | yes |
| `energy` sensor (import Wh, NVS) | yes | yes | yes |
| `energy_exported` sensor (NVS) | always 0 | yes | yes |
| Extended diagnostic registers | no | no | yes |
| Higher channel-count SKUs (UI5 / UI7 / I6 / I8) | no | no | yes |

> The base register block is identical across all tiers. STANDARD- and PRO-specific registers live in a separate range above the base block. The component reads only what sensors are declared for — an undeclared register is never requested.

## YAML Keys by Tier

### `bidirectional:` (default `false`)

Enables the export-energy register read branch on every `update()` cycle.

```yaml
rbamp:
  id: solar_meter
  address: 0x51
  bidirectional: true   # meaningful on STANDARD / PRO; a no-op on BASIC
```

On BASIC the key is accepted by the schema without error, but `energy_exported` always returns 0. The component has no way to determine the tier at runtime (the current firmware provides no tier register) — you set the key yourself to match your SKU.

> The key is reserved for upcoming firmware revisions. On the current build the component already accepts it in the schema and reserves `energy_export_wh[3]` slots in NVS, but the actual export-register read will be wired up in an upcoming component revision together with the final register layout.

### `sensor_class:` — pin the sensor family

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013   # default SCT_013
```

Accepted on all tiers. On firmware v1.2+ the value is written to flash and becomes a **precondition** for writing the CT model: the module refuses to write the model if the class is not set. On earlier firmware the value is accepted by the schema and is applied automatically on a firmware upgrade — with no need to change the YAML.

Enum values: `SCT_013` (current family), `WIRED_CT`, `BUILTIN_CT` (reserved for future SKUs).

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

Accepted on all tiers, mutually exclusive. On firmware v1.2+ the module automatically loads the factory coefficients for the selected model — no additional calibration steps are required. On earlier firmware the value is stored as a model marker; full coefficient auto-load arrives with a firmware upgrade.

For more on selecting a model, see [03_sensor_selection.md](03_sensor_selection.md).

### `new_address:` — one-time I²C address provisioning

```yaml
rbamp:
  id: meter1
  address: 0x50
  new_address: 0x51   # one-shot; remove after a successful boot
```

Accepted on all tiers. Requires **factory-provisioning mode** on the module side — this is the mode the module is put into for one-time operations (address change, factory reset). Standard production modules ship with provisioning mode disabled — consult the module documentation or your supplier for the procedure to enable it.

Execution flow over a single boot: the component polls the current address, writes the new address and saves it to flash, resets the module, and verifies the response at the new address. After success, update the YAML to `address: 0x51` and remove `new_address:`. If the component finds that the old address is silent while the new one responds, it adapts automatically with a warning in the log (in case a previous boot already applied the change).

### `fleet_gc_enable:` / `broadcast_latch:` — synchronous latch for a multi-module bus

```yaml
rbamp:
  - id: meter1
    fleet_gc_enable: true   # v1.3 production path — recommended
    group_id: 1             # cluster id (modules in the same group latch together)
```

When enabled, the component sends a 5-byte general-call frame to the I²C broadcast address — synchronizing the period snapshot across all modules in the same `group_id` in a single transaction.

On v1.3 firmware (REG_VERSION = 0x04) the general-call latch is **opt-in** via the `FLEET_CONFIG.bit0` register and gated by the `CAP_GC_LATCH` capability bit (bit1 of `REG_CAPABILITY`). The component reads the capability bitmap at boot and enables the path automatically when both the YAML key and the firmware capability agree. On legacy firmware (v1.0..v1.2, REG_VERSION < 0x04) the capability bit is not set; the component falls back to a sequential latch for each module individually and logs an informational `ESP_LOGI` line.

The legacy `broadcast_latch: true` key is still accepted as a deprecated alias for `fleet_gc_enable: true` (without an explicit `group_id`, the component assumes group 0). New deployments should use `fleet_gc_enable:` + `group_id:`.

The skew between modules on a 50 kHz bus is on the order of ~540 µs × N, which is less than 0.003% of a 60-second period. On daily HA totals this unevenness is imperceptible.

### `topology:` — topology hint

```yaml
rbamp:
  id: meter1
  topology: SINGLE      # SINGLE / SPLIT_PHASE / THREE_PHASE
```

Accepted on all tiers. On the current firmware the component infers the channel count from the declared sensor slots (if `current_1` is declared, there are at least 2 channels). The key is cosmetic and goes into the `dump_config` log line. It becomes authoritative once the module begins publishing its topology through a register in an upcoming firmware revision.

### `drdy_pin:` — optional data-ready interrupt

```yaml
rbamp:
  id: meter1
  drdy_pin: GPIO15
```

Accepted on all tiers. Connects to the module's optional open-drain DRDY output. When declared, the component uses the pin as a hint that the instantaneous registers are fresh before reading them. With a typical `update_interval: 60s` the key can be omitted: the component skips instantaneous reads if the status register is not ready, regardless of DRDY.

Requires a 10 kΩ pull-up on the master GPIO to 3.3 V (the output is open-drain; there is no internal pull-up on the module side). See [04_hardware.md](04_hardware.md).

## Summary Table — YAML Key × Tier

| YAML key | BASIC | STANDARD | PRO | Notes |
|---|:---:|:---:|:---:|---|
| `address:` | yes | yes | yes | — |
| `update_interval:` | yes | yes | yes | — |
| `drdy_pin:` | yes | yes | yes | Optional, needs a 10 kΩ pull-up |
| `topology:` | yes | yes | yes | Informational hint on the current firmware |
| `sensor_class:` | yes | yes | yes | Accepted by the schema; on v1.2+ a precondition for writing the CT model |
| `ct_model:` / `ct_models:` | yes | yes | yes | Mutually exclusive; coefficient auto-load on v1.2+ |
| `bidirectional:` | accepted | works | works | A no-op on BASIC |
| `new_address:` | yes | yes | yes | Requires factory-provisioning mode; one-shot |
| `fleet_gc_enable:` / `broadcast_latch:` | yes | yes | yes | v1.3 opt-in via `FLEET_CONFIG.bit0` + `CAP_GC_LATCH`; legacy fw falls back to sequential latch |
| `energy_exported` sensor | always 0 | works | works | Requires STANDARD / PRO hardware |
| Phased sensors (`voltage_a/b/c`, `current_a/b/c`, …) | accepted by schema | accepted by schema | accepted by schema | Reserved for the upcoming rbAmp-U2I2 (split-phase) and rbAmp-U3I3 (three-phase) SKUs — the current firmware does not publish the values. Details in [09 §6](09_api_reference.md). |

## What's Next

- [03_sensor_selection.md](03_sensor_selection.md) — choosing a CT clamp and the behavior of `ct_model:` / `ct_models:`
- [04_hardware.md](04_hardware.md) — physical wiring, GPIO selection

