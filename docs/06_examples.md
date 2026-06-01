# 06 · YAML cookbook

This chapter is a complete set of working YAML configurations for the rbAmp ESPHome component. Every example is self-contained: copy it, fill in your board name and Wi-Fi credentials, compile, flash. Where an example deviates from the minimal template, the deviation is explained.

Cross-references:

- Component schema: [`09_api_reference.md`](09_api_reference.md)
- Period metering protocol: [SPEC.md §7](https://rbamp.com/docs/modules-basic-standard-api-reference)
- Multi-module topology: [SPEC.md §8](https://rbamp.com/docs/modules-basic-standard-api-reference)
- Broadcast LATCH: [SPEC.md §9](https://rbamp.com/docs/modules-basic-standard-api-reference)
- NACK loose-sanity rule: [SPEC.md §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)

---

## 1 · UI1 — single channel (minimal)

Source: [`example/ui1.yaml`](../example/ui1.yaml)

The simplest useful configuration: one voltage channel, one current channel, six derived quantities. Use this as the starting point for any new deployment and add fields as needed.

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
  frequency: 50kHz    # 100 kHz causes periodic NACKs from the module;
                      # 50 kHz reduces them by ~5-10x (see SPEC §B.5).
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
  ct_model: SCT_013_030    # CT clamp model — change to match yours

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

**What this demonstrates**: a minimum viable configuration. Six sensors cover the basic case — whole-house metering or a single load. The `energy` sensor already has `device_class: energy` + `state_class: total_increasing` set, so it lands in the HA Energy dashboard out of the box without extra configuration. For details on the period-metering protocol, see [SPEC §7](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 1.1 · UI1 with customization — all standard ESPHome knobs

**rbAmp is a "good citizen" of ESPHome.** Every slot (`voltage`, `current`, `power`, ...) is a regular ESPHome `sensor:` object. The full standard set of properties and filters works on it. No `lambda:`, no custom templates — everything you do with `pzemac` / `atm90e32` / `cse7766` works here in exactly the same way. The example below shows the most common customizations in a single block:

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
  update_interval: 10s        # ↑ Poll once per 10 s instead of the default 60 s —
                              # denser graph, ~6x I²C traffic.
  ct_model: SCT_013_030

sensor:
  - platform: rbamp
    rbamp_id: meter1

    # ──────────── Customizing names + precision + filters ────────────
    voltage:
      name: "Mains Voltage"              # Any name — becomes the HA entity_id
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
      name: "Load Current"
      accuracy_decimals: 3               # 0.755 instead of 0.75
      filters:
        # Mask noise floor — drop values below 50 mA (50 mW at 230 V).
        # Equivalent of the `noise_floor` option in pzemac / cse7766.
        - lambda: 'return x < 0.05 ? 0.0f : x;'

    power:
      name: "Power"
      accuracy_decimals: 1
      filters:
        # Throttle publishes: at most once per 30 s, even if update_interval is 10 s.
        - throttle: 30s

    energy:
      name: "Energy (Wh)"
      # The standard device_class: energy + state_class: total_increasing
      # are already set by the component — the Energy dashboard works.
      filters:
        # Convert Wh → kWh for a more familiar scale in Lovelace.
        # (The HA Energy dashboard itself expects Wh, but if you want a kWh
        #  graph — declare a second template sensor with division rather
        #  than changing units here.)
        - multiply: 0.001
      unit_of_measurement: "kWh"         # ← overrides the default "Wh"
      accuracy_decimals: 3

    frequency:
      name: "Mains Frequency"
      accuracy_decimals: 2

    power_factor:
      name: "Power Factor"
      accuracy_decimals: 3
      # Optional: filter to hide PF at zero load (P ~ 0)
      filters:
        - lambda: 'return isnan(x) ? 0.0f : x;'
```

**What this demonstrates**:

- **Custom names** (`name:`) become HA `entity_id`s — `sensor.rbamp_ui1_tuned_mains_voltage`. Names in any language, including Cyrillic, work.
- **Output precision** (`accuracy_decimals:`) — independent of the module; the module returns float32 at maximum precision and ESPHome truncates for the UI.
- **`update_interval:`** does not need to be changed per sensor — it is a property of the **component** (`PollingComponent`), set once in the `rbamp:` block. If you want rare publishes from individual sensors, add `filters: - throttle: <interval>`.
- **`filters:`** — the full ESPHome set:
  - `calibrate_linear` — point calibration against your reference (multimeter, reference ammeter).
  - `exponential_moving_average` / `sliding_window_moving_average` — smoothing.
  - `lambda` — arbitrary transformation (noise-floor mask, conditional zero).
  - `multiply` / `offset` — arithmetic (for example, Wh → kWh).
  - `throttle` / `delta` — reducing publish frequency.
- **`unit_of_measurement:`** can be overridden (for example, to `kWh` with `multiply: 0.001`). HA picks up the new unit automatically.
- **No `lambda:` for basic operation** — all RMS, power, frequency, and PF computations are done by the module. The ESP32 only renames, formats, and filters.

> ⚠ **Do not override `device_class:` for `energy`** — the HA Energy dashboard strictly expects `device_class: energy` + `state_class: total_increasing`. Unit conversion via `multiply` + `unit_of_measurement:` preserves compatibility, but changing `device_class` will break the dashboard.

The full list of standard sensor properties and filters is in the [official ESPHome `Sensor Component` documentation](https://esphome.io/components/sensor/). That documentation **applies in full to rbAmp slots** — no deviations from the standard.

---

## 2 · UI3 — three channels with mixed clamps

Source: [`example/ui3.yaml`](../example/ui3.yaml)

The UI3 SKU has one voltage input and three independent current channels. The canonical scenario is to use **different** clamp models on different channels: a small clamp for low-current loads (for maximum resolution), and a large one on the main feed (for headroom on peaks).

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
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  # CH0=5A for standby loads and LED lighting,
  # CH1=30A for the main household loads,
  # CH2=100A for the main feed or EV charging

sensor:
  - platform: rbamp
    rbamp_id: meter1
    # Shared (topology-independent)
    voltage:
      name: "Mains Voltage"
    frequency:
      name: "Mains Frequency"
    apparent_power:
      name: "CH0 Apparent Power"

    # Channel 0 — primary current input (small loads, 5A)
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

    # Channel 2 — main feed / EV (100A)
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

**What this demonstrates**: all three UI3 SKU channels plus mixed clamp models via `ct_models:`. The schema validator checks that `power_N` requires `current_N` and `voltage` — if you declare `power_1` without `current_1`, compilation fails with a clear message. Each channel has its own independent NVS-persisted `energy_*` accumulator. If all channels use the same clamp model, use the global `ct_model:` instead of `ct_models:`.

---

## 3 · Multi-module bus (three rbAmp on 0x50 / 0x51 / 0x52)

Source: [`example/multi_module.yaml`](../example/multi_module.yaml)

Three modules on a single I²C bus — the house main feed, the solar generation output, the EV charger. Each gets its own `rbamp:` block (the component schema sets `MULTI_CONF = True`).

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
  frequency: 50kHz    # mandatory: at 100 kHz with three modules, periodic
                      # NACKs are almost inevitable in every cycle
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

# Three independent rbamp: blocks — MULTI_CONF = True in the schema.
# broadcast_latch: true is declared here for forward compatibility: on
# current firmware the component logs a warning at load (the broadcast
# address is disabled) and falls back to sequential latch for each
# module. The warning will disappear automatically once firmware adds
# broadcast support — the YAML will not need to change. See SPEC §9.
rbamp:
  - id: meter_house
    address: 0x50
    update_interval: 60s
    broadcast_latch: true

  - id: meter_solar
    address: 0x51
    update_interval: 60s
    broadcast_latch: true
    bidirectional: true     # STANDARD / PRO tiers; energy_exported_*
                             # publishes 0 until the export accumulator
                             # appears in firmware

  - id: meter_evcharger
    address: 0x52
    update_interval: 60s
    broadcast_latch: true

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
      internal: true        # hidden from HA — same mains as House Voltage
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

**What this demonstrates**: the `MULTI_CONF = True` pattern — declare as many `rbamp:` blocks as you have modules on the bus. Each maintains its own NVS accumulator, the key of which is derived from the I²C address, so the three counters remain independent across reboots. The `internal: true` flag on duplicate voltage sensors hides three nearly identical mains-voltage entities from HA when all modules measure the same phase. For the address provisioning procedure, see Example 4 below.

> **Semantics of `internal: true`.** The flag **does not disable**
> reading the register from the module — the component still reads
> voltage from every module every `update_interval` (this is required
> to compute power). The flag only hides the sensor from the Home
> Assistant API: the entity is not created, does not appear in
> Lovelace, and is not counted in the Energy dashboard.
> If you want to reduce I²C traffic, do not declare the `voltage:`
> slot under `sensor.platform: rbamp` at all — the component then
> stops requesting the register. Once declared, the `internal:`
> flag is cosmetic only.

The second constraint concerns address uniqueness on the bus:

> ⚠ **Unique addresses are the configuration's responsibility.** The
> component schema (`MULTI_CONF = True`) does not validate `address:`
> collisions across `rbamp:` blocks. If two blocks specify the same
> `address: 0x50`, both declarations are accepted, and on the bus both
> targets will respond simultaneously — expect I²C conflicts, garbled
> responses, or symptoms resembling a missing module. Before committing
> YAML, verify that all `address:` values are unique (and do not clash
> with other I²C devices on the bus). After every `new_address:`
> provisioning, update the `rbamp:` block to the new address — the old
> entry is not invalidated automatically.

### Pull-ups on a multi-module bus

All three modules ship from the factory with 4.7 kΩ pull-ups. On a three-module bus the parallel combination drops to ~1.6 kΩ — increased current draw and marginal signal integrity at 50 kHz. Cut the pull-up solder jumpers on modules #2 and #3 (leave only #1, or install a single external 4.7 kΩ pair near the ESP32). For details, see [04_hardware.md](04_hardware.md), the multi-module bus section.

---

## 4 · One-time I²C address change (0x50 → 0x51)

Source: [`example/address_change.yaml`](../example/address_change.yaml)

All rbAmp modules ship from the factory at address `0x50`. To put several modules on the same bus, each must be reflashed to its own unique address **before** they are connected in parallel.

```yaml
esphome:
  name: rbamp-addr-change

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG    # DEBUG shows the address-change log lines

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
  new_address: 0x51      # one-shot target — REMOVE this line after success

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "rbAmp Voltage (post-relocate)"
    current:
      name: "rbAmp Current (post-relocate)"
```

**What this demonstrates**: the one-shot address-change flow. Flashing this YAML makes the component run the following procedure once at boot:

1. Probe `0x50`. If `0x50` is silent and `0x51` is already responding — the change was applied in a previous boot, the component adapts and logs a warning asking you to clean up the YAML.
2. Verify the module is ready to accept a provisioning operation.
3. Write the new address to flash (~700 ms), perform a soft reset (~300 ms), re-lock onto the AC cycle.
4. Confirm on the new address. On success — log lines `Address change confirmed at 0x51` and `IMPORTANT: update YAML to address: 0x51 and remove new_address:`.

**Requirement**: the module must be in **factory-provisioning mode** — a mode for one-shot provisioning operations. Standard production modules ship with provisioning mode disabled; to change the address in the field, consult the module documentation or supplier for the procedure to enable it temporarily.

**After success**: edit the YAML to `address: 0x51` and remove the `new_address: 0x51` line. Flash again. From the next boot, the component will use `0x51` normally and the change procedure will not run again.

### Revert checklist after successful provisioning

To avoid leaving a "dangling" provisioning variant in the repository:

```yaml
# BEFORE (provisioning):
rbamp:
  id: meter1
  address: 0x50               # current module address
  new_address: 0x51           # provisioning target
  update_interval: 60s

# AFTER (production — applied next flash):
rbamp:
  id: meter1
  address: 0x51               # ← replaced with the new address
  # new_address: 0x51         # ← REMOVE the line (do not comment it out)
  update_interval: 60s
```

Exactly three edits:

1. Change the value of `address:` from the old to the new one.
2. Remove (do not comment out) the `new_address:` line.
3. Reflash via OTA or USB.

After step 3, the log should show a normal `dump_config` line without the warning `address change requested but current address matches new_address` — that means the revert went through correctly. See also [10_troubleshooting §4 "OTA right after `new_address:` provisioning does not load"](10_troubleshooting.md#ota-right-after-new_address-provisioning-does-not-load) if the first OTA after the revert refuses to start.

---

## 5 · Bench configuration with secrets

Source: `example/bench-ui1.yaml`

The `bench-*.yaml` pattern is for development machines where Wi-Fi credentials need to be present in the config but must not be committed. `example/.gitignore` excludes `bench-*.yaml` and `secrets.yaml` from version control.

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
  version: 3           # optional: local HTTP UI — handy for bench testing

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

The matching `secrets.yaml` (in `example/`, not tracked):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

**What this demonstrates**: separating compile-time credentials from version-controlled YAML using ESPHome's `!secret`. Listing `bench-*.yaml` in `.gitignore` lets you keep as many bench variants as you want without risking leaks. Use `esphome run --device 192.168.0.173 example/bench-ui1.yaml` for OTA after the first network attach, or `esphome run --device COM6 example/bench-ui1.yaml` for the initial USB flash.

---

## 6 · Brownout and loss of mains

When mains power is cut, the module's isolated analog front-end stops receiving signal. The voltage register drops to `0.0 V` — a valid IEEE 754 finite float that passes the component's `isfinite()` sanity filter. From SPEC §B.5:

> NO physical lower bounds — brownout (U=0 V on mains disconnect), voltage sag, off-grid / UPS test, and breaker-trip MUST pass through to HA so the user can see the real state.

No special YAML is required to detect a mains loss — the `voltage` sensor itself will read `0.0 V`.

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

### HA automation: mains loss alert

```yaml
# configuration.yaml or Automations UI
automation:
  - alias: "rbAmp mains loss alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_voltage
        below: 10          # U < 10 V = mains disconnected or severe brownout
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

**What this demonstrates**: using the 0 V passthrough behavior (SPEC §B.5) as a "mains loss" event source. Because the component publishes `0.0 V` instead of suppressing the publish, HA automations and the Energy dashboard see the brownout event in real time. The `for: 5 seconds` condition blocks false positives during the ~250 ms boot warm-up (when registers read 0 before the first valid measurement).

---

## 7 · HA Energy dashboard

The `energy` sensor (and `energy_1`, `energy_2`, `energy_exported`, etc.) is automatically configured with `device_class: energy` and `state_class: total_increasing` — exactly what the HA Energy dashboard expects for cumulative consumption sensors. No extra YAML is needed.

After discovery:

1. Settings → Dashboards → Energy.
2. Add Consumption → pick "Mains Energy" (or the entity tracking your import).
3. If you have solar — Add Production → pick Solar Energy (from `meter_solar` in the multi-module example).

Energy values are in **Wh**. HA converts to kWh for display itself. The component's NVS persistence guarantees that the accumulator survives ESP32 reboots without a spurious "counter reset" in HA statistics.

```yaml
# Example: consumption + solar export for the HA Energy dashboard
sensor:
  - platform: rbamp
    rbamp_id: meter_house
    energy:
      name: "House Grid Import"    # → HA Energy: grid consumption

  - platform: rbamp
    rbamp_id: meter_solar
    energy:
      name: "Solar Production"     # → HA Energy: solar production
    energy_exported:
      name: "Solar Grid Export"    # → HA Energy: return to grid
                                   # reads 0 until the export accumulator
                                   # in STANDARD/PRO firmware ships
```

---

## 8 · HA automations on rbAmp sensors

### Load shedding: relay opens when threshold is exceeded

```yaml
automation:
  - alias: "Load shedding — drop non-critical loads at 3 kW"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_multi_house_power
        above: 3000        # 3 kW threshold
        for:
          seconds: 10
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.ev_charger_relay   # non-critical load relay

  - alias: "Load shedding — restore when below 2 kW"
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

### Power-factor correction reminder

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

### Daily budget exceeded alert

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

## 9 · Lovelace cards

### Gauge — live power

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

## 10 · Utility meter — aggregation by day / week / month

ESPHome publishes cumulative totals in Wh. The HA `utility_meter` integration resets the counter at configurable intervals and publishes the delta as a separate sensor — useful for daily or monthly billing cuts.

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

HA creates three entities: `sensor.energy_daily`, `sensor.energy_monthly`, `sensor.energy_weekly`. Each resets to 0 at the start of the corresponding period and accumulates the delta from the source `total_increasing` sensor.

### Derived kWh/day sensor

`utility_meter` outputs Wh by default. To display in kWh, add a `template` sensor:

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

## 11 · Per-load sub-metering (3 modules on 0x50 / 0x51 / 0x52)

Installing three rbAmp modules in a distribution panel enables per-circuit energy metering without a smart panel or extra hardware.

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

Combined with the HA Energy dashboard, you get per-circuit monthly statements without extra kWh × tariff calculations.

---

## 12 · rbAmp + relay — closed-loop control

### Water heater with relay control

```yaml
# ESPHome YAML on the same ESP32 as rbAmp
switch:
  - platform: gpio
    pin: GPIO5
    id: heater_relay
    name: "Water Heater"
```

With rbAmp and the relay on the same ESP32, you can build a closed loop directly in ESPHome (through Lambda or `on_value`) without round-tripping through HA:

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

**What this demonstrates**: using the rbAmp sensor's `on_value` callback to drive a relay without HA involvement. The callback fires on the ESP32 every `update_interval` (60 s) and updates the relay state from the latest power reading. For tighter control (< 60 s loop), drop `update_interval` to `10s` — this proportionally increases I²C bus load. A more complete closed-loop example for a water heater is in [07_diy_integrations.md](07_diy_integrations.md).

---

## 13 · DRDY integration

The module's open-drain DRDY pin produces a ~10 µs LOW pulse every ~200 ms after the instantaneous registers update. You can wire it to an ESP32 GPIO and use it as an interrupt hint instead of polling on a fixed timer.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s        # energy integration still runs every 60 s
  drdy_pin:
    number: GPIO15
    mode:
      input: true
      pullup: true            # DRDY is open-drain; a pull-up is required
    inverted: true            # DRDY pulses LOW; inverted=true yields a rising edge
```

> **Note**: in current firmware the component logs `drdy_pin` in `dump_config` but does not yet implement interrupt-driven RT reads — `update_interval` still drives the poll rate. The pin can be wired up now in anticipation of future firmware revisions without code changes.

---

## 14 · Preparing for future split-phase / three-phase SKUs

The component schema already accepts phased sensor fields (`voltage_a/b/c`, `current_a/b/c`, `power_a/b/c`, `energy_a/b/c`, `power_factor_a/b/c`, `reactive_power_a/b/c`, `power_total`). They are reserved for the upcoming rbAmp-U2I2 (split-phase US) and rbAmp-U3I3 (three-phase Europe) SKUs.

If you are preparing YAML for a future three-phase deployment today, you can describe the full phased block and validate it now. On current firmware it compiles cleanly (there are just no real registers to read). The schema validator checks that single and phased groups are not mixed in the same `sensor:` block.

```yaml
# Forward-compat three-phase config — compiles today, sensor data will
# become available when rbAmp-U3I3 firmware ships. DO NOT USE on
# current single-channel UI* SKUs — readings will be 0.
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
    energy_a:
      name: "Phase A Energy"
    energy_b:
      name: "Phase B Energy"
    energy_c:
      name: "Phase C Energy"
    frequency:
      name: "Grid Frequency"
```

**What this demonstrates**: the phased field group. Mixing single and phased fields in the same `sensor:` block produces a compile-time error — pick the group that matches your hardware SKU. The topology definition is in [SPEC §8](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 15 · Migrating from other ESPHome metering components

### From `pzem004t`

The `pzem004t` platform exports the same field names as `rbamp` for the common AC quantities. The replacement is block-level with identical sensor names:

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

# After (rbamp, I²C) — same sensor names → same entity IDs in HA
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

Identical `name:` fields yield identical entity IDs in HA. Energy dashboard history survives the swap: the `total_increasing` sensor picks up where it left off. Remove the `uart:` block if it is not shared with another component.

### From `cse7766` (Sonoff POW R2)

Same pattern. `cse7766` provides `voltage`, `current`, `power`, `energy`, `power_factor`, `apparent_power` — all of which exist in `rbamp`. Replace `platform: cse7766` with `platform: rbamp`, add `rbamp_id: meter1`, remove `uart:`, and add an `i2c:` and `rbamp:` block.

---

## YAML validation rules summary

| Rule | Where enforced |
|---|---|
| `power` / `energy` / `power_factor` / `reactive_power` require `current` | Schema validator (`_SINGLE_SLOT_DEPS`) |
| P/Q/PF/E for any channel require `voltage` | Schema validator |
| Single and phased groups cannot be mixed in one `sensor:` | `_validate_topology_consistency` |
| `new_address` ≠ `address` | `_validate_new_address` |
| `address` within `0x08..0x77` | `cv.i2c_address` |
| `update_interval` minimum: not enforced; practical — `10s` | `cv.polling_component_schema` |
| `ct_model:` and `ct_models:` are mutually exclusive | Schema validator |

Validation errors are reported at `esphome compile` time with a human-readable message pointing at the offending key. You do not need to flash hardware to find a configuration mistake.



---

← [Quickstart](05_quickstart.md) · [Docs index](README.md) · [DIY Integrations](07_diy_integrations.md) →
