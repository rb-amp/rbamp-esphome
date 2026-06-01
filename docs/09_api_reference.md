# 09 · YAML schema reference

This chapter is the complete YAML schema reference for the `rbamp` ESPHome external component. If a key isn't here, it isn't in the schema.

Contents:

1. [Component block — `rbamp:`](#1-component-block--rbamp)
2. [Sensor platform — `sensor.platform: rbamp`](#2-sensor-platform--sensorplatform-rbamp)
    - [Single-phase fields](#21-single-phase-fields)
    - [Phased fields (future SKUs)](#22-phased-fields-future-skus)
    - [Shared fields](#23-shared-fields-topology-independent)
    - [Validation rules](#24-schema-validation-rules)
3. [Data flow and timings](#3-data-flow-and-timings)
4. [I²C bus settings](#4-ic-bus-settings)

---

## 1. Component block — `rbamp:`

The top-level `rbamp:` block registers a component instance. It inherits from `PollingComponent` (which provides `update_interval`) and `i2c.I2CDevice` (which provides `address` and the I²C-bus handle). `MULTI_CONF: True` — any number of `rbamp:` blocks can coexist in a single YAML, each referring to its own I²C slave.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  drdy_pin: GPIO4
  sensor_class: SCT_013
  ct_model: SCT_013_030
  # or for UI3 with mixed CT clamps:
  # ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  bidirectional: false
  new_address: 0x51
  broadcast_latch: false
  topology: SINGLE
```

---

### `id`

| Attribute | Value |
|---|---|
| Type | `id` (ESPHome identifier) |
| Default | Auto-generated |
| Required | No |

Assigns a stable identifier to the instance. Sensors reference it via `rbamp_id: meter1`. With multiple `rbamp:` blocks, an explicit `id` is required.

```yaml
rbamp:
  id: kitchen_meter
```

---

### `address`

| Attribute | Value |
|---|---|
| Type | `i2c_address` (7-bit, 0x08..0x77) |
| Default | `0x50` |
| Required | No |

The current I²C address of the rbAmp module. Factory default is `0x50`. After applying an address change (via `new_address`) — update this key to the new value and remove `new_address`.

Permitted range: `0x08..0x77` (reserved 7-bit I²C addresses are excluded).

```yaml
rbamp:
  address: 0x52   # two modules on the bus: 0x50, 0x51, 0x52
```

Cross-reference: [SPEC.md §2](https://rbamp.com/docs/modules-basic-standard-api-reference) for the bus protocol; [§10](https://rbamp.com/docs/modules-basic-standard-api-reference) for address change.

---

### `update_interval`

| Attribute | Value |
|---|---|
| Type | `time` (ESPHome duration string) |
| Default | `60s` |
| Required | No |

How often `update()` is invoked. Each call:

1. Sends the latch command and schedules a non-blocking 50 ms timeout to read the period snapshot.
2. Checks the status register; if the module is ready — publishes all bound instantaneous sensors.

The 60 s default is reasonable: the module internally refreshes the average power per ~200 ms period, but energy integration on the master side needs a long enough window for correctness. At 60 s and an average 60 W load, a missed latch loses ≤ 1 Wh.

The minimum practical value is `10s`. Values < `1s` give no benefit and clog the bus.

```yaml
rbamp:
  update_interval: 30s   # more frequent dashboard updates, more traffic
```

---

### `drdy_pin`

| Attribute | Value |
|---|---|
| Type | `gpio_input_pin_schema` |
| Default | None (optional) |
| Required | No |

Connects the module's open-drain DRDY output to an ESP32 GPIO. When specified, the pin is configured in `setup()` at boot.

> On the current firmware, the pin is logged in `dump_config` but is not used as a read trigger — instantaneous registers are polled on `update_interval`. Declaring the pin does not change behavior, but reserves it for future firmware revisions with interrupt-driven reads.

```yaml
rbamp:
  drdy_pin: GPIO4
```

---

### `sensor_class`

| Attribute | Value |
|---|---|
| Type | `enum` (`SCT_013`, `WIRED_CT`, `BUILTIN_CT`) |
| Default | `SCT_013` |
| Required | No |

Pins the current sensor family on the module side. On firmware v1.2+, the value is written to flash and becomes a **precondition** for writing the CT model: the module will refuse to write a model if the class is not set. On earlier firmware, the value is accepted by the schema and is applied automatically on upgrade.

| Value | Status |
|---|---|
| `SCT_013` | Available now, default |
| `WIRED_CT` | Reserved for future SKUs |
| `BUILTIN_CT` | Reserved for future SKUs |

```yaml
rbamp:
  sensor_class: SCT_013   # default; can be omitted
```

More on CT-clamp and family selection in [03_sensor_selection.md](03_sensor_selection.md).

---

### `ct_model`

| Attribute | Value |
|---|---|
| Type | `enum` (`SCT_013_005`, `_010`, `_030`, `_050`, `_100`) |
| Default | None (optional) |
| Required | No |

Writes the CT-clamp model identifier to the module's flash. On firmware v1.2+, it automatically loads the factory coefficients for the chosen model — no additional calibration steps are required.

| YAML value | Nominal current |
|---|---|
| `SCT_013_005` | 5 A |
| `SCT_013_010` | 10 A |
| `SCT_013_030` | 30 A |
| `SCT_013_050` | 50 A |
| `SCT_013_100` | 100 A |

```yaml
rbamp:
  ct_model: SCT_013_030
```

Applied once in `setup()`. Each write is accompanied by a ~700 ms flash write (the module NACKs all I²C operations during this time — the component feeds the watchdog automatically).

**Mutually exclusive** with `ct_models:` — only one of the two can be used per `rbamp:` block.

More in [03_sensor_selection.md](03_sensor_selection.md). Cross-reference: [SPEC.md §11](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `ct_models`

| Attribute | Value |
|---|---|
| Type | list of **1–3** enum values |
| Default | None (optional) |
| Required | No |

Per-channel CT-clamp models — for UI2/UI3 with mixed clamps on different channels. Accepts the same enum values as `ct_model:`. The schema validates array length via `cv.Length(min=1, max=3)`; the element count must match the number of physical channels on the module (1 for UI1, 2 for UI2, 3 for UI3).

```yaml
rbamp:
  id: ui3_meter
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  # CH0=5A for standby loads (max resolution),
  # CH1=30A for main household loads,
  # CH2=100A for the main feed / EV charging
```

On firmware v1.2+, each channel gets its own set of factory coefficients independently. Total setup time at boot is ~2–3 seconds (~700 ms flash write per channel).

**Mutually exclusive** with `ct_model:`.

---

### `bidirectional`

| Attribute | Value |
|---|---|
| Type | `bool` |
| Default | `false` |
| Required | No |

Enables the export-energy register read branch for STANDARD / PRO tiers. When `true`, the component attempts to read per-channel export-power registers on each period snapshot and accumulate export Wh separately.

**Status on current firmware**: the key is accepted by the schema and reserves `energy_export_wh[]` slots in NVS, but the export-energy register is not yet wired up in firmware. `energy_exported_*` sensors publish 0 until firmware with this register ships.

Declare `energy_exported` (or `_1` / `_2`) under `sensor.platform: rbamp` only when `bidirectional: true` is set.

```yaml
rbamp:
  bidirectional: true   # makes sense on STANDARD / PRO
```

---

### `new_address`

| Attribute | Value |
|---|---|
| Type | `i2c_address` (0x08..0x77) |
| Default | None (optional) |
| Required | No |

Triggers a one-shot I²C address change at boot. Must differ from `address` — the validator raises an error if they match.

The full flow runs once in `setup()`:

1. Probe the current `address`. If the module does not answer there but answers at `new_address` — the component adapts to the new address with a warning (assuming a previous boot already applied the change) and skips the write.
2. Check that the module is ready for the provisioning operation. If the module is not in provisioning mode — the change is skipped with a warning.
3. Write the new address to flash (~700 ms), soft-reset the module (~300 ms), re-lock on the AC cycle.
4. The component switches its internal I²C address to `new_address` and verifies that the module responds.
5. If the module does not respond after the change — the component calls `mark_failed()` and stops.

**After a successful change**: update the YAML to `address: <new>` and remove `new_address:`. If the module is at the new address on the next boot and `new_address:` is still in the YAML — the boot-time probe of the old address will fail, the probe of the new address will succeed, the component adapts with a warning, and re-writing does not happen.

> **WARNING — `new_address:` requires factory-provisioning mode**
>
> Standard production modules ship with provisioning mode disabled. Writing `new_address:` on such a module will be rejected (warning in the ESPHome log at boot), and the address will not change. If you need a field address change — consult the module documentation or vendor for a procedure to temporarily enable provisioning mode.

```yaml
rbamp:
  address: 0x50
  new_address: 0x51   # remove this line after the first successful boot
```

**Recovery**: if the address change applied but the module does not respond — see [10_troubleshooting.md](10_troubleshooting.md) → "Address changed but module not responding".

Cross-reference: [SPEC.md §10](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `broadcast_latch`

| Attribute | Value |
|---|---|
| Type | `bool` |
| Default | `false` |
| Required | No |

Enables I²C General-Call broadcast for the latch command, synchronizing multiple rbAmp modules on the same bus with a single write to the broadcast address.

**Limitation on current firmware**: I²C general-call is disabled on the module side — writes to the broadcast address are silently dropped. The component sees the firmware version and logs a warning:

```
broadcast_latch: true requested but firmware v0x01 has I2C general-call
DISABLED — broadcasts will be dropped by the slave.
```

When `broadcast_latch: true` is set and the warning fires — the component falls back to a sequential latch for each module. Skew between modules at 50 kHz bus is ~ `270 µs × N`; for three modules on a 60 s cadence that's < 0.0015% relative error — invisible in daily HA totals.

Leave the key in the YAML when upgrading firmware — the warning will disappear automatically when firmware adds broadcast support.

```yaml
rbamp:
  broadcast_latch: false   # set true once firmware adds broadcast
```

Cross-reference: [SPEC.md §9](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `topology`

| Attribute | Value |
|---|---|
| Type | `enum` (`SINGLE`, `SPLIT_PHASE`, `THREE_PHASE`) |
| Default | `SINGLE` |
| Required | No |

Declares the physical configuration of the module.

| Value | Current firmware | When authoritative |
|---|---|---|
| `SINGLE` | Cosmetic (logged in `dump_config`). Channel count is derived from the declared `current[_1/_2]` slots. | Already matches every current SKU; will be confirmed from an in-band register after its release in firmware. |
| `SPLIT_PHASE` | Accepted by the schema, written into `dump_config`. Phased keys (`voltage_a/b/c`, `current_a/b/c`, …) in `sensor.platform: rbamp` may be declared — the component will then reserve the slots, but there is nothing to publish into them yet. | After release of the rbAmp-U2I2 SKU with an in-band topology register. |
| `THREE_PHASE` | Same as `SPLIT_PHASE`. | After release of the rbAmp-U3I3 SKU with an in-band topology register. |

On the current firmware there is no in-band topology register (reserved for future revisions). The hint is informational: it goes into a `dump_config` line, but the actual channel count is derived independently from the declared `current[_1/_2]` sensor slots.

```yaml
rbamp:
  topology: SINGLE         # UI1, UI2, UI3, I1, I2, I3 — current SKUs
  # topology: SPLIT_PHASE  # US split-phase (U2I2) — future SKU
  # topology: THREE_PHASE  # European 3-phase (U3I3) — future SKU
```

Once the module begins publishing topology via its register — the component will prefer the module's value and use the YAML hint only as a fallback. The `SINGLE` default matches every current SKU — no changes are required to deployed configs.

Cross-reference: [SPEC.md §8](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 2. Sensor platform — `sensor.platform: rbamp`

Each `sensor:` block declares one set of named sensors bound to a parent `rbamp:` component. The only required key in the block is `rbamp_id`.

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"
    frequency:
      name: "Mains Frequency"
    power_factor:
      name: "Mains Power Factor"
```

All fields under the `sensor:` block are optional. Declare only the quantities your scenario needs — the component reads only the registers corresponding to the declared sensors.

### `rbamp_id`

| Attribute | Value |
|---|---|
| Type | `use_id(RbAmpComponent)` |
| Required | **Yes** |

References the `rbamp:` block this sensor group is bound to.

---

### 2.1 Single-phase fields

Used for current SKUs (UI1, UI2, UI3, I1, I2, I3). All fields are optional. Mixing with phased fields (`voltage_a` etc.) raises a validation error.

Each field accepts the standard ESPHome `sensor.sensor_schema` sub-keys: `name`, `id`, `filters`, `unit_of_measurement`, `accuracy_decimals`, `icon`, etc.

#### `voltage`

| Attribute | Value |
|---|---|
| Unit | V |
| `device_class` | `voltage` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Source | Mains RMS voltage (module instantaneous register) |

Mains RMS voltage. Read every `update()` when the module is ready. The 4-byte float is read with per-byte retry (3 attempts × 5 ms) and passes a `std::isfinite()` + `|val| < 10000` check.

Zero is a valid value to publish: a mains drop-out event or brownout gives U ≈ 0 V and passes through to HA unfiltered (see [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)).

```yaml
voltage:
  name: "Mains Voltage"
  filters:
    - sliding_window_moving_average:
        window_size: 3
        send_every: 1
```

#### `current` / `current_1` / `current_2`

| Attribute | Value |
|---|---|
| Unit | A |
| `device_class` | `current` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 3 |

RMS current for channels 0, 1, 2 respectively. Channel 0 is the primary CT clamp; channels 1 and 2 are present on UI2 / UI3 and I2 / I3 SKUs.

The active channel count is derived from the number of declared slots: if only `current` is declared — `n_channels_ = 1`; if `current` and `current_1` — `2`; adding `current_2` gives `3`.

```yaml
current:
  name: "Phase Current"
current_1:
  name: "Load 1 Current"
current_2:
  name: "Load 2 Current"
```

#### `power` / `power_1` / `power_2`

| Attribute | Value |
|---|---|
| Unit | W |
| `device_class` | `power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Dependencies | `power` requires `current` + `voltage`; `power_1` requires `current_1` + `voltage`; `power_2` requires `current_2` + `voltage` |

Active power in watts, **signed**. Negative values = reverse flow (generation into the grid) on STANDARD / PRO tiers. On BASIC, negative instantaneous values inside the period window are clamped to 0 at the firmware level — the period average is `≥ 0`. Instantaneous active power can still read negative at the moment of generation.

Declaring `power` without `current` + `voltage` raises a validation error:
`power requires current to also be declared`.

```yaml
power:
  name: "Active Power"
```

#### `energy` / `energy_1` / `energy_2`

| Attribute | Value |
|---|---|
| Unit | Wh |
| `device_class` | `energy` |
| `state_class` | `total_increasing` |
| `accuracy_decimals` | 3 |
| Source | Master-side accumulator (not a module register) |
| Dependencies | Requires `current` + `voltage` |

Accumulated consumed energy in Wh. Computed entirely on the ESP32 by the formula:

```
E_Wh[ch] += avg_P_W[ch] * master_dt_s / 3600
```

where `avg_P_W[ch]` is the period-average power read from the module after each latch command, and `master_dt_s` is the wall-clock interval on the ESP32 between latches.

Values are saved to NVS every 5 minutes and restored before the first `publish_state` at boot — this prevents the HA Energy dashboard from interpreting an instantaneous 0 as a counter reset. Worst-case loss on sudden power failure is up to 5 minutes of energy (≈ 5 Wh at average 60 W).

`state_class: total_increasing` is required for the HA Energy dashboard. The value rises monotonically; it does not decrease under normal operation. When the NVS layout changes (version bump), the counter starts from 0.

```yaml
energy:
  name: "Mains Energy"
```

#### `energy_exported` / `energy_exported_1` / `energy_exported_2`

| Attribute | Value |
|---|---|
| Unit | Wh |
| `device_class` | `energy` |
| `state_class` | `total_increasing` |
| `accuracy_decimals` | 3 |
| Source | Master-side export accumulator |
| Dependencies | Requires `current` + `voltage` + `bidirectional: true` |

Exported (generated) energy in Wh, accumulated separately from `energy`. Wired into the component, but publishes 0 until firmware adds the corresponding period-negative-power register. Set `bidirectional: true` in the `rbamp:` block when declaring these sensors.

#### `power_factor` / `power_factor_1` / `power_factor_2`

| Attribute | Value |
|---|---|
| Unit | (dimensionless) |
| `device_class` | `power_factor` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 3 |
| Dependencies | Requires `current` + `voltage` for the corresponding slot |

Power factor in the range −1..+1. A negative PF is a leading or lagging load — the sign convention is defined by the firmware. The sanity filter (§B.5) discards values outside `|pf| > 10000`; there is no lower bound.

```yaml
power_factor:
  name: "Power Factor"
```

#### `reactive_power` / `reactive_power_1` / `reactive_power_2`

| Attribute | Value |
|---|---|
| Unit | VAr |
| `device_class` | `reactive_power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Dependencies | Requires `current` + `voltage` for the corresponding slot |

Reactive power in VAr. Signed. Published from the instantaneous-register block on each `update()` cycle.

```yaml
reactive_power:
  name: "Reactive Power"
```

> **Note on `device_class: reactive_power`** — ESPHome accepts it
> (the `DEVICE_CLASS_REACTIVE_POWER` constant), and the value reaches
> Home Assistant. However, the official Home Assistant `device_class`
> list has changed over time: in some versions `reactive_power` is
> available in the UI and works with the `VAr` unit_of_measurement;
> in others, it renders as a generic sensor without a specialized icon
> or unit conversion. The data itself is always published (it's just a
> `state`); the only question is how HA renders the sensor in Lovelace.
> If your HA version shows the sensor as unknown — that's cosmetic,
> not a functional regression. Remove the `reactive_power:` key from
> the YAML if you don't use the sensor.

#### `apparent_power`

| Attribute | Value |
|---|---|
| Unit | VA |
| `device_class` | `apparent_power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Source | Computed on the master: `S = V_rms × I_rms[0]` |
| Dependencies | Requires `current` + `voltage` |

Apparent power in VA. Computed by the component from the `U_rms` and `I0_rms` values read in the same `update()` cycle — no separate register is read. Both reads must succeed in the same cycle; if at least one fails (retries exhausted), the state is not published in this cycle and the last value in HA is preserved.

`apparent_power` lives in `SHARED_FIELDS` and works with any topology, but depends on `voltage` and `current` for the V × I calculation.

```yaml
apparent_power:
  name: "Apparent Power"
```

---

### 2.2 Phased fields (future SKUs)

Reserved for split-phase (U2I2, US market) and three-phase (U3I3) future SKUs. The schema accepts them now — a user can prepare the configuration in advance. On current firmware, declaring phased slots reads the corresponding register addresses (which return `0.0` for unimplemented channels) and publishes 0 — not an error, but not useful data either.

Single-phase and phased fields are **mutually exclusive** within one `sensor:` block. The validator raises:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...).
```

All phased fields share the same `sensor_schema` defaults as their single-phase counterparts (same units, `device_class`, `state_class`, `accuracy_decimals`).

| Field | Unit | `device_class` |
|---|---|---|
| `voltage_a` / `_b` / `_c` | V | voltage |
| `current_a` / `_b` / `_c` | A | current |
| `power_a` / `_b` / `_c` | W | power |
| `power_total` | W | power |
| `energy_a` / `_b` / `_c` | Wh | energy |
| `energy_exported_a` / `_b` / `_c` | Wh | energy |
| `power_factor_a` / `_b` / `_c` | — | power_factor |
| `reactive_power_a` / `_b` / `_c` | VAr | reactive_power |

> Per-phase dependency validation is not enforced for phased fields (only `_SINGLE_SLOT_DEPS` for single-phase). Firmware support for split / three-phase variants is reserved for future SKUs.

Example for a future three-phase deployment:

```yaml
rbamp:
  id: panel_meter
  address: 0x50
  topology: THREE_PHASE

sensor:
  - platform: rbamp
    rbamp_id: panel_meter
    voltage_a:
      name: "Phase A Voltage"
    voltage_b:
      name: "Phase B Voltage"
    voltage_c:
      name: "Phase C Voltage"
    current_a:
      name: "Phase A Current"
    current_b:
      name: "Phase B Current"
    current_c:
      name: "Phase C Current"
    power_total:
      name: "Total Active Power"
```

---

### 2.3 Shared fields (topology-independent)

Two fields work in any topology group, including the case when no single-phase or phased current sensor is declared (voltage-only or frequency-only deployment).

#### `frequency`

| Attribute | Value |
|---|---|
| Unit | Hz |
| `device_class` | `frequency` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 0 |
| Type | `uint8_t` (not float) |

Mains frequency, read as a single byte. The component publishes the value only when it equals 50 or 60 — other values (`0` = ZC not caught, implausibly large) are dropped without publishing. This avoids the "Unknown" → 0 Hz → 50 Hz transition on the HA entity during warm-up.

```yaml
frequency:
  name: "Mains Frequency"
```

#### `apparent_power` (shared)

See the identical entry under single-phase fields above. `apparent_power` is part of `SHARED_FIELDS` and may appear in either a single-phase or a phased sensor block, provided `voltage` and `current` (or `current_a`) are also declared.

---

### 2.4 Schema validation rules

The Python validator `_validate_topology_consistency` applies two classes of rules during `esphome config` / compile time — before any C++ code runs.

#### Topology mutual exclusion

Single-phase fields (`voltage`, `current`, `current_1`, etc.) and phased fields (`voltage_a`, `voltage_b`, etc.) cannot coexist in the same `sensor:` block. Attempt to mix:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...). Pick one group based
on your rbAmp SKU.
```

#### Per-slot companion requirements (single-phase only)

Each derived field requires the input fields the module needs for the calculation:

| Declared field | Required companions |
|---|---|
| `power` / `_1` / `_2` | `current` (or `_1` / `_2`) + `voltage` |
| `energy` / `_1` / `_2` | `current` (or `_1` / `_2`) + `voltage` |
| `power_factor` / `_1` / `_2` | `current` (or `_1` / `_2`) + `voltage` |
| `reactive_power` / `_1` / `_2` | `current` (or `_1` / `_2`) + `voltage` |
| `apparent_power` | `current` + `voltage` |

A missing companion raises:

```
`power` requires `current` to also be declared — the underlying chip
cannot compute it without that input.
```

#### Other rules at the `rbamp:` block level

| Rule | Description |
|---|---|
| `ct_model:` ↔ `ct_models:` mutually exclusive | Only one of the two can be used per `rbamp:` block — otherwise validation error. |
| `new_address` ≠ `address` | If they match — `cv.Invalid`. |
| `address` in range `0x08..0x77` | Enforced by `cv.i2c_address`. |

All validation errors are reported during `esphome compile` with a human-readable message pointing to the offending key. No need to flash hardware to find a configuration error.

---

## 3. Data flow and timings

Conceptual flow of one `update()` cycle:

```
Module (autonomous)                 ESP32 (ESPHome component)
─────────────────────────────────   ────────────────────────────────────────────
Internal ADC sampling and
RMS / P / PF / Q computation
  ↓ (~200 ms per cycle)
Atomically publishes the            update() fires every update_interval (60 s)
instantaneous register block          ↓
  ↓                                 phase 1 — latch:
                                      write latch command
Closes the period accumulator,        50 ms timeout (non-blocking)
opens a new one                       ↓ (main loop keeps running)
  ↓
  wait 50 ms                        phase 2 — period snapshot:
                                      read valid flag
                                      read per-channel average powers
                                      E_Wh[ch] += avg_P × dt_s / 3600
                                      save to NVS if 5 minutes have passed
                                      ↓
                                    phase 3 — instantaneous values:
                                      read status register
                                      read U_rms, I[0..n]_rms, P[0..n],
                                            PF[0..n], Q[0..n], frequency
                                      publish all bound sensors
                                      ↓
                                    Pass state to HA over native API
```

**Characteristic timings**:

| Event | Cadence |
|---|---|
| Module's internal commit of the instantaneous block | ~200 ms |
| `update()` call | `update_interval` (default 60 s) |
| Settle timeout after latch command | 50 ms (non-blocking) |
| Save to NVS | Every 5 minutes |
| Pause between per-byte retries | 5 ms, up to 3 attempts |
| Flash-write window when writing the model | ~700 ms (the module NACKs everything during this time) |

The latch-settle timeout (`set_timeout("rbamp_period", 50, ...)`) is **non-blocking** — the cooperative ESPHome scheduler keeps processing WiFi, API and other components during the 50 ms wait. The remaining `update()` phases (reading the instantaneous registers) run immediately after returning from the latch phase.

The warning `[W][component:522]: rbamp took a long time for an operation (XXX ms)` on every cycle is **expected and harmless**. It reflects the wire time for up to 36 I²C transactions (4 bytes for each of 9 float registers) at 50 kHz. See [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 4. I²C bus settings

The `rbamp` component inherits the I²C bus configured in the top-level `i2c:` block. Recommended settings on current firmware:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz   # 100 kHz causes periodic NACKs; 50 kHz reduces them ~5x
  scan: true         # optional: logs discovered addresses at boot
```

**Bus speed**: use `50kHz` on current firmware. The module's I²C peripheral periodically NACKs at 100 kHz (~20% of transactions) due to a known behavior in the ESP-IDF `i2c_master` driver (confirmed in [esp-idf issue #9426](https://github.com/espressif/esp-idf/issues/9426), marked "Won't Do" by Espressif). The component's three-layer mitigation (retry + sanity + 50 kHz) brings the effective bad-read rate below 1%. Once a firmware fix ships — the speed can be raised back to 100 kHz with a one-line YAML change (see [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)).

**Pull-up resistors**: 4.7 kΩ to 3.3 V on SDA and SCL recommended. Larger values (10 kΩ) increase rise time and raise the NACK rate at 100 kHz.

**Multiple devices**: the ESP32 I²C bus supports up to 112 devices on different 7-bit addresses. Use `address:` and `new_address:` to assign unique addresses to each module. `scan: true` in the `i2c:` block logs every responding address at boot — verify that each module is discovered before enabling period metering.

> The full wire-level register map and module command description are in [SPEC.md](https://rbamp.com/docs/modules-basic-standard-api-reference). For a typical YAML-schema user this chapter is sufficient — all registers are hidden behind the declarative interface of the component.


---

← [Home Assistant](08_has_integrations.md) · [Docs index](README.md) · [Troubleshooting](10_troubleshooting.md) →
