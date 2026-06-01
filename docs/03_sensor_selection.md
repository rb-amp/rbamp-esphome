# 03 · Current Sensor Selection

rbAmp measures current through an external CT clamp (current transformer) and voltage through a built-in voltage transformer (on UI* SKUs). This chapter covers how to choose a clamp for your load and how to tell the module which model is installed via YAML.

## Sensor family (`sensor_class:`)

The hardware revision of the module determines which current-sensor family is supported. Current SKUs ship with one option:

| Class | YAML value | Status |
|---|---|---|
| SCT-013 (split-core CT) | `SCT_013` | Available now, default |
| WIRED_CT (wired CT) | `WIRED_CT` | Reserved for future SKUs |
| BUILTIN_CT (built-in shunt) | `BUILTIN_CT` | Reserved for future SKUs |

The class is set once and stored in the module's flash:

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013   # defaults to SCT_013, may be omitted
```

On firmware v1.2+ this value becomes a precondition for writing the CT model: the module rejects the write if the class is not set. On earlier firmware the value is accepted by the schema and applied automatically on upgrade.

> **What "reserved" means** for `WIRED_CT` and `BUILTIN_CT` — the component
> schema accepts both values today (the YAML validates, no `cv.Invalid`
> is raised). However, **no current rbAmp hardware revision physically
> supports them**: the module will accept the class write into the
> register, but without the matching front-end (`WIRED_CT` — a copper
> resistive shunt on the current-carrying bus; `BUILTIN_CT` — an
> on-board toroidal CT) the readings will not correspond to reality.
>
> For production deployments today, **choose `SCT_013` only** (or omit
> the key — it is the default). Declaring `WIRED_CT` / `BUILTIN_CT`
> without the matching hardware is an anti-pattern that the component
> cannot detect and will not warn about.
>
> These classes will enter production documentation after the
> rbAmp-WiredCT / rbAmp-BuiltinCT SKUs ship — track [11_changelog.md](11_changelog.md).

## Choosing an SCT-013 model

The module accepts any of the five models in the SCT-013 series. Their electrical outputs differ, so the module needs to know which one is installed — factory coefficients are loaded for the specific model.

| Model | Rated current | Typical load | Max at 230 V |
|---|---|---|---|
| SCT-013-005 | 5 A | LED lighting, individual appliances, monitors, laptop power supplies | ~1.15 kW |
| SCT-013-010 | 10 A | Desktop PC, small air conditioner, dedicated ring circuit | ~2.3 kW |
| SCT-013-030 | 30 A | Whole-house metering at typical European consumption | ~6.9 kW |
| SCT-013-050 | 50 A | EV charging, large air conditioner, induction cooktop | ~11.5 kW |
| SCT-013-100 | 100 A | Three-phase panels, commercial distribution, fast EV charging | ~23 kW |

The burden resistor is already fitted on the module PCB — no external resistors between the clamp output leads are required.

### Selection tree

```
  Peak continuous current through the conductor:
  |
  +-- <= 5 A    (LED, individual appliances, <= 1.15 kW)
  |       -> SCT-013-005
  |
  +-- 5..10 A   (PC, small loads, <= 2.3 kW)
  |       -> SCT-013-010
  |
  +-- 10..30 A  (whole-house UK/EU, <= 6.9 kW)
  |       -> SCT-013-030
  |
  +-- 30..50 A  (EV, large air conditioner, <= 11.5 kW)
  |       -> SCT-013-050
  |
  +-- > 50 A    (panels, commercial)
          -> SCT-013-100
```

Rule of thumb: pick the lowest-rated clamp whose maximum measured value covers the peak load with about 30% headroom. An oversized clamp on a small load reduces signal amplitude relative to noise — accuracy drops. A clamp rated below the peak saturates the core and produces grossly incorrect readings.

## How to tell the module

There are two keys in YAML — pick one of them depending on whether you have the same clamp on every channel or different clamps. The syntactic shift is from a scalar value to a YAML list (array):

### Step 1 — one clamp for every channel (`ct_model:`, scalar)

If you have UI1 (one current channel) or UI3 with identical clamps on all three channels, one line is enough:

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_030      # ← scalar value, one model for every channel
```

This is the most common case. Most deployments start with a single clamp model and never move past this line.

### Step 2 — different clamps on different channels (`ct_models:`, list)

If you have UI2 / UI3 and want to fit different clamps on different channels (for example a small clamp on a standby line and a larger one on the main feed), replace `ct_model:` with `ct_models:` (with a trailing `s`) and use a YAML list in square brackets:

```yaml
rbamp:
  id: meter_ui3
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  #         ↑                            ↑                           ↑
  #     channel 0                    channel 1                  channel 2
  #
  # This is a standard YAML list (1..3 elements).
  # Order matters: position in the list = channel index.
```

