# 06 · YAML Cookbook

This chapter opens with the **flagship scenarios** — the real-world deployment
patterns the component was designed for in v1.3 (mains + N sub-loads on one bus
with `fleet_gc_enable` synchronization, provisioning workflow, multi-channel
mixed-CT, fleet GC sync). After those come the single-module scenarios, from a
minimal UI1 to a production deployment with HA automations.

Each example is self-contained: copy it, fill in your board name and Wi-Fi
credentials, compile, and flash. Wherever an example departs from the minimal
template, an explanation tells you why.

Cross-references:

- Component schema, register map, period-metering protocol, multi-module topology, fleet GC sync, and the NACK loose-sanity rule: [09 · API reference](09_api_reference.md)

| # | Scenario | Topology | When to use |
|:---:|---|:---:|---|
| **1** | **Mains + N sub-loads — the 80% canon** | UI1 + N× I2/I3 | Whole-home metering with a breakdown by sub-branches |
| **2** | **Provisioning workflow (virgin → fleet)** | sequential single | Bring-up of a new batch of modules |
| **3** | **Multi-channel mixed-CT (UI3 with different clamps)** | UI3 | Metering of mixed loads with accuracy optimization |
| **4** | **Fleet GC sync (tight synchrony)** | N× UI/I | Multi-tenant clusters and sub-metering |
| 5 | UI1 — single-channel (minimal) | UI1 | Hello-world single-module |
| 6 | UI1 with customization | UI1 | All the standard ESPHome knobs |
| 7 | UI3 — three-channel with mixed clamps | UI3 | Single-module multi-channel |
| 8 | Multi-module bus (GC sync) | N× UI/I | `fleet_gc_enable` + `group_id` on three modules |
| 9 | One-time I²C address change | single | Provisioning a single module |
| 10..20 | HA integrations and automations | mixed | Production deployment patterns |
| 21 | STANDARD (STD) module — on-device energy totals | any | Grand totals, reactive energy, `start_current_ua:` |
| 22 | Switching the SCT rating, `ct_model_force` | any | Rating change without losing accuracy |

> Scenarios 1-4 are the **flagship** ones, targeting the canonical v1.3
> deployment of the package. Their code uses `fleet_gc_enable` + identity surface
> + read-compare-write and is exercised on a heterogeneous
> UI1+I2+I3 setup. Scenarios 5-22 are single-module and compact
> patterns. Scenario 8 is the short three-module bus configuration.

---

## 1 · Mains + N sub-loads (the 80% canon) — integrated metering system

**Goal**: the canonical deployment of the package — a coherent metering system on
a heterogeneous fleet, closed within a single ESPHome boot (discovery → configure
→ enable GC → loop[publish_state + auto-emit GC tick]).

