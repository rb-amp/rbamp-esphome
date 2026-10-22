# 03 · Current Sensor Selection

![CT clamp installation: clamp the L wire only, arrow pointing toward the load](images/hw-ct-install.png)

rbAmp measures current through an external CT clamp (current transformer) and voltage through a built-in voltage transformer (on UI* SKUs). This chapter is about how to pick the clamp for your load and how to tell the module which model is installed via YAML.

## Sensor Family (`sensor_class:`)

The module's hardware revision determines which current-sensor family is supported. On current SKUs only one is available:

| Class | YAML value | State |
|---|---|---|
| SCT-013 (split-core CT) | `SCT_013` | Available now, default |
| WIRED_CT (module with its own fitted wired CT) | `WIRED_CT` | Factory-paired: calibrated together with its sensor |
| BUILTIN_CT (module with a built-in sensor) | `BUILTIN_CT` | Factory-paired: calibrated together with its sensor |

The class is set once and stored in the module's flash:

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013   # defaults to SCT_013, can be omitted
```

On firmware v1.2+, this value becomes a precondition for writing the CT model: the module will refuse if the class is not set. The component never writes a class the module already has.

### Write guards: read-compare-write (v1.3+) and factory protection (v1.8.1+)

On boot the component **reads** the module's current sensor class and compares it with the
YAML-requested value. If they match, the flash write **does not happen** (a class write on
the module would also clear the channel models, so an unchanged value is never rewritten).
This means:

- The YAML block can keep `sensor_class: SCT_013` forever — a repeat boot
  does not trigger a flash erase (~0 ms instead of ~700 ms).
- The old advice to "remove `sensor_class:` from YAML after the first boot" is obsolete
  — the config is idempotent.

The same behavior applies to `ct_model:` / `ct_models:` (see below).

> **Factory-paired modules (`WIRED_CT` / `BUILTIN_CT`).** A module supplied together
> with its own fitted sensor is calibrated as a pair with that sensor. The component
> protects that calibration: a `sensor_class:` or `ct_model:` change from YAML on such a module is
> **refused** with an explicit log line (the write would replace the individual calibration with a
> generic setting). Leave both keys out of the YAML for these modules. For SCT-013 clamps
> (`SCT_013`, the default) the keys work as described below.

## Choosing the SCT-013 Model

The module accepts any of the seven models in the SCT-013 series.
Their electrical output differs, so the module needs to know which one is
installed — the factory coefficients are loaded for the specific model.

**Which model do I have?** Read the rating off the label on the clamp body: `SCT-013-030`
means 30 A, so you write `ct_model: SCT_013_030`. Use the model whose rating matches the label,
not the one that "looks closest" to your load.

| Code | YAML name | Rated current | Typical load | Maximum at 230 V | v1.3 status |
|---|---|---|---|---|---|
| 1 | `SCT_013_005` | 5 A | LED lighting, single appliances, laptops | ~1.15 kW | production |
| 2 | `SCT_013_010` | 10 A | Desktop PCs, small AC unit, single circuit | ~2.3 kW | production |
| 6 | `SCT_013_020` | 20 A | Water heaters, medium branches, ~3-5 kW | ~4.6 kW | production |
| 3 | `SCT_013_030` | 30 A | Whole-home metering at typical consumption | ~6.9 kW | production (default SKU) |
| 4 | `SCT_013_050` | 50 A | EV charging, large AC unit, induction hob | ~11.5 kW | production |
| 7 | `SCT_013_060` | 60 A | Industrial sub-meter, large branches | ~13.8 kW | production |
| 5 | `SCT_013_100` | 100 A | Main service entrance, large feeds | ~23 kW | production |

> **Installations above 50 A.** `SCT_013_060` (60 A) and `SCT_013_100` (100 A)
> are accepted; pick the clamp whose rating fits your maximum current.

The burden resistor is already installed on the module board — no external resistors between the clamp's output leads need to be added.

### Selection Tree

```
  Peak continuous current through the conductor:
  |
  +-- ≤ 5 A    (LED, single appliances, ≤ 1.15 kW)
  |       → SCT_013_005    [code 1]
  |
  +-- 5..10 A  (PCs, small loads, ≤ 2.3 kW)
  |       → SCT_013_010    [code 2]
  |
  +-- 10..20 A (water heaters, branches, ≤ 4.6 kW)
  |       → SCT_013_020    [code 6, new in v1.3]
  |
  +-- 20..30 A (whole UK/EU home, ≤ 6.9 kW)
  |       → SCT_013_030    [code 3, default SKU]
  |
  +-- 30..50 A (EV, large AC unit, ≤ 11.5 kW)
  |       → SCT_013_050    [code 4]
  |
  +-- > 50 A   (panels, commercial)
          → several SCT_013_050 in parallel (split into channels)
