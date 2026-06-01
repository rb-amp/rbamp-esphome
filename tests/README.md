# Bench validation plan for esphome-rbamp v1

The component is feature-complete (m1–m7). This document is the **cumulative
bench-validation checklist** to run before tagging v1 and submitting to HACS.

## Prerequisites

- ESPHome ≥ 2023.6 (needed for `i2c::I2CDevice::set_i2c_address()` in m6).
  Locally tested against 2026.3.x / 2026.4.x cgen — all 4 example YAMLs
  validate cleanly.
- ESP32 dev board (DevKit-C or any board with usable I2C on GPIO21/22).
  ESP8266 is out of v1 scope.
- One rbAmp module on the I2C bus at 0x50, ideally a **UI3 SKU** so all
  three current channels can be exercised in a single boot.
- For m6 testing: device configured in **factory-provisioning mode** per
  the device's hardware documentation (required for flash-writing operations
  such as CT model / address change).
- Use the ESPHome Home Assistant Add-on or a Python 3.10–3.13 venv. The
  PlatformIO bootstrap on host Python 3.14 currently fails; not a code
  issue — switch interpreter and proceed.

## Verification phases

### Phase 1 — smoke test (m1 + m2 + auto-detect)

Flash `example/ui3.yaml`. Watch ESPHome logs:

```
[I] [rbamp:???]: Setting up rbAmp at 0x50 ...
[C] [rbamp:???]:   Firmware version: 0xXX
[C] [rbamp:???]: rbAmp:
[C] [rbamp:???]:   Address: 0x50
[C] [rbamp:???]:   Firmware version: 0xXX
[C] [rbamp:???]:   Topology: SINGLE, channels: 3, voltage: yes
```

Confirm in HA:

- `Mains Voltage` ≈ 220 V
- `CH0 Current` ≈ load current
- `CH0 Power` ≈ load power (W)
- `CH0 Power Factor` 0.0..1.0
- `Mains Frequency` = 50 (or 60 in US bench)
- No `Cannot read REG_MODE` / `Probe failed` warnings

### Phase 2 — energy integration accuracy (m3)

Stable known load (e.g. 100 W incandescent lamp) for 1 hour. Expect:

- `CH0 Energy` increments by ~100 Wh over 1 h, within ±1 Wh
- Cross-check: same `avg_p_W × dt_s / 3600` as
  `tools/batch_test/energy_master_12_j9.log` for the same configuration

### Phase 3 — multi-channel (m4)

Three independent loads on CH0/CH1/CH2 (e.g. 100 W lamp / 60 W fan / 30 W
charger). Verify each `Power_N` and `Energy_N` tracks its own load
independently. Per-channel Wh should match a single-channel reference
reading via an independent I²C master tool.

### Phase 4 — persistence (m5)

Mid-soak (after Phase 2), reboot the ESP32 via OTA or power cycle. After
boot:

- `Mains Energy` resumes from the pre-reboot value, **not 0**
- ESPHome log on boot:
  `Restored Wh from NVS: total[X.XXX, ...] export[...]`
- HA Energy dashboard does NOT show a meter-reset event

Run for 1+ hours covering at least 2 reboot cycles to confirm no drift.

### Phase 5 — address change (m6)

Develop-mode firmware required.

1. Flash `example/address_change.yaml` (0x50 → 0x51).
2. Watch logs:

```
[C] [rbamp:???]:   Changing I2C address: 0x50 -> 0x51
[C] [rbamp:???]:   Address change confirmed at 0x51 (fw 0xXX)
[W] [rbamp:???]: IMPORTANT: update YAML to `address: 0x51` and remove `new_address:`
```

3. Edit YAML: `address: 0x51`, remove `new_address:`. Re-flash. Confirm
   normal operation continues on 0x51.

**Production-mode firmware variant** of the test:

- Same YAML, but device booted in standard production mode (factory mode disabled).
- Expected log: `Address-change requires the device's factory-provisioning mode (REG_MODE=1, got 0)`
- Component stays on 0x50 with a warning, no flash write.

### Phase 6 — replacement test (HA Energy dashboard continuity)

This is the v1 marketing claim: a user replacing `pzem004t` with `rbamp`
should not lose energy history.

1. Set up an ESPHome node with `pzem004t` in HA, let it run for >24 h
   (or a synthetic config with simulated values).
2. Replace the `pzem004t` block with `rbamp`, keeping all `sensor:` entries
   with identical `name:` fields. See README.md "Conversion guide".
3. Re-flash, wait for the next HA Energy dashboard refresh.
4. Confirm: history tab shows continuous energy progression across the
   swap, no reset spike.

### Phase 7 — broadcast LATCH (experimental)

`example/multi_module.yaml` with two or three rbAmp modules on the same
bus.

1. Verify broadcast write goes through. With logger at DEBUG level,
   the LATCH cycle should issue a write to 0x00 (general call).
2. Compare snapshot timing: read PERIOD_LATCH_MS (0xEC) from each module;
   they should differ by no more than the ESPHome polling jitter
   (~10 ms) when `broadcast_latch: true` is enabled, vs the per-module
   wall-clock delta (>100 ms) without it.
3. Energy totals across modules summed over a soak should match a
   reference single-master reading.

This phase is **expected to expose firmware-side issues** if any —
general-call has documented support but no master-side validation in
this repo's history. Findings feed back into the firmware.

## Acceptance criteria for v1 tag

- Phases 1–6 pass cleanly.
- Phase 7 may stay experimental — `broadcast_latch:` remains opt-in with
  a documented caveat.
- README, example YAMLs, and `docs/user-doc-draft/06_esphome_plan.md` are
  in sync with the shipping component.

## What "fail" looks like

- `Probe failed at 0x50` on boot → check wiring, pull-ups, slave power.
- `Period snapshot not yet valid` log lines indefinitely → bench frequency
  not locked, or device firmware in cal / production-write-guard state.
- `Failed to save Wh totals to NVS` → check ESP32 NVS partition; very rare.
- `Address change applied but slave not responding at 0xNN` → manual
  recovery needed; consult the device's hardware documentation.