**Hardware (UI1 + I2 + I3 test setup)**: UI1@0x50 + I2@0x51 + I3@0x52;
~0.58 A load; pull-ups per the 3-module row of the [04_hardware.md](04_hardware.md#multi-module-bus-primary-topology) table (cut all, one external 2.7–3.3 kΩ pair).

```yaml
esphome:
  name: home-fleet

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: INFO

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

# Three rbAmp modules on shared bus, all with fleet_gc_enable + group_id=1
# for synchronized period latching (tight sync between mains + sub-meters).
rbamp:
  - id: mains_meter         # UI1 on the mains feed
    address: 0x50
    sensor_class: SCT_013
    ct_model: SCT_013_030
    fleet_gc_enable: true
    group_id: 1

  - id: boiler_meter        # I2 on the boiler + washing machine
    address: 0x51
    sensor_class: SCT_013
    ct_models: [SCT_013_020, SCT_013_010]
    fleet_gc_enable: true
    group_id: 1

  - id: panel_meter         # I3 on the distribution-panel branches
    address: 0x52
    sensor_class: SCT_013
    ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
    fleet_gc_enable: true
    group_id: 1

sensor:
  - platform: rbamp
    rbamp_id: mains_meter
    voltage:      { name: "Mains Voltage" }
    current:      { name: "Mains Current" }
    power:        { name: "Mains Power", id: mains_power_state }
    power_factor: { name: "Mains PF" }
    energy:       { name: "Mains Energy" }
    frequency:    { name: "Mains Frequency" }

  - platform: rbamp
    rbamp_id: boiler_meter
    voltage:   { name: "Boiler Voltage", internal: true }   # energy needs voltage AND current
    current:   { name: "Boiler Current" }       # 20A CT
    current_1: { name: "Washer Current" }       # 10A CT
    energy:    { name: "Boiler Energy" }
    energy_1:  { name: "Washer Energy" }

  - platform: rbamp
    rbamp_id: panel_meter
    current:   { name: "Lighting Current" }     # 5A CT
    current_1: { name: "Appliances Current" }   # 30A CT
    current_2: { name: "Sub-panel Current" }    # 20A CT

  # Optional: template sensor for the household power (same sensor: list, not a second sensor: key)
  - platform: template
    name: "Household Total Power"
    update_interval: 60s
    accuracy_decimals: 0
    unit_of_measurement: "W"
    device_class: power
    lambda: |-
      // Sum of power from the mains meter only (sub-meters are a subset of the same mains)
      auto v = id(mains_power_state);
      return v.has_state() ? v.state : 0.0f;
```

### Example output (~0.58 A load)

```text
[I][i2c:136]: Found i2c device at address 0x50
[I][i2c:136]: Found i2c device at address 0x51
[I][i2c:136]: Found i2c device at address 0x52
[C][rbamp:450]: mains_meter:  variant=UI1, capability=0x0718, fleet_gc=on, group=1
[C][rbamp:450]: boiler_meter: variant=I2,  capability=0x0618, fleet_gc=on, group=1
[C][rbamp:450]: panel_meter:  variant=I3,  capability=0x…, fleet_gc=on, group=1
[I][rbamp:493]: GC latch enabled fleet-wide: 3 modules in group 1

ITER 1 @ 60s: GC tick=1, 3/3 in_sync
  mains:   V=232.6  I=5.79  P=1259 W   E_total: 0 → 0.35 Wh
  boiler:  I=[11.58, 3.87]                                ; current-only
  panel:   I=[3.84, 2.55, 5.84]                           ; current-only
```

### Notes for use

- **One synchronized loop — the whole system.** On every `update_interval` tick,
  the ESP32 emits a GC frame to 0x00 → all 3 modules latch their period
  accumulators at the same wire moment (validated 3/3 in_sync under load).
- **`mains_power`** in a heterogeneous fleet = the active power from UI1
  (mains_meter). The sub-meters (I2/I3) are current-only, with no canonical P. In
  the 80% canon, `mains_power` ≈ the total power of all loads (mains_meter covers
  the feed).
- **Wh on BASIC modules is integrated on the ESP32** against its own clock (the time
  between latches), so all modules of the fleet share one time base. STANDARD
  modules keep their own energy counters, which the component accumulates.
- **MISS-resilient.** If one module goes off-bus (provisioning / power cycle), the
  rest keep working; HA shows "Unavailable" for its entities until it recovers.

---

## 2 · Provisioning workflow (virgin → fleet)

**Goal**: the canonical bring-up procedure for new modules, from the factory
`0x50` to unique fleet addresses. It demonstrates the two-phase address commit
(v1.3 production-OK) and the required discipline of "one virgin on the bus at a
time".

### Step 2.1 — provision UI1 mains (0x50 → 0x50, sensor_class + ct_model)

```yaml
# Connect ONLY the UI1 to the bus. Flash this YAML.
rbamp:
  - id: provisioning_target
    address: 0x50
    # new_address not needed — 0x50 is already fine for UI1
    sensor_class: SCT_013
    ct_model: SCT_013_030
    fleet_gc_enable: true
    group_id: 1

sensor:
  - platform: rbamp
    rbamp_id: provisioning_target
    voltage:      { name: "Provision UI1 Voltage" }
    current:      { name: "Provision UI1 Current" }

text_sensor:
  - platform: template
    name: "Provision UI1 variant"
    update_interval: 5s
    lambda: 'return id(provisioning_target).get_variant_str();'
  - platform: template
    name: "Provision UI1 UID"
    update_interval: 5s
    lambda: 'return id(provisioning_target).get_uid_hex();'
  - platform: template
    name: "Provision UI1 capability"
    update_interval: 5s
    lambda: 'return id(provisioning_target).get_capability_hex();'
```

**Check**: in HA you should see `sensor.provision_ui1_variant == "UI1"` +
`sensor.provision_ui1_capability` showing a non-zero hex bitmap (`"0x0000"` means
legacy firmware). Record the UID for inventory tracking.

### Step 2.2 — provision I2 (0x50 → 0x51 via two-phase commit)

**Disconnect the UI1 from the bus** (otherwise there will be an address
conflict). Connect the I2 module. Flash the updated YAML:

```yaml
rbamp:
  - id: provisioning_target
    address: 0x50            # virgin address
    new_address: 0x51        # ⚠ two-phase commit — production-OK on v1.3
    sensor_class: SCT_013
    ct_models: [SCT_013_020, SCT_013_010]
    fleet_gc_enable: true
    group_id: 1
```

**What happens at boot**:

```text
[I][rbamp:xxx]: provisioning_target: probe at 0x50 → OK (variant=I2)
[I][rbamp:xxx]: provisioning_target: new_address=0x51 requested
[I][rbamp:xxx]: provisioning_target: two-phase address change supported → production path
[D][rbamp:xxx]: provisioning_target: stage REG_I2C_ADDRESS=0x51
[D][rbamp:xxx]: provisioning_target: write REG_ADDR_COMMIT_MAGIC=0xA5
[D][rbamp:xxx]: provisioning_target: send CMD_COMMIT_ADDR → wait 700ms
[I][rbamp:xxx]: provisioning_target: re-probe 0x51 → OK, re-enumeration complete
[I][rbamp:xxx]: provisioning_target: i2c address handle updated to 0x51
```

**After success**: update the YAML to `address: 0x51` and **remove
`new_address:`**. The next boot will not run the change flow.

### Step 2.3 — provision I3 (0x50 → 0x52) + repeat for the rest

Same pattern as 2.2: disconnect the I2, connect the virgin I3, flash the YAML
with `new_address: 0x52`, wait for re-enumeration, and update the YAML to
`address: 0x52`.

> 🛑 **MUST: one virgin on the bus at a time.** If two virgin modules are both on
> 0x50, both will respond on the address phase and drive SDA against each other →
> indeterminate state, and no provisioning procedure will work.

### Step 2.4 — assemble the fleet

Once provisioning of all modules is complete, update the YAML to the final fleet
config (see scenario §1 above), disable the internal pull-ups on all modules
(3 modules = cut all) and add one external pair of 2.7–3.3 kΩ at the ESP32 — see the pull-up table in [04_hardware.md](04_hardware.md#multi-module-bus-primary-topology).
Flash the final YAML — all modules should be discovered:

```text
[D][i2c:136]: Found i2c device at address 0x50
[D][i2c:136]: Found i2c device at address 0x51
[D][i2c:136]: Found i2c device at address 0x52
```

### Failure modes (provisioning)

| Symptom | Cause | Recovery |
|---|---|---|
| `Probe failed at 0x50` after flash | Module not powered or a broken contact | Check VCC + GND + SDA + SCL connectivity |
| `[W]` two-phase address change not supported | Module on legacy firmware (v1.0-v1.2) | Single-phase fallback requires factory-provisioning mode (see [04_hardware.md](04_hardware.md)) |
| `[E] address change committed but re-probe at 0x51 failed` | Marginal bus or aborted commit | Retry the boot; if it repeats, the external pull-up is too weak or there's an EMI burst |
| `[W] WARN_FACTORY_MODE_REQUIRED` | Legacy firmware without two-phase address change | The supplier enables factory mode (out of scope for the library) |

---

## 3 · Multi-channel mixed-CT (UI3 with different clamps)

**Goal**: to show that the CT model on different channels of a single module can
be mixed according to the load characteristic. The component binds
channels one by one; the order does not matter.

```yaml
rbamp:
  - id: panel_meter
    address: 0x52
    sensor_class: SCT_013
    # Mixed-CT: small/medium/large on channels 0/1/2
    ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
    # The component binds channels one by one; the order does not matter.
    # Each channel keeps its own model: ch0 = 5 A, ch1 = 30 A, ch2 = 20 A
    fleet_gc_enable: true
    group_id: 1

sensor:
  - platform: rbamp
    rbamp_id: panel_meter
    current:    { name: "Panel Lighting (5A CT)" }       # ch0
    current_1:  { name: "Panel Appliances (30A CT)" }    # ch1
    current_2:  { name: "Panel Boiler (20A CT)" }        # ch2
    # No voltage / power — this is an I3 SKU, current-only

text_sensor:
  - platform: template
    name: "Panel last error"
    update_interval: 30s
    lambda: 'return id(panel_meter).get_last_error_str();'
    # → "OK" in a healthy state; "ERR_PARAM" if a CT model code is rejected
```

### Verify boot-time CT binding

After flash + boot, in the log:

```text
[D][rbamp:xxx]: panel_meter: REG_CT_MODEL_CH0 mirror = 1 (SCT_013_005)
[D][rbamp:xxx]: panel_meter: REG_CT_MODEL_CH1 mirror = 3 (SCT_013_030)
[D][rbamp:xxx]: panel_meter: REG_CT_MODEL_CH2 mirror = 6 (SCT_013_020)
[I][rbamp:xxx]: panel_meter: CT binding verified, all mirrors match YAML request
[C][component:246]: Setup panel_meter took ~2100ms (cold install: 3 × 700ms flash + verify)
```

On a warm boot (mirrors match), with no flash write:

```text
[I][rbamp:xxx]: panel_meter: CT_MODEL[0..2] match YAML → SKIP flash erase
[C][component:246]: Setup panel_meter took 72ms (warm boot: read-compare only)
```

### Observed behaviour

- **Mixed CT binding**: codes [1, 3, 6] on a three-channel module at 0x52 are each kept on their own channel.
- **Invalid code rejection**: an attempt at a code outside the class's accepted set
  → `esphome config` rejects the schema (cv.Invalid). If the YAML slips
  past the validator (manual edit / legacy build), the firmware returns
  `REG_ERROR = 0xFE DEV_ERR_PARAM`, with the mirror left at its prior value.
- **Cold install timing**: ~2.1 s on the UI3 (3 × ~700 ms flash erase).
- **Warm boot timing**: **~70 ms** (read-compare-write skips all flash erase).
- **Latency measurement**: ~1.4 s from `setup()` start to first publish (bench).

---

## 4 · Fleet GC sync (tight synchrony)

**Goal**: to show that fleet sync 3/3 is not academic but a real
requirement for multi-tenant deployments (sub-metering apartments in a shared
panel) and for precise sub-metering (UI1 mains must match the sum of the
sub-meter powers to within the duration of a single period).

```yaml
# Fleet config — all modules in one group for synchronized latching:
rbamp:
  - id: tenant1_mains
    address: 0x50
    sensor_class: SCT_013
    ct_model: SCT_013_030
    fleet_gc_enable: true
    group_id: 1

  - id: tenant1_panel
    address: 0x51
    sensor_class: SCT_013
    ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
    fleet_gc_enable: true
    group_id: 1

  - id: tenant2_mains       # Independent tenant (separate group)
    address: 0x52
    sensor_class: SCT_013
    ct_model: SCT_013_030
    fleet_gc_enable: true
    group_id: 2             # ⚠ different group → independent sync timeline

# Native API services for manual sync verification:
api:
  port: 6053
  services:
    - service: emit_gc_tenant1
      then:
        - lambda: 'id(tenant1_mains).transmit_gc_frame();'  # group = tenant1_mains group_id
    - service: emit_gc_tenant2
      then:
        - lambda: 'id(tenant2_mains).transmit_gc_frame();'  # group = tenant2_mains group_id
    - service: check_sync_all
      then:
        - lambda: |-
            uint16_t t1m = id(tenant1_mains).read_gc_tick_received();
            uint16_t t1p = id(tenant1_panel).read_gc_tick_received();
            uint16_t t2m = id(tenant2_mains).read_gc_tick_received();
            ESP_LOGI("rbamp", "GC tick state: tenant1[mains=%u, panel=%u]  tenant2[mains=%u]",
                     t1m, t1p, t2m);

text_sensor:
  - platform: template
    name: "Tenant1 GC tick (mains)"
    update_interval: 30s
    lambda: 'return std::to_string(id(tenant1_mains).read_gc_tick_received());'
  - platform: template
    name: "Tenant1 GC tick (panel)"
    update_interval: 30s
    lambda: 'return std::to_string(id(tenant1_panel).read_gc_tick_received());'
```

### Example output (10-iteration sync test)

```text
[I][rbamp:572]: GC frame emit: group=1, tick=42, all 2 tenant1 modules ACK'd
[I][rbamp:580]: check_sync: tenant1_mains.tick=42  tenant1_panel.tick=42  → 2/2 in_sync
[I][rbamp:572]: GC frame emit: group=2, tick=42, tenant2 module ACK'd
[I][rbamp:580]: check_sync: tenant2_mains.tick=42  → 1/1 in_sync (tenant2 isolated)

ROUND 0: GC tick=1   →  tenant1[1, 1]  tenant2[1]   → 3/3 in_sync
ROUND 1: GC tick=2   →  tenant1[2, 2]  tenant2[2]   → 3/3 in_sync
ROUND 2: GC tick=3   →  tenant1[3, 3]  tenant2[3]   → 3/3 in_sync
...
ROUND 9: GC tick=10  →  tenant1[10,10] tenant2[10]  → 3/3 in_sync

L8 soak: 180 cycles, 0 sync drops, 0 identity drops
```

### How `group_id` helps in multi-tenant

Without `group_id` there is no separation of tenants — all modules react to any
GC frame. With `group_id`:

- Tenant 1's modules respond only to GC frames with `group=1` (or the all-call `group=0`)
- Tenant 2's modules respond only to GC frames with `group=2`

This lets the master emit **separate** GC frames for the tenants at independent
moments — for example, by tenant period:

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
automation:
  - alias: "Tenant 1 period latch"
    trigger:
      platform: time_pattern
      seconds: "/60"            # every 60 s
    action:
      - service: esphome.<node>_emit_gc_tenant1

  - alias: "Tenant 2 period latch"
    trigger:
      platform: time_pattern
      seconds: "/15"            # every 15 s (high-resolution tenant)
    action:
      - service: esphome.<node>_emit_gc_tenant2
```

### Validation in production

- **HA template sensor** "mains_minus_subsum" — a discrepancy indicator:
  ```yaml
  sensor:
    - platform: template
      name: "Mains - SubSum (W)"
      update_interval: 60s
      unit_of_measurement: "W"
      lambda: |-
        // here the sub-meters are current-only, so this is a rough estimate:
        // mains_power - sum(sub_currents × V_mains)
        float v_mains = id(mains_voltage_state).state;
        float sub_i = id(sub_i0).state + id(sub_i1).state + id(sub_i2).state;
        return id(mains_power_state).state - sub_i * v_mains;
  ```
  A discrepancy < 1% of mains_power on the bench = sync correct.

- **Wall-clock integration** — even with GC sync 3/3, BASIC modules are integrated
  against the ESP32's own clock, so a small skew between modules does not distort Wh.

---

## 5 · UI1 — single-channel (minimal)

Source: [`../example/ui1.yaml`](https://github.com/rb-amp/rbamp-esphome)

The simplest useful configuration: one voltage channel, one current channel, and
six derived quantities from them. A starting point for any new deployment —
extend it with more fields as needed.

```yaml
esphome:
  name: rbamp-ui1

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"
    password: !secret ap_password

captive_portal:
api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz    # recommended on ESP32; 100 kHz causes periodic NACKs due to ESP-IDF i2c_master driver interaction;
                      # 50 kHz reduces them ~5-10× — see the chapter end link.
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  ct_model: SCT_013_030    # CT clamp model — change it to match yours

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

**What it demonstrates**: the minimal viable configuration. The six sensors cover
the core case — whole-home metering or a single load. The `energy` sensor already
has `device_class: energy` + `state_class: total_increasing` set, so it goes
straight into the HA Energy dashboard with no extra setup. The period-metering
protocol (latch / 50 ms settle / read average power, integrated on the master
clock) is documented in [09 · API reference](09_api_reference.md).

---

## 6 · UI1 with customization — all the standard ESPHome knobs

**rbAmp is a "good citizen" ESPHome component.** Each slot (`voltage`, `current`,
`power`, …) is an ordinary ESPHome `sensor:` object. The whole standard set of
properties and filters works on it. You need neither a `lambda:` nor custom
templates — everything you do with `pzemac` / `atm90e32` / `cse7766` works here
exactly the same way. The example shows the most common customizations in a
single block:

```yaml
esphome:
  name: rbamp-ui1-tuned

esp32:
  board: esp32dev
  framework:
    type: arduino

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50
  update_interval: 10s        # ↑ Poll once / 10 s instead of the default 60 s —
                              # a denser graph, ~6× the I²C traffic.
  ct_model: SCT_013_030

sensor:
  - platform: rbamp
    rbamp_id: meter1

    # ──────────── Customizing names + precision + filters ────────────
    voltage:
      name: "Mains voltage"              # Any name — it becomes the HA entity_id
      accuracy_decimals: 1               # 226.7 instead of the default 226.70
      filters:
        # Linear calibration against a reference multimeter:
        - calibrate_linear:
            - 0.0 -> 0.0
            - 230.0 -> 230.5
        # Smoothing of sharp spikes (standard ESPHome):
        - exponential_moving_average:
            alpha: 0.3
            send_every: 1

    current:
      name: "Load current"
      accuracy_decimals: 3               # 0.755 instead of 0.75
      filters:
        # Mask the noise floor — drop values below 50 mA (50 mW at 230 V).
        # The equivalent of the `noise_floor` option in pzemac / cse7766.
        - lambda: 'return x < 0.05 ? 0.0f : x;'

    power:
      name: "Power"
      accuracy_decimals: 1
      filters:
        # Throttle publications: no more than once / 30 s, even with update_interval 10 s.
        - throttle: 30s

    energy:
      name: "Energy (Wh)"
      # The standard device_class: energy + state_class: total_increasing
      # are already set by the component — the Energy dashboard works.
      filters:
        # Convert Wh → kWh for a more familiar scale in Lovelace.
        # (The HA Energy dashboard itself expects Wh, but if you want a graph in kWh,
        #  declare a second template sensor with a division rather than changing units here.)
        - multiply: 0.001
      unit_of_measurement: "kWh"         # ← overrides the default "Wh"
      accuracy_decimals: 3

    frequency:
      name: "Mains frequency"
      accuracy_decimals: 2

    power_factor:
      name: "Power factor"
      accuracy_decimals: 3
      # Optional: a filter to hide PF at zero load (P ~ 0)
      filters:
        - lambda: 'return isnan(x) ? 0.0f : x;'
```

**What it demonstrates**:

- **Custom names** (`name:`) become the HA `entity_id` — for example,
  `sensor.rbamp_ui1_tuned_mains_voltage`. Names in any language, including
  Cyrillic, work.
- **Output precision** (`accuracy_decimals:`) is independent of the module; the
  module returns float32 at maximum precision, and ESPHome truncates it for the
  UI.
- **`update_interval:`** does not need to be changed per sensor — it's a property
  of the **component** (`PollingComponent`), set once in the `rbamp:` block. If
  you want rarer publications of individual sensors, add
  `filters: - throttle: <interval>`.
- **`filters:`** — the full ESPHome set:
  - `calibrate_linear` — point calibration against your reference (multimeter,
    reference ammeter).
  - `exponential_moving_average` / `sliding_window_moving_average` — smoothing.
  - `lambda` — an arbitrary transform (noise-floor mask, conditional zero).
  - `multiply` / `offset` — arithmetic (for example, Wh → kWh).
  - `throttle` / `delta` — reducing the publication rate.
- **`unit_of_measurement:`** can be overridden (for example, to `kWh` with
  `multiply: 0.001`). HA picks up the new unit automatically.
- **No `lambda:` for basic operation** — all the RMS, power, frequency, and PF
  calculations are done by the module. The ESP32 only renames, formats, and
  filters.

> ⚠ **Do not override `device_class:` for `energy`** — the HA Energy dashboard
> strictly expects `device_class: energy` + `state_class: total_increasing`. Unit
> conversion via `multiply` + `unit_of_measurement:` keeps compatibility, but
> changing the `device_class` will break the dashboard.

The full list of standard sensor properties and filters is in the
[official ESPHome `Sensor Component` documentation](https://esphome.io/components/sensor/).
That documentation is **fully applicable to rbAmp slots** — there are no
deviations from the standard.

---

## 7 · UI3 — three-channel with mixed clamps

Source: [`../example/ui3.yaml`](https://github.com/rb-amp/rbamp-esphome)

The UI3 SKU has one voltage input and three independent current channels. The
canonical scenario is to put **different** clamp models on different channels: a
small clamp on low-current consumers (for maximum resolution) and a large one on
the main feed (for headroom on peaks).

```yaml
esphome:
  name: rbamp-ui3

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  # drdy_pin: GPIO15
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
  # CH0=5A for standby consumers and LED lighting,
  # CH1=30A for the main household loads,
  # CH2=20A for the boiler / a medium-power branch (~3-5 kW)
  # For currents above 50 A use SCT_013_060 (60 A) or SCT_013_100 (100 A).

sensor:
  - platform: rbamp
    rbamp_id: meter1
    # Common (topology-independent)
    voltage:
      name: "Mains Voltage"
    frequency:
      name: "Mains Frequency"
    apparent_power:
      name: "CH0 Apparent Power"

    # Channel 0 — primary current input (small consumers, 5A)
    current:
      name: "CH0 Current"
    power:
      name: "CH0 Power"
    energy:
      name: "CH0 Energy"
    power_factor:
      name: "CH0 Power Factor"
    reactive_power:
      name: "CH0 Reactive Power"

    # Channel 1 — main loads (30A)
    current_1:
      name: "CH1 Current"
    power_1:
      name: "CH1 Power"
    energy_1:
      name: "CH1 Energy"
    power_factor_1:
      name: "CH1 Power Factor"
    reactive_power_1:
      name: "CH1 Reactive Power"

    # Channel 2 — boiler / medium-power branch (20A)
    current_2:
      name: "CH2 Current"
    power_2:
      name: "CH2 Power"
    energy_2:
      name: "CH2 Energy"
    power_factor_2:
      name: "CH2 Power Factor"
    reactive_power_2:
      name: "CH2 Reactive Power"
```

**What it demonstrates**: all three channels of the UI3 SKU + mixed clamp models
via `ct_models:`. The schema validator checks that `power_N` and `energy_N` (and `power_factor_N`, `reactive_power_N`) require `current_N`
and `voltage` — if you declare `power_1` without `current_1`, compilation fails
with a clear message. Each channel has its own independent NVS-persisted
accumulator `energy_*`. If all channels use the same clamp model, use the global
`ct_model:` instead of `ct_models:`.

---

## 8 · Multi-module bus (three rbAmp at 0x50 / 0x51 / 0x52)

Source: [`../example/multi_module.yaml`](https://github.com/rb-amp/rbamp-esphome)

Three modules on one I²C bus — the house's main feed, the solar generation
output, and EV charging. Each has its own `rbamp:` block (in the component schema,
`MULTI_CONF = True`).

```yaml
esphome:
  name: rbamp-multi

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: INFO

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz    # recommended: at 100 kHz the ESP-IDF i2c_master driver interaction causes periodic
                      # NACKs are almost inevitable on every cycle
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

# Three independent rbamp: blocks — MULTI_CONF = True in the schema.
# On v1.3 firmware the general-call latch is opt-in via fleet_gc_enable +
# group_id (capability-gated on the firmware side). Modules in the same
# group latch in a single transaction, with ~540 µs skew per module. On legacy
# firmware (v1.0..v1.2) the component falls back to a sequential latch
# and logs an informational line. (The old broadcast_latch: key is still
# accepted by the schema but has no effect.)
rbamp:
  - id: meter_house
    address: 0x50
    update_interval: 60s
    fleet_gc_enable: true
    group_id: 1

  - id: meter_solar
    address: 0x51
    update_interval: 60s
    fleet_gc_enable: true
    group_id: 1
    bidirectional: true     # needed for energy_exported_*; on BASIC it
                             # publishes 0, on STANDARD it is real export energy

  - id: meter_evcharger
    address: 0x52
    update_interval: 60s
    fleet_gc_enable: true
    group_id: 1

sensor:
  - platform: rbamp
    rbamp_id: meter_house
    voltage:
      name: "House Voltage"
    current:
      name: "House Current"
    power:
      name: "House Power"
    energy:
      name: "House Energy"
    frequency:
      name: "Mains Frequency"

  - platform: rbamp
    rbamp_id: meter_solar
    voltage:
      name: "Solar Voltage"
      internal: true        # hidden from HA — the same mains as House Voltage
    current:
      name: "Solar Current"
    power:
      name: "Solar Power"
    energy:
      name: "Solar Energy"
    energy_exported:
      name: "Solar Energy Exported"

  - platform: rbamp
    rbamp_id: meter_evcharger
    voltage:
      name: "EV Charger Voltage"
      internal: true
    current:
      name: "EV Charger Current"
    power:
      name: "EV Charger Power"
    energy:
      name: "EV Charger Energy"
```

**What it demonstrates**: the `MULTI_CONF = True` pattern — declare as many
`rbamp:` blocks as you have modules on the bus. Each maintains its own NVS
accumulator, whose key is derived from the I²C address, so the three counters stay
independent across reboots. The `internal: true` flag on the duplicate voltage
sensors hides the three nearly identical mains-voltage entities from HA when all
modules measure the same phase. For the address-provisioning procedure, see
Example 2 below.

> **Semantics of `internal: true`.** The flag does **not disable** the register
> read from the module — the component still reads the voltage from each module
> on every `update_interval` (this is required for the power calculation). The
> flag only hides the sensor from the Home Assistant API: no entity is created,
> it does not appear in Lovelace, and it is not counted in the Energy dashboard.
> If you want to reduce I²C traffic, do not declare the `voltage:` slot under
> `sensor.platform: rbamp` at all — the component then does not query the
> register. If you do declare it, the `internal:` flag is purely cosmetic.

A second constraint concerns address uniqueness on the bus:

> ⚠ **Unique addresses are the configuration's responsibility.** The component
> schema (`MULTI_CONF = True`) does not validate overlapping `address:` values
> between `rbamp:` blocks. If two blocks specify the same `address: 0x50`, both
> declarations are accepted, and on the bus both targets will respond at once —
> expect I²C conflicts, garbled responses, or symptoms resembling a missing
> module. Before committing the YAML, verify that all `address:` values are
> unique (and do not collide with other I²C devices on the bus). After each
> `new_address:` provisioning, update the `rbamp:` section to the new address —
> the old entry is not invalidated automatically.

### Pull-ups on a multi-module bus

All three modules ship with factory 4.7 kΩ pull-ups. On a bus of three modules,
the parallel combination is ~1.6 kΩ — higher current draw and marginal signal
integrity at 50 kHz. Cut the pull-up solder jumpers on all three modules and place
one external pair of 2.7–3.3 kΩ next to the ESP32 (the 3-module row of the table in
[04_hardware.md](04_hardware.md#multi-module-bus-primary-topology)).

### Advanced: decoupled LATCH cadence for multi-module fleets *(v1.4+)*

Fleet installations pay the LATCH-cycle cost per module per cycle: on a 3-module bus polling at `update_interval: 10s`, that's 3× the LATCH write + 3× the 50 ms settle window every 10 s — plus the RT-block reads on top. On ESP8266 hosts (single core; WiFi ISRs compete for CPU during I²C transactions) or shared I²C buses hosting other devices, that overhead becomes visible as WiFi glitches or stale readings on non-rbAmp entities.

`latch_interval:` (available since v1.4, BASIC modules) decouples the two cadences: RT-block reads (V/I/P/PF/Q) stay at `update_interval:` — the reader still sees fresh values on the dashboard — while the LATCH → snapshot → Wh integration only runs every `latch_interval:`. Integration uses the actual wall-clock time between LATCH events, so a 60 s LATCH interval just produces 60 s-wide (and still correct) integration steps. `latch_interval` must be ≥ `update_interval`.

```yaml
rbamp:
  - id: mod_mains
    address: 0x50
    update_interval: 10s     # dashboard: fresh V/I/P/PF every 10 s
    latch_interval: 60s      # bus: LATCH + Wh integration once a minute
    ct_models: [SCT_013_050, SCT_013_030, SCT_013_030]
    fleet_gc_enable: true
    group_id: 1
  - id: mod_branches
    address: 0x51
    update_interval: 10s
    latch_interval: 60s
    ct_models: [SCT_013_005, SCT_013_030, SCT_013_050]
    fleet_gc_enable: true
    group_id: 1
```

Bus-load reduction vs the same fleet without `latch_interval:`: at `update_interval: 10s`, each module contributes ~15 ms of RT read + ~65 ms of LATCH+settle per cycle → ~80 ms per module per cycle. With `latch_interval: 60s`, the LATCH+settle fires only once a minute → RT reads stay cheap every 10 s + one short LATCH burst every 60 s, roughly a 3× lower average bus duty. Larger fleets see proportionally larger savings.

When to omit `latch_interval:`: single-module deployments on ESP32 hosts where the baseline duty isn't costing anything — the v1.3 single-cadence default is simpler and equally correct.

---

## 9 · One-time I²C address change (0x50 → 0x51)

Source: [`../example/address_change.yaml`](https://github.com/rb-amp/rbamp-esphome)

All rbAmp modules ship from the factory at address `0x50`. To put several modules
on one bus, you need to reflash each to its own unique address **before**
connecting them in parallel.

```yaml
esphome:
  name: rbamp-addr-change

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG    # DEBUG shows the address-change lines

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50          # current address — before the change
  new_address: 0x51      # one-time target — DELETE this line after success

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "rbAmp Voltage (post-relocate)"
    current:
      name: "rbAmp Current (post-relocate)"
```

**What it demonstrates**: the one-time address-change flow. Flashing this YAML
makes the component run the procedure once at boot:

1. Probe `0x50`. If `0x50` is silent and `0x51` already responds, the change was
   applied in a previous boot; the component adapts and logs a warning asking you
   to clean up the YAML.
2. Check that the module is ready to accept a provisioning operation.
3. Write the new address to flash (~700 ms), a soft reset (~300 ms), and re-lock
   to the AC cycle.
4. Confirm at the new address. On success — the log `Address change confirmed at
   0x51` and `IMPORTANT: update YAML to address: 0x51 and remove new_address:`.

**Firmware compatibility**: on v1.3+ firmware the address change is **production-OK** — the component uses the two-phase commit protocol (stage → arm → commit → reboot) with no special mode required. On legacy firmware (v1.0-v1.2) the single-phase fallback requires **factory-provisioning mode**; consult the module documentation or your supplier for the procedure to temporarily enable it.

**After success**: edit the YAML to `address: 0x51` and remove the line
`new_address: 0x51`. Flash again. From the next boot, the component uses `0x51`
normally and the change procedure no longer runs.

### Revert checklist after a successful provisioning

So as not to leave a "dangling" provisioning variant in the repository
(two alternatives, never paste both into one file):

```yaml
# BEFORE (provisioning):
rbamp:
  id: meter1
  address: 0x50               # the module's current address
  new_address: 0x51           # the provisioning target
  update_interval: 60s
```

```yaml
# AFTER (production — applied next flash):
rbamp:
  id: meter1
  address: 0x51               # ← replaced with the new address
  # new_address: 0x51         # ← DELETE the line (do not comment it out)
  update_interval: 60s
```

Exactly three edits:

1. Change the `address:` value from the old one to the new one.
2. Delete (do not comment out) the `new_address:` line.
3. Reflash via OTA or USB.

After step 3, the log should show a normal `dump_config` line without the
warning `address change requested but current address matches new_address` —
which means the revert was correct. See also
[10_troubleshooting §4 "OTA right after `new_address:` provisioning does not load"](10_troubleshooting.md#ota-right-after-new_address-provisioning-does-not-load)
if the first OTA after the revert doesn't start.

---

## 10 · Bench config with secrets

Source: [`../example/bench-ui1.yaml`](https://github.com/rb-amp/rbamp-esphome)

The `bench-*.yaml` pattern is for development machines where Wi-Fi credentials are
needed in the config but must not end up in a commit. `example/.gitignore`
excludes `bench-*.yaml` and `secrets.yaml` from version control.

```yaml
esphome:
  name: rbamp-ui1

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"
    password: !secret ap_password

captive_portal:
api:
ota:
  - platform: esphome

web_server:
  port: 80
  version: 3           # optional: a local HTTP UI — handy for bench testing

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  ct_model: SCT_013_030

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

The corresponding `secrets.yaml` (in `example/`, not tracked):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

**What it demonstrates**: separating compile-time credentials from
version-controlled YAML via ESPHome's `!secret`. Listing `bench-*.yaml` in
`.gitignore` lets you keep as many bench variants as you like with no leak risk.
Use `esphome run --device 192.168.0.173 example/bench-ui1.yaml` for OTA after the
first network connection, or `esphome run --device COM6 example/bench-ui1.yaml`
for the first USB flash.

---

## 11 · Brownout and loss of mains

When mains power is interrupted, the module's isolated analog front-end stops
receiving a signal. The voltage register drops to `0.0 V` — a valid IEEE 754
finite float that passes the component's `isfinite()` sanity filter. The
component's filter policy is intentional:

> NO physical lower bounds — brownout (U=0 V on mains disconnect), voltage sag,
> off-grid / UPS test, and breaker-trip MUST pass through to HA, so the user sees
> the real state.

No special YAML is needed to detect a loss of mains — the `voltage` sensor itself
reads `0.0 V`.

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"    # reads 0.0 V when mains is disconnected
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"
```

### HA automation: alert on loss of mains

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
# configuration.yaml or the Automations UI
automation:
  - alias: "rbAmp mains loss alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_voltage
        below: 10          # U < 10 V = mains disconnected or a severe brownout
        for:
          seconds: 5       # debounce: sustained, not transient
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "Mains power lost"
          message: >
            Voltage dropped to {{ states('sensor.rbamp_ui1_mains_voltage') }} V
            at {{ now().strftime('%H:%M:%S') }}.
```

**What it demonstrates**: using the 0 V passthrough behavior as the
source of a "loss of mains" event. Because the component publishes `0.0 V` rather
than skipping the publish, HA automations and the Energy dashboard see the
brownout event in real time. The `for: 5 seconds` condition blocks false
triggers during the ~250 ms boot warm-up (when the registers read 0 before the
first valid measurement).

---

## 12 · HA Energy dashboard

The `energy` sensor (and `energy_1`, `energy_2`, `energy_exported`, etc.) is
automatically configured with `device_class: energy` and
`state_class: total_increasing` — exactly what the HA Energy dashboard expects for
cumulative consumption sensors. No extra YAML is needed.

After discovery:

1. Settings → Dashboards → Energy.
2. Add Consumption → choose "Mains Energy" (or the import-metering entity you
   want).
3. If you have solar — Add Production → choose Solar Energy (from `meter_solar` in
   the multi-module example).

Energy values are in **Wh**. HA converts them to kWh for display itself. The NVS
persistence in the component ensures the accumulator survives an ESP32 reboot
without a false "counter reset" in the HA statistics.

```yaml
# Example: consumption + solar export for the HA Energy dashboard
# (assumes the rbamp: blocks meter_house and meter_solar from the multi-module example)
# energy / energy_exported need voltage AND current in the same block;
# internal: true keeps them out of Home Assistant.
sensor:
  - platform: rbamp
    rbamp_id: meter_house
    voltage:
      name: "House Voltage"
      internal: true
    current:
      name: "House Current"
      internal: true
    energy:
      name: "House Grid Import"    # → HA Energy: grid consumption

  - platform: rbamp
    rbamp_id: meter_solar
    voltage:
      name: "Solar Voltage"
      internal: true
    current:
      name: "Solar Current"
      internal: true
    energy:
      name: "Solar Production"     # → HA Energy: solar production
    energy_exported:
      name: "Solar Grid Export"    # → HA Energy: return to grid
                                   # needs bidirectional: true in rbamp:;
                                   # real values on STANDARD (STD) modules,
                                   # reads 0 on BASIC
```

---

## 13 · HA automations on rbAmp sensors

### Load shedding: a relay turns off when a threshold is exceeded

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
automation:
  - alias: "Load shedding — turn off non-critical loads at 3 kW"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_multi_house_power
        above: 3000        # 3 kW threshold
        for:
          seconds: 10
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.ev_charger_relay   # the non-critical load relay

  - alias: "Load shedding — restore when it drops below 2 kW"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_multi_house_power
        below: 2000
        for:
          seconds: 30
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.ev_charger_relay
```

### Power factor correction reminder

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
automation:
  - alias: "Poor power factor notification"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_power_factor
        below: 0.7
        for:
          minutes: 5
    action:
      - service: persistent_notification.create
        data:
          title: "Low power factor"
          message: >
            Power factor is {{ states('sensor.rbamp_ui1_mains_power_factor') }}.
            Check for large inductive loads (motors, old lighting ballasts).
```

### Daily budget overrun alert

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
automation:
  - alias: "Daily energy budget — warning at 10 kWh"
    trigger:
      - platform: template
        value_template: >
          {{ states('sensor.energy_daily') | float(0) > 10000 }}
    action:
      - service: notify.mobile_app_your_phone
        data:
          message: "Daily energy usage exceeded 10 kWh."
```

---

## 14 · Lovelace cards

### Gauge — live power

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
type: gauge
entity: sensor.rbamp_ui1_mains_power
min: 0
max: 3600
name: "Live Power"
segments:
  - from: 0
    color: green
  - from: 1500
    color: orange
  - from: 2500
    color: red
```

### Apex Charts — power over time

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
type: custom:apexcharts-card
header:
  title: "Power (W)"
  show: true
graph_span: 4h
series:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
    stroke_width: 2
    curve: smooth
```

### Apex Charts — energy comparison across three channels

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
type: custom:apexcharts-card
header:
  title: "Channel Energy (Wh)"
  show: true
graph_span: 24h
series:
  - entity: sensor.rbamp_ui3_ch0_energy
    name: CH0
  - entity: sensor.rbamp_ui3_ch1_energy
    name: CH1
  - entity: sensor.rbamp_ui3_ch2_energy
    name: CH2
```

### Mini graph — voltage

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
type: custom:mini-graph-card
entities:
  - sensor.rbamp_ui1_mains_voltage
name: "Mains Voltage"
hours_to_show: 12
points_per_hour: 4
line_width: 2
```

---

## 15 · Utility meter — aggregation by day / week / month

ESPHome publishes cumulative totals in Wh. The HA `utility_meter` integration
resets the counter at configurable intervals and publishes the delta as a separate
sensor — handy for daily or monthly breakdowns.

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
# configuration.yaml
utility_meter:
  energy_daily:
    source: sensor.rbamp_ui1_mains_energy
    cycle: daily
    name: "Daily Energy"

  energy_monthly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: monthly
    name: "Monthly Energy"

  energy_weekly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: weekly
    name: "Weekly Energy"
```

HA creates three entities: `sensor.energy_daily`, `sensor.energy_monthly`,
`sensor.energy_weekly`. Each resets to 0 at the start of its respective period and
accumulates the delta from the source `total_increasing` sensor.

### Derived kWh/day sensor

`utility_meter` outputs Wh by default. To display in kWh, add a `template`
sensor:

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
template:
  - sensor:
      - name: "Daily Energy kWh"
        unit_of_measurement: "kWh"
        state_class: total_increasing
        device_class: energy
        state: >
          {{ (states('sensor.energy_daily') | float(0) / 1000) | round(3) }}
```

### Daily cost sensor

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
template:
  - sensor:
      - name: "Daily Energy Cost"
        unit_of_measurement: "EUR"
        icon: mdi:currency-eur
        state: >
          {% set kwh = states('sensor.energy_daily') | float(0) / 1000 %}
          {% set rate = 0.28 %}    # EUR/kWh — set your own tariff
          {{ (kwh * rate) | round(2) }}
```

---

## 16 · Per-load sub-metering (3 modules at 0x50 / 0x51 / 0x52)

Installing three rbAmp modules in a distribution panel lets you do per-circuit
energy metering with no smart panel or extra hardware.

*Home Assistant YAML (not ESPHome) — goes into your Home Assistant configuration, not the node's YAML.*

```yaml
# Extending the multi-module example with per-circuit utility meters

utility_meter:
  kitchen_daily:
    source: sensor.rbamp_multi_house_energy
    cycle: daily
    name: "Kitchen Circuit Daily"

  solar_monthly:
    source: sensor.rbamp_multi_solar_energy
    cycle: monthly
    name: "Solar Production Monthly"

  ev_daily:
    source: sensor.rbamp_multi_ev_charger_energy
    cycle: daily
    name: "EV Charging Daily"
```

Combined with the HA Energy dashboard, you get monthly statements per circuit with
no extra kWh×tariff calculations.

---

## 17 · rbAmp + a relay — closed-loop control

### A boiler with relay control

```yaml
# ESPHome YAML on the same ESP32 as rbAmp
switch:
  - platform: gpio
    pin: GPIO5
    id: heater_relay
    name: "Water Heater"
```

With rbAmp and a relay on the same ESP32, you can build a closed loop right in
ESPHome (via a Lambda or `on_value`) with no round trip through HA:

*Assumes you already have a `switch:` entry with the id used below.*

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    power:
      name: "House Power"
      on_value:
        then:
          - if:
              condition:
                lambda: 'return x > 3000.0f;'
              then:
                - switch.turn_off: heater_relay
              else:
                - switch.turn_on: heater_relay
```

**What it demonstrates**: using a rbAmp sensor's `on_value` callback to control a
relay without HA involvement. The callback fires on the ESP32 every
`update_interval` (60 s) and updates the relay state from the latest power
reading. For tighter control (a < 60 s loop), reduce `update_interval` to `10s` —
this increases the load on the I²C bus proportionally. A more complete closed-loop
example for a boiler is in [07_diy_integrations.md](07_diy_integrations.md).

---

## 18 · DRDY integration

The module's open-drain DRDY pin gives a LOW pulse of ~10 µs every ~100 ms after
the instantaneous registers are updated. You can wire it to an ESP32 GPIO.
The component logs the DRDY pin; reads are timer-driven (`update_interval`).

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s        # energy integration still happens every 60 s
  drdy_pin:
    number: GPIO15
    mode:
      input: true
      pullup: true            # DRDY is open-drain; it needs a pull-up
    inverted: true            # the DRDY pulse is LOW; inverted=true gives a rising edge
```

> **Note**: on the current firmware the component logs `drdy_pin` in
> `dump_config` but does not implement interrupt-driven RT reads — the
> `update_interval` still sets the polling rate.

---

## 19 · Phased sensors (split-phase / three-phase variants)

For modules with more than one voltage input the component offers a phased
sensor group (`voltage_a/b/c`, `current_a/b/c`, `power_a/b/c`, `energy_a/b/c`,
`power_factor_a/b/c`, `reactive_power_a/b/c`, `power_total`, plus
`voltage_angle_b/c`). A slot publishes only when the connected module reports
the corresponding channel; on a single-voltage module the phased slots stay
without a value. The schema checks that single and phased groups are not mixed
in one `sensor:` block.

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
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
    power_a:
      name: "Phase A Power"
    power_b:
      name: "Phase B Power"
    power_c:
      name: "Phase C Power"
    power_total:
      name: "Total Power"
    voltage_angle_b:
      name: "Phase B Angle"        # degrees vs phase A
    voltage_angle_c:
      name: "Phase C Angle"
    phase_sequence:
      name: "Phase Sequence"       # 1 forward, 2 reverse, 3 abnormal
    frequency:
      name: "Grid Frequency"
```

**What it demonstrates**: the phased field group. Mixing single and phased fields
in one `sensor:` block produces a compile-time error — choose the group that
matches your module. Details are in [09 · API reference](09_api_reference.md) §2.2.

---

## 20 · Migration from other ESPHome metering components

### From `pzem004t`

The `pzem004t` platform exports the same field names as `rbamp` for the common AC
quantities. The swap is block-for-block with identical sensor names:

```yaml
# Before (pzem004t, UART)
uart:
  rx_pin: GPIO16
  tx_pin: GPIO17
  baud_rate: 9600

sensor:
  - platform: pzem004t
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

```yaml
# After (rbamp, I²C) — the same sensor names → the same entity IDs in HA
# (replaces the block above; do not paste both)
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz

rbamp:
  id: meter1
  address: 0x50
  ct_model: SCT_013_030

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

Identical `name:` fields give identical entity IDs in HA. The Energy dashboard
history is preserved across the swap: the `total_increasing` sensor picks up where
it left off. Remove the `uart:` block if it isn't shared with another component.

### From `cse7766` (Sonoff POW R2)

Same pattern. `cse7766` provides `voltage`, `current`, `power`, `energy`,
`power_factor`, `apparent_power` — all present in `rbamp`. Replace
`platform: cse7766` with `platform: rbamp`, add `rbamp_id: meter1`, remove
`uart:`, and add `i2c:` and the `rbamp:` block.

---

## 21 · STANDARD module — on-device energy totals

On a STANDARD module the component accumulates the module's own energy counters.
Totals survive restarts of both the ESP32 and the module and stay monotonic. The
same YAML works on a BASIC module too, except that the grand-total keys then
never publish.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 30s
  bidirectional: true          # per-channel energy_exported
  start_current_ua: 400        # no energy counted below 400 µA per channel

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy_total:
      name: "Energy Imported"          # all channels, total_increasing
    energy_exported_total:
      name: "Energy Exported"          # all channels, total_increasing
    energy_net:
      name: "Energy Net"               # import - export, may decrease
    reactive_energy:
      name: "Reactive Energy Imported" # varh
    reactive_energy_exported:
      name: "Reactive Energy Exported"
```

**What it demonstrates**: the grand-total sensors, which need no companion
`current`/`voltage` declaration; `start_current_ua:` (`0` = off, otherwise
`10`..`2540` in steps of `10`) so that noise at no load does not creep into the
import and export counters (instantaneous readings are unaffected). For the HA
Energy dashboard use `energy_total` as grid consumption and
`energy_exported_total` as return to grid.

---

## 22 · Switching the SCT rating and service override

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_010      # module was delivered with the SCT-013-030
```

Readings stay accurate after the switch: on module firmware up to 1.4.19 the
component completes the model write with the required module restart (visible in
the log) and scales current, power and energy to the selected sensor; newer
firmware does it on the module. A value the module already holds is never
rewritten, and on a module supplied calibrated together with its own fitted
sensor (`sensor_class: WIRED_CT` / `BUILTIN_CT`) the change is refused with a log
line. Only for a deliberate preset re-apply:

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_010
  ct_model_force: true       # one-off service option; remove both lines afterwards
```

---

## YAML validation rules summary

| Rule | Enforced by |
|---|---|
| `power` / `energy` / `power_factor` / `reactive_power` require `current` | Schema validator (`_SINGLE_SLOT_DEPS`) |
| P/Q/PF/E for any channel require `voltage` | Schema validator |
| Single and phased groups are not mixed in one `sensor:` | `_validate_topology_consistency` |
| `new_address` ≠ `address` | `_validate_new_address` |
| `address` in the range `0x08..0x77` | `cv.i2c_address` |
| `update_interval` minimum `1s`, enforced | Schema validator |
| `latch_interval` ≥ `update_interval` | Schema validator |
| `ct_model:` and `ct_models:` are mutually exclusive | Schema validator |
| `ct_models:` has 1..16 entries | Schema validator |
| `ct_model_force: true` requires `ct_model:` or `ct_models:` | Schema validator |
| `start_current_ua` is `0` or `10`..`2540` in steps of `10` | Schema validator |
| CT model code is valid for the pinned `sensor_class` | Schema validator |

Validation errors are reported at `esphome compile` time with a human-readable
message pointing to the offending key. You don't need to flash hardware to find a
configuration error.