```

General rule: choose the clamp with the lowest rating whose maximum measurable value covers the peak load with a ~30% margin. An oversized rating on a small load lowers the signal amplitude relative to noise — accuracy drops. A clamp rated below the peak will saturate the magnetic core and give grossly wrong readings.

### Per-class CT validation (new in v1.3)

The schema validator `_validate_ct_model_per_class` rejects invalid
`sensor_class:` × `ct_model:` combinations at the `esphome config` stage, **before**
flash + boot. The allowed sets **are not contiguous** — this is explicit canon, not a bug:

| sensor_class | Allowed codes | YAML names | Behavior on mismatch |
|---|---|---|---|
| `SCT_013` | {1, 2, 3, 4, 5, 6, 7} | `SCT_013_005/010/030/050/100/020/060` | `cv.Invalid` with a hint |
| `WIRED_CT` | {1–10, 12} | `WIRED_CT_<code>` (for example `WIRED_CT_6`) | `cv.Invalid` |
| `BUILTIN_CT` | {} (none) | — | `cv.Invalid` (any ct_model is rejected) |
| `UNSET` | {} | — | `cv.Invalid` (class not set) |

Specifically:

- **`SCT_013` non-contiguous**: the accepted codes are `{1, 2, 3, 4, 5, 6, 7}`
  (`SCT_013_005`, `_010`, `_030`, `_050`, `_100`, `_020`, `_060`); the code order is not
  the rating order.
- **`WIRED_CT`** is the class of modules supplied with their own fitted wired CT; the schema
  accepts codes 1/2/3 for it. The module is factory-calibrated with that sensor, so the component
  refuses to change class or model on it (see the factory-paired note above).
- **`BUILTIN_CT`** is the class of modules with a built-in sensor; `ct_model:` is neither needed nor
  accepted by the schema. Declaring `BUILTIN_CT` without the matching hardware is pointless.

If the YAML slips past the validator (old schema + new firmware, manual
override), the firmware returns `REG_ERROR = 0xFE DEV_ERR_PARAM` — the module will not
write an unsupported code: it returns `DEV_ERR_PARAM` and the mirrors stay at the
prior value.

## How to Tell the Module

There are two keys in YAML — you pick one of the two depending on whether you have the same clamp on all channels or different ones. The syntactic transition is from a scalar value to a YAML list (array):

### Step 1 — one clamp for all channels (`ct_model:`, scalar)

If you have a UI1 (single current channel) or a UI3 with identical clamps on all three channels — one line:

```yaml
rbamp:
  id: meter1
  ct_model: SCT_013_030      # ← scalar value, one model for all channels
```

This is the most common case. Most deployments start with a single clamp model and never move beyond this line.

### Step 2 — different clamps on different channels (`ct_models:`, list)

If you have a UI2 / UI3 and want to put different clamps on the channels (for example, a small clamp on a standby line, a large one on the mains feed) — replace `ct_model:` with `ct_models:` (with a trailing `s`) and use a YAML list in square brackets:

```yaml
rbamp:
  id: meter_ui3
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]
  #         ↑                            ↑                           ↑
  #     channel 0                     channel 1                channel 2
  #
  # This is a standard YAML list (1..16 elements; only as many as the module has channels are applied).
  # Order matters: the position in the list = the channel index.
  # Mixed codes such as [1, 3, 6] are each kept on their own channel.
```

This is the spot where people most often trip up when modifying multi-channel systems. Remember: `ct_model:` (no `s`) is a scalar value for a single model; `ct_models:` (with `s`) is a list of values in square brackets, one per channel. Only as many entries as the module has channels are applied (1 for UI1, 2 for UI2, 3 for UI3); extra entries are ignored with a warning.

> **STANDARD (STD) modules.** Per-channel models are stored only by modules that keep them across a power cycle. If the connected module does not, the component refuses loudly (warning, keys ignored) rather than writing a value that would silently revert — use the single `ct_model:` there.

> ⚠ **Mutually exclusive**. Declaring both `ct_model:` AND `ct_models:` at the same time is a schema validation error (`cv.Invalid`). Use only one of the two.

### A UI3 Installation with Different Clamp Sizes

The canonical scenario for a home with its own generation or with mixed load types:

- **Channel 0** — small consumers (LED lighting, standby mode, electronics). An SCT_013_005 clamp (5 A) gives maximum resolution at low current.
- **Channel 1** — main loads (hob, water heater). An SCT_013_030 clamp (30 A) covers the typical household peak.
- **Channel 2** — a medium-power branch (water heater, ~3-5 kW). An SCT_013_020 clamp (20 A) is the sweet spot between accuracy and headroom for a typical submeter.

In a single `rbamp:` YAML block it is enough to declare:

```yaml
rbamp:
  id: home_meter
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]

sensor:
  - platform: rbamp
    rbamp_id: home_meter
    voltage:
      name: "Mains Voltage"
    current:
      name: "Lighting Current"     # SCT_013_005, channel 0
    current_1:
      name: "Appliances Current"   # SCT_013_030, channel 1
    current_2:
      name: "Boiler Current"       # SCT_013_020, channel 2
    # ...power_0/_1/_2, energy_0/_1/_2 likewise
