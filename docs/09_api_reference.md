# 09 · YAML Schema Reference

![rbAmp register map — page-0 address space (0x00..0xFF)](images/api-register-map.png)

![Atomic period latch — one write closes the period and opens the next](images/period-atomic-latch.png)

This chapter is the complete reference for the YAML schema of the `rbamp` external component for ESPHome. If a key is not listed here, it does not exist in the schema.

Contents:

1. [Component block — `rbamp:`](#1-component-block--rbamp)
2. [Sensor platform — `sensor.platform: rbamp`](#2-sensor-platform--sensorplatform-rbamp)
    - [Single fields (single-phase)](#21-single-fields)
    - [Phased fields (future SKUs)](#22-phased-fields-future-skus)
    - [Shared fields](#23-shared-fields-topology-independent)
    - [Validation rules](#24-schema-validation-rules)
3. [Data flow and timing](#3-data-flow-and-timing)
4. [I²C bus settings](#4-ic-bus-settings)

---

## 1. Component block — `rbamp:`

The top-level `rbamp:` block registers a component instance. It inherits from `PollingComponent` (which provides `update_interval`) and `i2c.I2CDevice` (which provides `address` and the I²C bus handle). `MULTI_CONF: True` means any number of `rbamp:` blocks can coexist in one YAML, each referencing its own I²C slave.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  drdy_pin: GPIO4
  sensor_class: SCT_013
  ct_model: SCT_013_030
  # or, for a UI3 with mixed clamps:
  # ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
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

The current I²C address of the rbAmp module. The factory default is `0x50`. After applying an address change (via `new_address`), update this key to the new value and remove `new_address`.

Valid range: `0x08..0x77` (the reserved 7-bit I²C addresses are excluded).

```yaml
rbamp:
  address: 0x52   # three modules on the bus: 0x50, 0x51, 0x52
```

Cross-reference: [the I²C bus protocol](https://www.rbamp.com/docs/modules-basic-standard-api-reference) in the API reference; [runtime address change](https://www.rbamp.com/docs/modules-basic-standard-api-reference) for changing the address after boot.

---

### `update_interval`

| Attribute | Value |
|---|---|
| Type | `time` (ESPHome duration string) |
| Default | `60s` |
| Required | No |

How often `update()` runs. Each call:

1. Sends the latch command and schedules a non-blocking 50 ms timeout for reading the period snapshot.
2. Checks the status register; if the module is ready, publishes all bound instantaneous sensors.

The 60 s default is reasonable: the module internally updates its period-average power roughly every ~200 ms, but integrating energy on the master side requires a long enough window to be correct. At 60 s and an average load of 60 W, a missed latch loses ≤ 1 Wh.

The minimum allowed value in v1.3 is **`1s`** (validated by the schema via
`cv.positive_time_period_milliseconds(min=1000)`). Anything smaller causes
`esphome config` to reject the configuration with a friendly diagnostic.
Values below 5-10 s offer no benefit for billing-grade accuracy and just clutter
the bus.

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

Connects the module's open-drain DRDY output to a GPIO on the ESP32. When specified, the pin is configured in `setup()` at boot.

> On the current firmware the pin is logged in `dump_config` but is not used as a read trigger — the instantaneous registers are polled on `update_interval`. Declaring the pin does not change behavior, but it reserves it for future firmware revisions with interrupt-driven reads.

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

Fixes the current-sensor family on the module side. On firmware v1.2+ the value is written to flash and becomes a **precondition** for writing the CT model: the module will refuse to write the model if the class is not set. On earlier firmware the value is accepted by the schema and is applied automatically on upgrade.

| Value | Status |
|---|---|
| `SCT_013` | Available now, default |
| `WIRED_CT` | Reserved for future SKUs |
| `BUILTIN_CT` | Reserved for future SKUs |

```yaml
rbamp:
  sensor_class: SCT_013   # default; may be omitted
```

For more on choosing the clamp and family, see [03_sensor_selection.md](03_sensor_selection.md).

---

### `ct_model`

| Attribute | Value |
|---|---|
| Type | `enum` (`SCT_013_005`, `_010`, `_020`, `_030`, `_050`) |
| Default | None (optional) |
| Required | No |

Writes the CT clamp model identifier to the module's flash. On firmware v1.2+ this automatically loads the factory coefficients for the chosen model — no additional calibration steps are required.

| YAML value | Code | Nominal current | Status v1.3 |
|---|:---:|---|---|
| `SCT_013_005` | 1 | 5 A | production |
| `SCT_013_010` | 2 | 10 A | production |
| `SCT_013_020` | 6 | 20 A | **production (new in v1.3)** |
| `SCT_013_030` | 3 | 30 A | production (default SKU) |
| `SCT_013_050` | 4 | 50 A | production |

> `SCT_013_100` (code 5) was **removed in v1.3** — the code is reserved in
> firmware and returns `DEV_ERR_PARAM`. The per-class accept-set for `SCT_013`
> is `{1, 2, 3, 4, 6}` (non-contiguous). Non-matching values are rejected
> at `esphome config` via `cv.Invalid`. For more, see
> [03 · Current-sensor selection](03_sensor_selection.md#per-class-ct-validation-new-in-v13).

```yaml
rbamp:
  ct_model: SCT_013_030
```

Applied once in `setup()`. Each write is accompanied by a ~700 ms flash-write (the module NACKs all I²C operations during this time — the component feeds the watchdog automatically).

**Mutually exclusive** with `ct_models:` — only one of the two may be used per `rbamp:` block.

For more, see [03_sensor_selection.md](03_sensor_selection.md). Cross-reference: [the `ct_model` reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference).

---

### `ct_models`

| Attribute | Value |
|---|---|
| Type | list of **1–3** enum values |
| Default | None (optional) |
| Required | No |

Per-channel CT clamp models — for UI2/UI3 with mixed clamps on different channels. Accepts the same enum values as `ct_model:`. The schema validates the array length via `cv.Length(min=1, max=3)`; the number of elements must match the number of physical channels on the module (1 for UI1, 2 for UI2, 3 for UI3).

```yaml
rbamp:
  id: ui3_meter
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
  # CH0=5A for standby loads (maximum resolution),
  # CH1=30A for main household loads,
  # CH2=20A for a water heater / medium-power branch (~3-5 kW)
  # NOTE v1.3: SCT_013_100 reserved; for > 50A — several SCT_013_050 in parallel or a WIRED_CT SKU (roadmap)
```

On firmware v1.2+ each channel gets its own set of factory coefficients independently. The total setup time at boot is ~2-3 seconds (~700 ms flash-write per channel).

**Mutually exclusive** with `ct_model:`.

---

### `bidirectional`

| Attribute | Value |
|---|---|
| Type | `bool` |
| Default | `false` |
| Required | No |

Enables the export-energy register read path for the STANDARD / PRO tiers. When `true`, the component attempts to read the per-channel export-power registers on each period snapshot and accumulate export Wh separately.

**Status on the current firmware**: the key is accepted by the schema and reserves the `energy_export_wh[]` slots in NVS, but the export-energy register is not yet wired in firmware. The `energy_exported_*` sensors publish 0 until firmware that implements this register ships.

Declare `energy_exported` (or `_1` / `_2`) under `sensor.platform: rbamp` only when `bidirectional: true` is set.

```yaml
rbamp:
  bidirectional: true   # meaningful on STANDARD / PRO
```

---

### `new_address`

| Attribute | Value |
|---|---|
| Type | `i2c_address` (0x08..0x77) |
| Default | None (optional) |
| Required | No |

Triggers a one-time I²C address change at boot. It must differ from `address` — the validator raises an error if they are equal.

On firmware **v1.3** a **two-phase commit** is used — production-OK without
factory-provisioning gating. Compatibility behavior:

| Capability bit | Behavior |
|---|---|
| `CAP_TWO_PHASE_ADDR` (bit7) set | **Production path** — two-phase commit: staged write to `REG_I2C_ADDRESS` → magic `0xA5` to `REG_ADDR_COMMIT_MAGIC` → `CMD_COMMIT_ADDR` → re-enumeration. |
| `CAP_TWO_PHASE_ADDR` not set (legacy v1.0-v1.2) | **Legacy fallback** — single-phase write, requires factory-provisioning mode. Logs `WARN_FACTORY_MODE_REQUIRED` if not in provisioning. |

The full flow runs once in `setup()`:

1. Probe the current `address`. If the module does not respond there but does respond at `new_address`, the component adapts to the new address with a warning (assuming a previous boot already applied the change) and skips the write.
2. Check the `CAP_TWO_PHASE_ADDR` capability bit. Set → production path (see below). Not set → legacy path with the provisioning-mode check.
3. **Production path (v1.3)**: write `REG_I2C_ADDRESS` ← `new_address` (staged) → write `REG_ADDR_COMMIT_MAGIC` ← `0xA5` → write `CMD_COMMIT_ADDR` → wait ~700 ms → re-probe `new_address`. No factory-mode gating is required.
4. **Legacy path (v1.0-v1.2)**: single-phase write, requires factory-provisioning mode on the module side.
5. The component switches its internal I²C address via `i2c::I2CDevice::set_i2c_address()` to `new_address` and verifies that the module responds.
6. If the module does not respond after the change, the component calls `mark_failed()` and stops.

**After a successful change**: update the YAML to `address: <new>` and remove `new_address:`. If the module is at the new address on the next boot but `new_address:` is still in the YAML, the boot-time probe of the old address fails, the probe of the new address succeeds, the component adapts with a warning, and no re-write happens.

> ⚠ **Develop-mode-only on legacy firmware (v1.0-v1.2)**.
>
> Standard production modules on legacy firmware shipped with provisioning mode disabled. Writing `new_address:` to such a module without factory mode will be rejected (warning `WARN_FACTORY_MODE_REQUIRED`). On v1.3 firmware this restriction is gone — the capability bit detects the production-OK path automatically.

> ⚠ **Re-enumeration after commit**. After a successful commit the module resets and re-enumerates at the new address. Subsequent calls within the same `setup()` are directed to the new address transparently — but any other master on the bus (a raw I²C tool, a debug probe, another ESP32) still considers the module to be at the old address until it updates its own state.

```yaml
rbamp:
  address: 0x50
  new_address: 0x51   # remove this line after the first successful boot
```

**Recovery**: if the address change was applied but the module does not respond, see [10_troubleshooting.md](10_troubleshooting.md) → "Address changed, but the module doesn't respond".

Cross-reference: [the runtime address change reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference).

---

### `fleet_gc_enable` (v1.3 NEW)

| Attribute | Value |
|---|---|
| Type | `bool` |
| Default | `false` |
| Required | No |
| Capability gate | `CAP_GC_LATCH` (bit1 of `REG_CAPABILITY`) |

Opt-in: enables receipt of the **General-Call latch broadcast** from the master. When
the ESP32 emits a 5-byte GC frame to address `0x00`, all modules with `fleet_gc_enable:
true` (and a matching `group_id:`) atomically latch their period accumulators
at a single wire moment. This delivers **billing-grade synchronization** between
sub-meters.

**Capability behavior**: at boot the component reads `REG_CAPABILITY` (0x57). If
`CAP_GC_LATCH` (bit1) is not set → warning + skip without apply:

```
[W][rbamp:xxx]: fleet_gc_enable requested but CAP_GC_LATCH not in capability;
firmware v1.0-v1.2 doesn't support General-Call. Falling back to sequential
latch (~270 µs × N inter-module skew at 50 kHz).
```

**Persistence**: on apply the component **writes** `REG_FLEET_CONFIG.bit0`,
saves to flash via `CMD_SAVE_USER_CONFIG`, and **RESETs** the device — GC mode
is configured at init time, not toggleable live. Via read-compare-write
this happens once on the first boot; subsequent boots skip it.

```yaml
rbamp:
  - id: mains
    fleet_gc_enable: true     # opt-in, capability-gated
    group_id: 1               # see below
  - id: boiler
    fleet_gc_enable: true
    group_id: 1               # same group → synchronized
```

Cross-reference: [the Fleet Group-Commit reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference), [04_hardware.md Multi-module fleet](04_hardware.md).

---

### `group_id` (v1.3 NEW)

| Attribute | Value |
|---|---|
| Type | `uint8_t` (0..255) |
| Default | `0` |
| Required | No |

A cluster identifier for **selective** GC latch. The module accepts a GC frame
only if the group field in the frame matches this `group_id` **OR** equals
`0x00` (all-call).

**Use cases**:

- **Multi-tenant**: one ESP32 drives several independent clusters of
  modules (e.g. 3 apartments in one panel). Cluster 1 = `group_id: 1`,
  cluster 2 = `group_id: 2`. The master emits a GC frame with group=1 → only
  cluster 1 latches.
- **Single-tenant**: leave `group_id: 0` (all-call) or set `group_id: 1`
  as a label — the behavior is identical, all modules latch.

**Persistence**: written to `REG_GROUP_ID` and saved to flash via
read-compare-write. Changing it requires a boot.

```yaml
rbamp:
  - id: apt1_mains
    group_id: 1
  - id: apt2_mains
    group_id: 2
```

---

### `broadcast_latch` (deprecated, v0.4.0 legacy)

| Attribute | Value |
|---|---|
| Type | `bool` |
| Default | `false` |
| Required | No |
| Status | **Deprecated** — replaced by `fleet_gc_enable:` (v1.3+). |

A legacy alias for the General-Call broadcast latch. In v1.3 this key is accepted
by the schema as a no-op (for non-breaking migration from v0.4.0 YAML), but it has
**no effect**. For new code, use `fleet_gc_enable:` — it is capability-gated
and works on v1.3 firmware.

It will be removed in a future minor release; a deprecation warning appears in the log:

```
[W][rbamp:xxx]: 'broadcast_latch:' is deprecated since v1.3 — use
'fleet_gc_enable:' instead. This key has no effect.
```

Cross-reference: [the Fleet Group-Commit reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference).

---

### `topology`

| Attribute | Value |
|---|---|
| Type | `enum` (`SINGLE`, `SPLIT_PHASE`, `THREE_PHASE`) |
| Default | `SINGLE` |
| Required | No |

Declares the physical configuration of the module.

| Value | Current firmware | When it becomes authoritative |
|---|---|---|
| `SINGLE` | Cosmetic (logged in `dump_config`). The channel count is derived from the declared `current[_1/_2]` slots. | Already matches every current SKU; will be confirmed from an in-band register once that ships in firmware. |
| `SPLIT_PHASE` | Accepted by the schema, written to `dump_config`. Phased keys (`voltage_a/b/c`, `current_a/b/c`, …) may be declared in `sensor.platform: rbamp` — the component then reserves the slots, but there is nothing to publish into them yet. | After the rbAmp-U2I2 SKU ships with an in-band topology register. |
| `THREE_PHASE` | Same as `SPLIT_PHASE`. | After the rbAmp-U3I3 SKU ships with an in-band topology register. |

On the current firmware there is no in-band topology register (reserved for future revisions). The hint is informational: it goes into the `dump_config` line, but the actual channel count is derived independently from the declared `current[_1/_2]` sensor slots.

```yaml
rbamp:
  topology: SINGLE         # UI1, UI2, UI3, I1, I2, I3 — current SKUs
  # topology: SPLIT_PHASE  # US split-phase (U2I2) — future SKU
  # topology: THREE_PHASE  # European 3-phase (U3I3) — future SKU
```

Once the module starts publishing topology via its register, the component will prefer the value from the module and use the YAML hint only as a fallback. The `SINGLE` default matches every current SKU — no changes to deployed configs will be needed.

Cross-reference: [the `topology` reference](https://www.rbamp.com/docs/modules-basic-standard-api-reference).

---

## 2. Sensor platform — `sensor.platform: rbamp`

Each `sensor:` block declares one set of named sensors bound to the parent `rbamp:` component. The only required key in the block is `rbamp_id`.

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

All fields under the `sensor:` block are optional. Declare only the quantities your scenario needs — the component reads only the registers that correspond to the declared sensors.

### `rbamp_id`

| Attribute | Value |
|---|---|
| Type | `use_id(RbAmpComponent)` |
| Required | **Yes** |

References the `rbamp:` block this sensor group is bound to.

---

### 2.1 Single fields

Used for the current SKUs (UI1, UI2, UI3, I1, I2, I3). All fields are optional. Mixing them with phased fields (`voltage_a`, etc.) raises a validation error.

Each field accepts the standard ESPHome `sensor.sensor_schema` subkeys: `name`, `id`, `filters`, `unit_of_measurement`, `accuracy_decimals`, `icon`, and so on.

#### `voltage`

| Attribute | Value |
|---|---|
| Unit | V |
| `device_class` | `voltage` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Source | RMS mains voltage (instantaneous module register) |

RMS mains voltage. Read on every `update()` once the module is ready. The 4-byte float is read with per-byte retry (3 attempts × 5 ms) and passes a `std::isfinite()` check plus `|val| < 10000`.

Zero is a valid value to publish: a mains-outage or brownout event yields U ≈ 0 V and passes through to HA without filtering — the sanity filter only discards NaN/Inf, not legitimate zeros.

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

RMS current for channels 0, 1, 2 respectively. Channel 0 is the main CT clamp; channels 1 and 2 are present on the UI2 / UI3 and I2 / I3 SKUs.

The active channel count is derived from the number of declared slots: if only `current` is declared, `n_channels_ = 1`; with `current` and `current_1`, `2`; adding `current_2` gives `3`.

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

Active power in watts, **signed**. Negative values = reverse flow (generation into the grid) on the STANDARD / PRO tiers. On BASIC, negative instantaneous values within the period window are clamped to 0 at the firmware level — the period average is `≥ 0`. The instantaneous active power can still read negative at the moment of generation.

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

Accumulated consumed energy in Wh. Computed entirely on the ESP32 from the formula:

```
E_Wh[ch] += avg_P_W[ch] * master_dt_s / 3600
```

where `avg_P_W[ch]` is the period-average power read from the module after each latch command, and `master_dt_s` is the ESP32 wall-clock interval between latches.

The values are saved to NVS every 5 minutes and restored before the first `publish_state` at boot — this keeps the HA Energy dashboard from interpreting an instantaneous 0 as a counter reset. The worst-case loss on a sudden power failure is up to 5 minutes of energy (≈ 5 Wh at an average of 60 W).

`state_class: total_increasing` is required for the HA Energy dashboard. The value rises monotonically; it does not decrease under normal operation. When the NVS layout changes (a version bump), the counter starts from 0.

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

Export (generation) energy in Wh, accumulated separately from `energy`. Wired in the component but publishes 0 until firmware adds the corresponding period-negative-power register. Set `bidirectional: true` in the `rbamp:` block when declaring these sensors.

#### `power_factor` / `power_factor_1` / `power_factor_2`

| Attribute | Value |
|---|---|
| Unit | (dimensionless) |
| `device_class` | `power_factor` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 3 |
| Dependencies | Requires `current` + `voltage` for the corresponding slot |

Power factor in the range −1..+1. A negative PF means a leading or lagging load; the sign convention is defined by the firmware. The sanity filter (§B.5) discards values outside `|pf| > 10000`; there is no lower bound.

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

Reactive power in VAr. Signed. Published from the instantaneous-register block on every `update()` cycle.

```yaml
reactive_power:
  name: "Reactive Power"
```

> **Note on `device_class: reactive_power`** — ESPHome accepts it
> (the `DEVICE_CLASS_REACTIVE_POWER` constant), and the value passes through to Home
> Assistant. However, Home Assistant's official `device_class` list has
> changed over time: in some versions `reactive_power` is available in the UI
> and works with the unit_of_measurement `VAr`, while in others it is
> displayed as a generic sensor with no specialized icon or unit conversion.
> The data itself is always published (it is just a `state`); the only question is
> how HA renders the sensor in Lovelace. If the sensor shows up as unknown in
> your HA version, that is cosmetic, not a functional
> regression. Remove the `reactive_power:` key from the YAML if you do not use the
> sensor.

#### `apparent_power`

| Attribute | Value |
|---|---|
| Unit | VA |
| `device_class` | `apparent_power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Source | Computed on the master: `S = V_rms × I_rms[0]` |
| Dependencies | Requires `current` + `voltage` |

Apparent power in VA. Computed by the component from the `U_rms` and `I0_rms` values read in the same `update()` cycle — no separate register is read. Both reads must succeed in the same cycle; if even one fails (retry exhausted), no state is published in that cycle and the last value in HA is preserved.

`apparent_power` is in `SHARED_FIELDS` and works with any topology, but it depends on `voltage` and `current` for the V × I computation.

```yaml
apparent_power:
  name: "Apparent Power"
```

---

### 2.2 Phased fields (future SKUs)

Reserved for the split-phase (U2I2, US market) and three-phase (U3I3) future SKUs. The schema accepts them now — a user can prepare the configuration ahead of time. On the current firmware, declaring phased slots reads the corresponding register addresses (which return `0.0` for unimplemented channels) and publishes 0 — not an error, but not useful data either.

Single and phased fields are **mutually exclusive** within one `sensor:` block. The validator raises an error:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...).
```

All phased fields share the same `sensor_schema` defaults as their single-phase counterparts (the same units, `device_class`, `state_class`, `accuracy_decimals`).

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

> Per-phase dependency validation is not enforced for phased fields (only `_SINGLE_SLOT_DEPS` for single-phase). Firmware support for the split / three-phase variants is reserved for future SKUs.

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

Two fields work in any topology group, including the case where no single-phase or phased current sensor is declared at all (a voltage-only or frequency-only deployment).

#### `frequency`

| Attribute | Value |
|---|---|
| Unit | Hz |
| `device_class` | `frequency` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 0 |
| Type | `uint8_t` (not float) |

Mains frequency, read as a single byte. The component publishes the value only if it equals 50 or 60 — other values (`0` = ZC not caught, or implausibly large) are discarded without publishing. This avoids the "Unknown" → 0 Hz → 50 Hz transition in the HA entity during warm-up.

```yaml
frequency:
  name: "Mains Frequency"
```

#### `apparent_power` (shared)

See the identical entry under the single fields above. `apparent_power` is in `SHARED_FIELDS` and can appear in a single-phase or phased sensor block, provided that `voltage` and `current` (or `current_a`) are also declared.

---

### 2.4 Schema validation rules

The Python validator `_validate_topology_consistency` applies two classes of rules during `esphome config` / compile time — before any C++ code runs.

#### Topology mutual exclusion

Single-phase fields (`voltage`, `current`, `current_1`, etc.) and phased fields (`voltage_a`, `voltage_b`, etc.) cannot coexist in one `sensor:` block. An attempt to mix them:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...). Pick one group based
on your rbAmp SKU.
```

#### Per-slot companion requirements (single-phase only)

Each derived field requires the input fields the module needs to compute it:

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
| `ct_model:` ↔ `ct_models:` mutually exclusive | Only one of the two may be used per `rbamp:` block — otherwise a validation error. |
| `new_address` ≠ `address` | If they match — `cv.Invalid`. |
| `address` in the range `0x08..0x77` | `cv.i2c_address` is applied. |

All validation errors are reported during `esphome compile` with a human-readable message pointing at the offending key. There is no need to flash hardware to find a configuration error.

---

## 3. Data flow and timing

The conceptual flow of a single `update()` cycle:

```
Module (autonomous)                  ESP32 (ESPHome component)
─────────────────────────────────   ────────────────────────────────────────────
Internal ADC sampling and
RMS / P / PF / Q computation
  ↓ (~200 ms per cycle)
Atomically publishes the block       update() fires every update_interval (60 s)
of instantaneous registers            ↓
  ↓                                 phase 1 — latch:
                                      write the latch command
Closes the period accumulator,        50 ms timeout (non-blocking)
opens a new one                       ↓ (main loop keeps running)
  ↓
  wait 50 ms                        phase 2 — period snapshot:
                                      read the valid flag
                                      read per-channel average powers
                                      E_Wh[ch] += avg_P × dt_s / 3600
                                      save to NVS if 5 minutes elapsed
                                      ↓
                                    phase 3 — instantaneous values:
                                      read the status register
                                      read U_rms, I[0..n]_rms, P[0..n],
                                            PF[0..n], Q[0..n], frequency
                                      publish all bound sensors
                                      ↓
                                    Push state to HA over the native API
```

**Characteristic timings**:

| Event | Cadence |
|---|---|
| Internal commit of the instantaneous block by the module | ~200 ms |
| `update()` call | `update_interval` (60 s by default) |
| Settle timeout after the latch command | 50 ms (non-blocking) |
| Save to NVS | Every 5 minutes |
| Pause between per-byte retries | 5 ms, up to 3 attempts |
| Flash-write window when writing the model | ~700 ms (the module NACKs the whole time) |

The latch-settle timeout is non-blocking — the cooperative ESPHome scheduler keeps servicing WiFi, the API, and other components during the 50 ms wait. The remaining `update()` phases (reading the instantaneous registers) run immediately after returning from the latch phase.

The warning `[W][component:522]: rbamp took a long time for an operation (XXX ms)` is expected and harmless when it does appear. On v1.3 firmware the component reads the instantaneous-values block via a single burst read (READ auto-increment), so a healthy cycle stays well under 30 ms. The warning surfaces when the burst path falls back to per-register reads (4 transactions per float, up to 36 transactions at 50 kHz, plus retries) — typically due to a marginal bus or legacy firmware without the READ-burst capability.

---

## 4. I²C bus settings

The `rbamp` component inherits the I²C bus configured in the top-level `i2c:` block. Recommended settings on the current firmware:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz   # 100 kHz causes periodic NACKs; 50 kHz reduces them ~5x
  scan: true         # optional: logs the addresses found at boot
```

**Bus speed**: use `50kHz` on the current firmware. The module's I²C peripheral periodically NACKs at 100 kHz (~20% of transactions) due to a known behavior of the ESP-IDF `i2c_master` driver (confirmed in [esp-idf issue #9426](https://github.com/espressif/esp-idf/issues/9426), marked "Won't Do" by Espressif). The component's three-layer mitigation (retry + sanity + 50 kHz) drops the effective bad-read rate below 1%. When a firmware fix ships, the speed can be raised back to 100 kHz with a single line of YAML.

**Pull-up resistors**: 4.7 kΩ to 3.3 V on SDA and SCL is recommended. Larger values (10 kΩ) increase the rise time and raise the NACK rate at 100 kHz.

**Multiple devices**: the ESP32 I²C bus supports up to 112 devices at distinct 7-bit addresses. Use `address:` and `new_address:` to assign a unique address to each module. `scan: true` in the `i2c:` block logs every address that responds at boot — verify that every module is detected before enabling period metering.

> The full wire-level register map and the module's command set are on the [public API reference page](https://www.rbamp.com/docs/modules-basic-standard-api-reference). For the typical user of the YAML schema this chapter is sufficient — every register is hidden behind the component's declarative interface.

---

## 5. Identity surface (v1.3 NEW) — public C++ methods

The v1.3 component exposes identity / diagnostic getters that are accessible from a YAML
`lambda:` and through the template platform. All methods return a `std::string` (for
text_sensor compatibility) or primitive types — no exception throws, everything via
cached state.

| Method | Return type | Source | Description |
|---|---|---|---|
| `get_variant_str()` | `std::string` | `REG_HW_VARIANT` (0x55) | "UI1"/"UI2"/"UI3"/"I1"/"I2"/"I3"/"UNK" — the module's variant |
| `get_capability_hex()` | `std::string` | `REG_CAPABILITY` (0x57) | "0xNNNN" — the full 16-bit capability bitmap |
| `get_uid_hex()` | `std::string` | `REG_UID` (0x5C × 12 bytes) | 24-char hex (96-bit chip UID) |
| `get_last_error_str()` | `std::string` | `REG_ERROR` (0x02) | "OK"/"ERR_PARAM"/"ERR_BUSY"/"ERR_LATCH"/"ERR_CRC" |
| `get_event_flags()` | `uint16_t` | `REG_EVENT_FLAGS` (0x03) | sticky bits: bit3=ERR_LATCH, bit5=ADDR_CHANGED, etc. |
| `get_firmware_version()` | `std::string` | `REG_VERSION` (0x01) | "1.3" / "1.2" / "1.0" |
| `read_gc_tick_received()` | `uint16_t` | `REG_GC_LAST_TICK` (0x58) | last latched tick value from the GC frame |
| `is_capability_supported(bit)` | `bool` | cached `REG_CAPABILITY` | helper for capability-gated code |

**Caching**: `variant` / `capability` / `uid` / `firmware_version` are read
**once in `setup()`** and cached — repeat calls do not go to the bus.
`last_error_str()` / `event_flags()` / `gc_tick` are read **on every
call** for freshness.

### Use through a template text_sensor

```yaml
text_sensor:
  - platform: template
    name: "Mains variant"
    update_interval: 30s
    lambda: 'return id(mains_meter).get_variant_str();'
    # → entity_id: sensor.mains_variant, value: "UI1"

  - platform: template
    name: "Mains capability"
    update_interval: 30s
    lambda: 'return id(mains_meter).get_capability_hex();'
    # → "0x0718" = CAP_GC_LATCH | CAP_TWO_PHASE_ADDR | CAP_SAVE_USER_CONFIG | CAP_CLEAR_ERROR

  - platform: template
    name: "Mains UID"
    update_interval: 30s
    lambda: 'return id(mains_meter).get_uid_hex();'

  - platform: template
    name: "Mains last error"
    update_interval: 30s
    lambda: 'return id(mains_meter).get_last_error_str();'

  - platform: template
    name: "Mains firmware version"
    update_interval: 30s
    lambda: 'return id(mains_meter).get_firmware_version();'
```

These text_sensors appear in HA as `sensor.mains_variant` and so on — handy
for inventory dashboards, asset-tracking scenarios, and post-flash verification.

### Use through `lambda:` in automations

```yaml
sensor:
  - platform: rbamp
    rbamp_id: mains
    energy: { name: "Mains Energy" }

# In an automation: skip publishing if the module is on legacy firmware
automation:
  - trigger:
      platform: state
      entity_id: sensor.mains_energy
    condition:
      - condition: lambda
        return: 'return id(mains).is_capability_supported(0x0001);'  # CAP_HONEST_BOOT
    then:
      - service: persistent_notification.create
        data:
          message: "Mains energy published (firmware capability check passed)"
```

---

## 6. Fleet & GC sync (v1.3 NEW)

### `transmit_gc_frame(group, tick)` — programmatic GC emit

```cpp
void transmit_gc_frame(uint8_t group, uint16_t tick);
```

Emits a 5-byte General-Call latch frame to bus address `0x00`:

```text
START | 0x00 | A | 0xA5 | A | 0x27 | A | group | A | tick_lo | A | tick_hi | A | STOP
```

All modules with `fleet_gc_enable: true` + a matching `group_id:` (or `group_id: 0`)
latch their period accumulators **synchronously** at a single wire moment. The tick
value is written to each module's `REG_GC_LAST_TICK` for check_sync verification.

**Auto-emit**: the component emits a GC frame **automatically** in `update()` if
`fleet_gc_enable: true` and the `CAP_GC_LATCH` capability is set. A manual emit is needed
only for extra synchronization (for example, on an HA-side event trigger).

**Capability check**: if `CAP_GC_LATCH` is not set — vendor warning, no emit.

### `fleet_apply_now()` — force re-apply read-compare-write

```cpp
void fleet_apply_now();
```

Forcibly repeats the read-compare-write boot-writeback procedure:
re-reads `REG_SENSOR_CLASS`, `REG_CT_MODEL_CH0/1/2`, `REG_FLEET_CONFIG`,
`REG_GROUP_ID`, compares them with what the YAML requested, writes the delta + verifies.

Use case: after a `factoryReset()` via a debug tool, or for post-OTA
verification that the config applied.

### Bench-validated synchronization

On the Fix-A fleet UI1@0x50 + I2@0x51 + I3@0x52 (2026-06-16):

```
GC emit tick=42, group=1 → 3 modules ACK
check_sync(tick=42) → SYNC: t0x50=42 t0x51=42 t0x52=42 (3/3 in_sync)
```

**Validated bench output**:

- L8 soak 180 cycles, 0 sync drops
- GC sync 3/3 every round under a 0.58 A load
- L9 energy rel_err = 0.0000% (master wall-clock canon)

---

## 7. Native API services (v1.3 NEW) — recovery automations

The v1.3 component does not register services automatically — the YAML does.
But the public methods `write_clear_error()` / `write_reset()` /
`transmit_gc_frame()` are designed to be used through the `api: services:`
pattern. Canonical recipes:

```yaml
api:
  port: 6053

  services:
    # Recovery: clear REG_ERROR + EVENT_FLAGS bit3
    - service: mains_clear_error
      then:
        - lambda: 'id(mains).write_clear_error();'

    # Maintenance: soft reset (preserves flash)
    - service: mains_reset
      then:
        - lambda: 'id(mains).write_reset();'

    # Fleet: manual GC latch (beyond the auto-emit in update())
    - service: fleet_latch_now
      then:
        - lambda: 'id(mains).transmit_gc_frame(1, 0);'   # group=1, tick=auto-increment

    # Maintenance: force config re-apply (post-OTA verify)
    - service: mains_fleet_apply
      then:
        - lambda: 'id(mains).fleet_apply_now();'
```

After this, HA can call the services from automations:

```yaml
automation:
  - alias: "Auto-clear meter error on detection"
    trigger:
      platform: state
      entity_id: sensor.mains_last_error
      from: "OK"
    action:
      - delay: 60s   # wait one update_interval to confirm a persistent error
      - service: esphome.<node>_mains_clear_error
      - condition: state
        entity_id: sensor.mains_last_error
        state: "ERR_PARAM"
      - service: persistent_notification.create
        data:
          title: "Meter parameter error"
          message: "Meter at {{ states('sensor.mains_capability') }} reports ERR_PARAM after auto-clear retry. Check YAML config."
```

---

## 8. Error & recovery (v1.3) — REG_ERROR vs EVENT_FLAGS bit3

In v1.3 the module has **two independent error channels**:

| Channel | Register | Latch semantics | Clearing |
|---|---|---|---|
| **Sync (last-write)** | `REG_ERROR` (0x02) | last-write wins, overwritten by the next operation | `write_clear_error()` or the next successful operation |
| **Async durable** | `REG_EVENT_FLAGS` bit3 (0x03) | sticky, re-latches ~200-300 ms if the root cause is not resolved | `write_clear_error()` (one operation clears both) |

### Read pattern

```cpp
// Sync — fresh after the last operation
std::string err = id(mains).get_last_error_str();    // "ERR_PARAM" / "OK" / ...

// Durable — sticky bit3, sees errors even between polls
uint16_t flags = id(mains).get_event_flags();
bool latched_error = (flags & (1 << 3)) != 0;
```

### Recovery semantics

Clearing REG_ERROR alone is **not enough** if the firmware re-latches bit3:

```cpp
id(mains).write_clear_error();                       // clears REG_ERROR + EVENT bit3
delay(300);                                          // wait for the re-latch window
uint16_t flags_after = id(mains).get_event_flags();
if (flags_after & (1 << 3)) {
    // Root cause not resolved — firmware re-latched bit3
    ESP_LOGW("rbamp", "ERROR persists after clear — check config / param");
}
```

**For a full recovery**:

1. **Resolve the cause** (fix the YAML CT model code, correct sensor_class).
2. `write_clear_error()` — clears REG_ERROR + EVENT_FLAGS bit3.
3. Verify via `get_event_flags()` after ~300 ms that bit3 is not re-latched.

See [10_troubleshooting.md §6 Error & recovery](10_troubleshooting.md).