This is the spot people most often trip over when modifying multi-channel setups. Remember: `ct_model:` (no `s`) is a scalar value for a single model; `ct_models:` (with `s`) is a list of values in square brackets, one per channel. The list length must match the number of active module channels (1 for UI1, 2 for UI2, 3 for UI3).

> ⚠ **Mutually exclusive**. Declaring both `ct_model:` AND `ct_models:` at the same time is a schema validation error (`cv.Invalid`). Use only one of the two.

### A UI3 install with different clamp sizes

The canonical scenario for a home with on-site generation or a mix of load types:

- **Channel 0** — small loads (LED lighting, standby electronics). An SCT-013-005 (5 A) clamp gives maximum resolution on small currents.
- **Channel 1** — main appliances (cooktop, water heater). An SCT-013-030 (30 A) clamp covers the typical household peak.
- **Channel 2** — main inlet or a heavy consumer (EV, heat pump). An SCT-013-100 (100 A) clamp leaves headroom for peaks.

A single `rbamp:` block is enough:

```yaml
rbamp:
  id: home_meter
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]

sensor:
  - platform: rbamp
    rbamp_id: home_meter
    voltage:
      name: "Mains Voltage"
    current:
      name: "Lighting Current"     # SCT-013-005, channel 0
    current_1:
      name: "Appliances Current"   # SCT-013-030, channel 1
    current_2:
      name: "Main Inlet Current"   # SCT-013-100, channel 2
    # ...power_0/_1/_2, energy_0/_1/_2 declared the same way
```

The component installs the exact model on each channel at setup.

### What the key does under the hood

On firmware v1.2+, writing `ct_model:` / `ct_models:` atomically loads the factory coefficients (noise floor + gain) for the selected model into the module's RAM and saves them to flash. No additional calibration steps are required on the user side — the module ships from the factory with every coefficient table preloaded.

On earlier firmware the value is stored as a model tag. Full auto-load of coefficients arrives with a firmware upgrade — no YAML changes will be needed.

Writing each model takes about 700 ms of flash-write time. For a UI3 with three different models (`ct_models: [...]`) total setup time is around 2-3 seconds at boot. The component feeds the watchdog during that window.

## Verifying the install

After the first boot, check the readings on a **purely resistive load** — an incandescent bulb, kettle, or heating element. A resistive load gives a known signal shape:

- Power factor (`power_factor`) should be close to 1.00 (consistently > 0.95).
- Real power (`power`) should be positive and stable.
- RMS current (`current`) should match `P / U` with reasonable accuracy.

If the readings do not match:
- `current` stays at ~0 with the load running — check polarity and that the clamp encloses a single conductor (see below)
- `power` is steadily negative — flip the clamp on the conductor (arrow the other way)
- `power_factor` at ~0.5..0.7 on an incandescent bulb — a reactive load may be nearby, or the clamp is installed incorrectly

Symptom-driven diagnostics are in [10_troubleshooting.md](10_troubleshooting.md).

## CT clamp polarity

The clamp body has a printed arrow indicating the "normal" energy flow direction (consumption, toward the load). With correct installation (arrow following current flow into the load):

- `P_real > 0` — consumption.
- `P_real < 0` — export (STANDARD / PRO only).

If real power is negative while a load is running, physically flip the clamp on the conductor. Do not correct polarity in software: that masks directional bugs in downstream automations.

The clamp must close completely around **one** conductor. Do not run L and N through the same clamp — their magnetic fields cancel and the sensor will read near zero.

## Voltage channel

UI* SKUs contain a built-in voltage transformer. Its primary winding connects to mains L and N through the module's terminal blocks; its secondary is loaded into the board's burden network and fed into the ADC.

The voltage transformer's factory configuration is fixed by the board revision — there is nothing for the user to tune. The `voltage` sensor is ready to use out of the box.

I-only SKUs (I1 / I2 / I3) have no voltage channel. The `voltage` slot returns 0 on those and must not be declared alongside `power:` or `energy:` (the schema validator enforces the dependency).

## Multi-channel modules

On UI2 / UI3 every channel is a separate ADC plus burden network, not a shared core. This lets you:

- Fit the same CT model on every channel (`ct_model:`) — the typical configuration for uniform loads.
- Fit different models on different channels (`ct_models:`) — to optimize accuracy on mixed installs (see above).
- Aim one channel at consumption and another at solar export — polarity on each channel is set independently by the physical clamp direction.

## What's next

- [04_hardware.md](04_hardware.md) — physical wiring, GPIO selection, multi-module bus
- [05_quickstart.md](05_quickstart.md) — first boot in 5 minutes with a pre-configured clamp
- [10_troubleshooting.md](10_troubleshooting.md) — symptom-driven install diagnostics



---

← [Module Tiers](02_tiers.md) · [Docs index](README.md) · [Wiring](04_hardware.md) →