```

The component itself assigns the exact model to each channel on boot.

### What the Key Does Under the Hood

On firmware v1.2+, writing `ct_model:` / `ct_models:` atomically loads the factory coefficients (noise floor + gain) for the chosen model into the module's RAM and saves them to flash. No additional calibration steps are required on the user's side — the module ships from the factory with all the necessary coefficient tables.

On earlier firmware the value is stored as a model marker.

#### Read-compare-write (v1.3+) — why boot is now "instant"

On boot, the v1.3 component **reads** the current `REG_CT_MODEL_CH0/1/2`
(mirrors 0x51-0x53) and compares them with the YAML-requested values. If they match,
the flash write is **skipped** entirely.

| Boot scenario | v0.4.0 (always-write) | v1.3 (read-compare-write) |
|---|---|---|
| Config already applied (mirrors match) | ~700 ms × N channels flash erase | **0 ms** (I²C read-back only) |
| Config changed (one model) | ~700 ms | ~700 ms (delta only) |
| UI3 with three different models (cold install) | ~2.8 s blackout | ~2.1 s (3 × 700 ms + verify) |
| UI3 with the same models (warm boot) | ~2.8 s blackout every time | **~0 ms** |

**In practice**: keep `ct_model:` / `ct_models:` in YAML forever —
the config is idempotent and will not wear out the flash on repeat boots.

#### Changing the SCT rating keeps the accuracy

On an STD module `ct_model` is a label only and sets no gain. The rest of this section describes BASIC modules.

Every SCT module is factory-calibrated with the SCT-013-030. When you select another
rating (`ct_model: SCT_013_010`, say), current, power and energy stay accurate — the
factory per-unit calibration is preserved:

- On module firmware up to 1.4.19 the component completes the model write with the module
  restart the procedure requires (save, reset, re-identify) and scales current, active/reactive
  power and energy to the selected sensor from the gain the module actually applies. The
  restart shows in the log and is normal; sensors may skip one cycle. A later module restart
  (yours or external) is detected and the scaling is recomputed automatically.
- On newer firmware (1.4.20+) the module performs the switch itself; the component detects
  this and leaves the readings untouched (plain model write, no restart).
- Voltage and power factor are not affected.
- If the gain the module applies differs from the expected value by more than ±5 %, the
  component logs a warning and does not scale.

No YAML change is needed for any of this.

#### Multi-channel binding

The component binds channels one by one; the order does not matter. Mixed models on
one module (for example codes `[1, 3, 6]` on a three-channel module) are each kept on
their own channel.

The component itself feeds the watchdog (`App.feed_wdt()`) during each
flash-write window — the typical ESPHome `setup()` budget of 15 s is not exceeded even
on a UI3 cold install.

## Verifying the Installation

After the first boot, check the readings on a **purely resistive load** — an incandescent bulb, a kettle, a heating element. A resistive load gives a known signal shape:

- The power factor (`power_factor`) should be close to 1.00 (steadily > 0.95).
- The active power (`power`) should be positive and stable.
- The RMS current (`current`) should match `P / U` with reasonable accuracy.

If the readings do not add up:
- `current` steadily ~0 with the load running → check the polarity and that the clamp encircles a single conductor (see below)
- `power` consistently negative → flip the clamp on the conductor (arrow the other way)
- `power_factor` ~0.5..0.7 on an incandescent bulb → there may be a reactive load nearby or the clamp may be installed incorrectly

Detailed symptom-oriented diagnostics are in [10_troubleshooting.md](10_troubleshooting.md).

## CT Clamp Polarity

The clamp body carries an arrow showing the direction of "normal" energy flow (consumption, toward the load). With correct installation (the arrow pointing in the direction of current into the load):

- `P_real > 0` — consumption.
- `P_real < 0` — export (STANDARD (STD) only).

If active power is negative with a consumer running — physically flip the clamp on the conductor. Do not correct polarity in software: it masks directional bugs in downstream automations.

The clamp must close completely around a **single** conductor. Do not pass both L and N through one clamp — their magnetic fields cancel out, and the sensor will read near zero.

## Voltage Channel

UI* SKUs contain a built-in voltage transformer. The primary winding connects to mains L and N through the module's terminal blocks, and the module measures it internally.

The transformer's factory configuration is fixed on the board revision — the user has nothing to configure. The `voltage` sensor is ready to work out of the box.

I-only SKUs (I1 / I2 / I3) have no voltage channel. The `voltage` slot returns 0 on them and must not be declared together with `power:` or `energy:` (the schema validator checks this dependency).

## Multi-Channel Modules

On UI2 / UI3 each channel is a separate, independent measurement circuit, not a shared magnetic core. This allows you to:

- Put a single CT model on all channels (`ct_model:`) — the typical configuration for uniform loads.
- Put different models on different channels (`ct_models:`) — to optimize accuracy on mixed installations (see above).
- Dedicate one channel to consumption and another to solar export — the polarity on each channel is set independently by the physical direction of the clamp.

## What's Next

- [04_hardware.md](04_hardware.md) — physical wiring, GPIO selection, the multi-module bus
- [05_quickstart.md](05_quickstart.md) — first run in 5 minutes with an already-configured clamp
- [10_troubleshooting.md](10_troubleshooting.md) — symptom-oriented installation diagnostics
