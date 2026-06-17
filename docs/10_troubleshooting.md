# 10 · Troubleshooting

![Troubleshooting decision tree: symptom → quick check → action](images/troubleshoot-flowchart.png)

A symptom-oriented debugging guide for the `rbamp` component in ESPHome. Every entry follows the same shape: what you see in the log, what it means, how to diagnose it, and a concrete fix.

Contents:

1. [I²C / hardware](#1-ic--hardware)
2. [Data quality](#2-data-quality)
3. [Energy and persistence](#3-energy-and-persistence)
4. [Home Assistant integration](#4-home-assistant-integration)
5. [Build and toolchain](#5-build-and-toolchain)
6. [Identity / Capability / Error (v1.3)](#6-identity--capability--error-v13)

---

## 1. I²C / hardware

### Probe failed at 0x50 — no response

**What you see in the log**:

```
[E][rbamp:xxx]: Probe failed at 0x50 — no response. Check wiring/address.
[E][component:xxx]: Component rbamp was marked as failed.
```

The component then calls `mark_failed()` and stops further `update()` calls. All bound sensors stay "Unknown" in Home Assistant.

**What it means**:

The ESP32 sent the address phase to 0x50 (or another address from `address:`) and received no ACK. The module is either absent, unpowered, or on a different address.

**How to diagnose**:

1. Confirm that `scan: true` is set in the `i2c:` YAML block. Reboot the ESP32 and watch for:

    ```
    [I][i2c.arduino:069]: Found i2c device at address 0x50
    ```

    If the scan found the device, the `address:` key in your `rbamp:` block is wrong — update it.

2. Measure 3.3 V on the module's VCC pin with a multimeter. Zero or < 3.0 V means power is missing or marginal.

3. Check the continuity of SDA and SCL from the ESP32 GPIO to the module connector. A missing or swapped wire is the most common cause on fresh builds.

4. Check the pull-up resistors on SDA and SCL (4.7 kΩ to 3.3 V). Without pull-ups the bus sits low and every address phase looks like a NACK.

5. If you recently performed an address change (the `new_address:` key), the module may now be on the new address. Add `scan: true`, find the device, update `address:` accordingly, and remove `new_address:`.

**Fix**: correct the wiring / power, and update `address:` if the module moved. If the module does not respond after an address change, see ["Address changed but module not responding"](#address-changed-but-module-not-responding) below.

---

### RT block not ready — publish skipped

**What you see**:

```
[D][rbamp:xxx]: RT block not ready (STATUS=0x00) — skipping live publish
```

**What it means**: the status register reported that the module is not ready. This happens when the module has not yet committed its first instantaneous-values block after startup, is mid flash-write, or is in a soft reset.

**How to diagnose**:

1. If the entry appears once at boot (in the first 30 s after power-up) and then disappears, this is normal. The module needs one commit cycle (~200 ms) to set the READY bit after power-up. The startup latch in `setup()` also produces one skipped cycle.

2. If the message streams continuously after warm-up, the module may be stuck. Check for repeated `ct_model:` / `ct_models:` entries (for example, a key left in the YAML that is written on every boot, producing a permanent flash-write window).

3. If the message appears occasionally (once every few cycles), this is normal variability tied to NACKs. The retry loop catches most transients; sometimes the status read still fails, which triggers this DEBUG entry.

**Fix**:

- Warm-up skips at boot are expected; wait two full `update_interval` cycles.
- If `ct_model:` / `ct_models:` is set in the YAML, leave it: on each boot the repeated write is idempotent (the module does not write if the value is already in flash). If the warm-up skips bother you, comment out the key after a successful first run.
- If the module stays not-ready for 60+ s, power-cycle the module (not the ESP32) for a clean cold start.

---

### "rbamp took a long time for an operation (XXX ms)"

**What you see**:

```
[W][component:522]: rbamp took a long time for an operation (406 ms), max is 30 ms
```

**What it means**: this warning fires when any component's `update()` exceeds the ESPHome cooperative scheduler's default 30 ms budget. For the rbamp component this is **expected and harmless**.

On v1.3 firmware, READ auto-increment is supported for the instantaneous-values block (the contiguous range covering U / I / P / PF / Q for all declared channels). The component issues a single burst read for that block — one address phase followed by ~36 data bytes — which costs ≈ 8–10 ms of wire time at 50 kHz. WRITE auto-increment is **not** supported, so per-register writes (CT model staging, sensor class, address commit) still go byte-at-a-time. If the burst read fails (NACK partway through, capability bit not set on legacy firmware), the component falls back to per-register reads (4 transactions per float, up to 36 transactions ≈ 90 ms at 50 kHz). With the per-byte retry triple (up to 3 × 5 ms gap on a NACK-heavy cycle) the worst-case fallback path is normally 300–450 ms — which is what produces this warning when it appears.

The latch-settle (`set_timeout("rbamp_period", 50, ...)`) is **non-blocking** — `update()` yields control to the scheduler while the 50 ms elapse. The warning reflects the wall-clock time of the whole blocking instantaneous-values read, not WiFi or API starvation.

**No user action is required**. The warning is cosmetic; HA sensors update correctly. On healthy v1.3 modules the burst-read path normally stays under 30 ms and the warning does not fire at all.

**If the warning fires every cycle**: the bus is forcing the per-register fallback (legacy firmware without the READ-burst capability, or marginal pull-ups producing mid-burst NACKs). Check the firmware version (`Firmware version: 0x04` in the boot log = v1.3) and the pull-up topology (single 4.7 kΩ pair to clean 3.3 V).

---

### Failed to read the period snapshot

**What you see**:

```
[W][rbamp:xxx]: Failed to read PERIOD_AVG_P[0]
[W][rbamp:xxx]: Failed to read PERIOD_AVG_P[1]
```

**What it means**: after the latch command and the 50 ms wait, the component could not read the period-average power register for channel N after three retries. The energy accumulator for that channel is **not updated** in this cycle.

The most common causes:

1. **Flash-write blackout**. The module is writing a CT model (triggered by `ct_model:` or `ct_models:` at boot) or saving a new address. The window is ~700 ms; if the 50 ms latch-settle lands in it, the read times out.
2. **Missed latch under heavy polling**. Under heavy back-to-back I²C polling the module side may miss a latch. The snapshot stays empty, but the valid flag will be 0 — the component skips integration with the message "Period snapshot not yet valid; keeping previous integration window". The warning fires only when the valid flag is set but the register read still failed.
3. **Transient NACK exhaustion**. All three retries on at least one of the float's four bytes failed. Rare at 50 kHz (< 1% of cycles); more frequent at 100 kHz.

**What happens to the energy**: on a warning, `last_latch_ms_` is **not** updated. On the next successful cycle, `dt_ms` covers both the missed window and the new one — energy recovers, assuming a roughly stable load over the interval. This is an acceptable small inaccuracy.

**Fix**:

- Remove address-change keys (`new_address:`) from the YAML after the first successful boot.
- If the warning fires more than once an hour, check the bus speed (`50kHz` is recommended) and the pull-up resistors (4.7 kΩ).
- If the warning fires on every cycle, check for a conflict with another device on the bus.

---

### Address changed but module not responding

**What you see**:

```
[E][rbamp:xxx]: Address change applied but slave not responding at 0x51 —
    check wiring and try power-cycling the rbAmp.
[E][component:xxx]: Component rbamp was marked as failed.
```

**What it means**: the address-change flow ran through all steps (write the new address, save to flash, soft reset, switch the component's internal I²C address), but the module did not respond on the new address on the first check.

Possible causes:

1. The address was written and saved, but the module entered a bad state (flash parameter corruption, a power glitch during the 700 ms write window).
2. The module was not in factory-provisioning mode at the time of the attempt. The mode-status read may itself have failed (NACK), in which case the component performs the write speculatively, with a warning in the log.
3. A wiring problem that existed before the address change (the old-address probe only succeeded by coincidence on one of the three retries).

**Recovery procedure**:

1. The module is on an unknown address. Use a test YAML with `i2c: scan: true` and a power cycle — find which address the module responds on.
2. If the module is found on the new address (for example 0x51), update the YAML to `address: 0x51`, remove `new_address:`, and reflash. The next boot will be clean.
3. If the module does not respond on any address, the flash write may have corrupted the parameters. Recovery requires physical intervention: consult the module documentation or your supplier.

Background: the address-change flow on v1.3 firmware is a two-phase commit — the master writes the new address into a staging register, then issues `CMD_COMMIT_ADDR` to make it persistent. The 700 ms write window is the flash-erase + program of the user-config page. See [09 · API reference](09_api_reference.md).

---

### i2c_master hung — ESP32 boot-loop or RT block never publishes

**What you see**:

```
[I][i2c.idf:xxx]: Performing I2C bus reset...
[E][i2c.idf:xxx]: i2c_master_send_buf failed: ESP_ERR_TIMEOUT
[E][rbamp:xxx]: Probe failed at 0x50 — no response. Check wiring/address.
[E][component:xxx]: Component rbamp was marked as failed.
```

In the worst case, a task watchdog timeout reboots the ESP32 after several
minutes of soak (under load, not at bring-up). It can look like "the module
worked for an hour and then stopped".

**What it means**: the ESP-IDF v5 `i2c_master` driver has a known hazard: on a
marginal bus (slow rise time, weak pull-up, EMI burst) the internal
`i2c_ll_is_bus_busy` infinite-spins, waiting for a STOP condition that never
arrives. In ESPHome 2025.x this spin can eat the app-WDT budget and
trigger a reboot. This is a **canon ESP-IDF problem**, not rbAmp-specific.

**Three-layer mitigation** (all active in the component by default, plus hardware
recommendations):

1. **Software**: the built-in NACK discipline — 50 kHz by default + 3× per-byte
   retry with a 5 ms gap + the IP-009 sanity filter `std::isfinite()`. Already
   in the component since v0.2+. Do not disable `frequency: 50kHz` until the
   stop-handler hardening fix ships in the firmware.
2. **Hardware**: an external **4.7 kΩ pull-up** on SDA + SCL (a single external
   pull-up pair, with the in-module jumpers cut — see [§Multi-module bus](#multi-module-bus-primary-topology)).
   The ESP32 internal pull-ups (~50 kΩ) are **weak** — on a long bus or with a
   marginal supply they are not enough. 4.7 kΩ pull-ups to clean 3.3 V are
   mandatory for a production deployment.
3. **Hardware**: **do not hold the debugger / OTA reset asserted** during I²C
   transactions. A RESET asserted in the middle of a transaction leaves the bus
   hung (the slave continues to drive SDA), which is exactly what provokes the
   bus-busy infinite-spin.

**Hang rate validated on the bench** (Fix-A fleet, 24-hour soak):

| Layer | Hang rate | Note |
|---|---|---|
| Software only (no external pull-up) | ~1× per 10 minutes | the bridge-WDT fires and recovers via reset, but Wh loses a window |
| Software + external 4.7kΩ pull-up | ~1× per 24 hours | EMI-burst edge case; auto-recovery |
| Software + external pull-up + clean reset | 0 hangs over 24h soak | production target |

**Severity = measurement-time** (full-duration soak), not bring-up. At bring-up
the NACK discipline catches 99%+ via the retry triple — real hangs
only begin after hours of clean soak. For the full discipline reference, see
[09 · API reference](09_api_reference.md).

> **If recovery fails**: as a last resort, configure
> `wdt:` (esphome core) with an increased `app_wdt_timeout_seconds:` (default 5 s;
> set 10–15 s). This buys time for component recovery without a reboot.

---

## 2. Data quality

### Voltage jumps around — 228 V / 502 V / 1.96 V intermixed

**What you see**:

```
[D][sensor:xxx]: 'Mains Voltage': Sending state 502.730 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 1.964 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 228.365 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 502.730 V
```

In Home Assistant the voltage graph is a sawtooth between ~228 V, ~2 V, and ~503 V. Current may simultaneously show 0 A or −2 A.

**What it means**: "ghost" values are surfacing from a buffer leak in the ESP-IDF `i2c_master` driver under NACK conditions at 100 kHz. This **should be eliminated** by the default configuration of the current component version. If the symptom appears:

- The `i2c:` block has `frequency: 100kHz` — change it to `50kHz`.
- The component version is older than 2026-05-24 — update it.

**Fix**:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz   # required for reliable operation
  scan: true
```

For more on the NACK-discipline mechanism (50 kHz default + per-byte retry + sanity filter for NaN/Inf), see [09 · API reference](09_api_reference.md).

> **Compatibility with `framework: type:`** — the NACK ghost-value problem
> manifests only under the ESP-IDF `i2c_master` driver. On
> `framework: type: arduino` (the default), the Arduino-framework `Wire`
> stack is used, which behaves differently under NACK and does not reproduce
> this symptom. This means:
>
> - If the YAML has `esp32: framework: type: arduino`, you can in theory
>   keep `frequency: 100kHz` without visible ghost spikes, but the
>   component's per-byte retry loop still runs on top of Wire and the
>   **periodic NACKs at 100 kHz remain** — just without the chilling
>   ghosts. For stable HA graphs we recommend 50 kHz regardless of
>   framework.
> - If the YAML has `esp32: framework: type: esp-idf`, then `frequency: 50kHz`
>   is **mandatory**. This is the configuration in which the `i2c_master`
>   driver shows the ghost symptom most vividly.
>
> The component itself does not choose the framework — it runs on top of
> both. This is purely a decision in your YAML.

---

### Current is permanently 0 A under load

**What you see**:

```
[D][sensor:xxx]: 'Mains Current': Sending state 0.000 A
```

Power is 0 W, energy does not grow, but voltage reads correctly (for example 228 V).

**What it means**: one of two typical causes:

**Cause 1 — `ct_model:` is not set or does not match the physical clamp**. On firmware v1.2+ the module uses the factory coefficients for the model selected in the YAML. If `ct_model:` is unset, or set to, say, `SCT_013_050` with a real SCT-013-005, the measurement range is so much wider than the physical signal that the AC-ADC resolution yields zero counts.

**Cause 2 — the clamp is installed incorrectly** on the conductor (see polarity in [03_sensor_selection.md](03_sensor_selection.md) and clamping around only one conductor).

**How to diagnose**:

1. Confirm the clamp is physically installed on a current-carrying conductor and that the conductor really carries current (use a clamp meter as a reference, if you have one).
2. Open the YAML and check that `ct_model:` (or `ct_models:` for UI3) matches the physical clamp model.
3. If `ct_model:` is set on firmware v1.2+ but `sensor_class:` is omitted or set to a value other than `SCT_013`, the module refuses the model write. The boot log shows a warning pointing to the missing precondition.

**Fix**:

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013       # default; required on v1.2+ before ct_model
  ct_model: SCT_013_005       # match the physical clamp
```

After boot the module writes the model to flash once (~700 ms) and loads the factory coefficients. The next instantaneous-values publish already gives correct current.

**Small-current measurement accuracy on UI2 / UI3** — the dual-CT pattern.
If you want high resolution on small loads (lighting, standby) and a wide
range for peaks (kettle, washing machine) at the same time, use two current
channels on the same phase simultaneously with different clamps:

```yaml
rbamp:
  id: home_meter
  sensor_class: SCT_013
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_050]
  # CH0 — SCT-013-005 for resolving idle loads (~5..50 W)
  # CH1 — SCT-013-030 for the main household load (~50..7000 W)
  # CH2 — SCT-013-050 for the peak (EV-charging headroom up to 11.5 kW)
  # NOTE v1.3: SCT_013_100 reserved; for peaks > 50A — several SCT_013_050 in parallel
```

All three clamps clamp around the **same** L-phase conductor.
SCT-013-005 gives ~10× better resolution on small currents than
SCT-013-030, but saturates at ~5 A. SCT-013-030 covers the "middle"
of the useful range; SCT-013-050 catches peak surges up to ~11.5 kW without saturating.
On the HA side, a template sensor selects the "active" measurement by threshold:

```yaml
# configuration.yaml — select the active channel by range
template:
  - sensor:
      - name: "Mains Current (active)"
        unit_of_measurement: "A"
        device_class: current
        state_class: measurement
        state: >
          {% set i0 = states('sensor.rbamp_ch0_current') | float(0) %}
          {% set i1 = states('sensor.rbamp_ch1_current') | float(0) %}
          {% set i2 = states('sensor.rbamp_ch2_current') | float(0) %}
          {% if i2 > 25 %}{{ i2 }}
          {% elif i1 > 4 %}{{ i1 }}
          {% else %}{{ i0 }}
          {% endif %}
```

This fully covers the 5 mA..100 A range with ~1 mA resolution on
small loads. Trade-off: three CT clamps on one conductor are physically
bulky, and each occupies one module channel (on UI3 that is the whole
module). For most deployments a single clamp with a rating matched to
the load covers 99% of scenarios — the dual/triple-CT pattern is needed
only when wide dynamic range is required.

---

### Period snapshot not valid — energy does not grow

**What you see**:

```
[D][rbamp:xxx]: Period snapshot not yet valid; keeping previous integration window
```

The `energy` sensor in Home Assistant shows a constant (or stays at 0 Wh after boot), even though power reads correctly.

**What it means**: the period-snapshot valid flag was 0 when the component checked it 50 ms after the latch command. The module has not yet committed a valid period snapshot.

Normal causes:

1. **First latch after boot**. The startup latch in `setup()` always discards its result — the module accumulator is reset on the first latch command. The second latch (one `update_interval` after boot) is the first one that integrates. Energy starts accumulating after the first full `update_interval` from boot.
2. **Module cold start**. The module needs at least one ~200 ms commit cycle for any data in the period accumulator. If the first latch falls earlier, the valid flag is 0.
3. **Missed latch under heavy polling**. Under heavy I²C load the module side may miss a latch. The snapshot stays empty; the next latch should succeed.

If the entry fires on **every** `update()` cycle indefinitely (not just the first two), the module may be in a state where the period accumulator never commits. Power-cycling the module clears this state.

---

## 3. Energy and persistence

### `energy_total` reset to 0 after a reboot

**What you see in the HA Energy dashboard**: a sharp drop to 0 Wh, then normal accumulation from 0. The old Wh history is preserved in HA long-term statistics, but the "current meter reading" looks reset.

**What it means**: restoring from NVS found no data or loaded invalid data. Possible causes:

1. **NVS layout version bump**. The NVS key is derived from the component ID, XORed with a layout-version constant. If the structure layout changed (with a component upgrade), the NVS slot key changes and the old data is no longer found. Restoration silently starts from 0.
2. **NVS fragmentation or corruption**. The ESP32 NVS partition uses flash wear-leveling. On very old deployments or after many flashes, fragmentation can cause a save to fail. The log shows: `[W][rbamp:xxx]: Failed to save Wh totals to NVS — will retry next cycle`.
3. **Partition layout change**. Flashing a new `partitions.csv` that changes the size or position of the `nvs` partition invalidates all saved values.
4. **First boot after a factory reset or full flash erase**. Expected: NVS is empty, the accumulator starts from 0.

**How to diagnose**:

Look in the boot log for:

```
[C][rbamp:xxx]: No saved energy state — starting from 0 Wh
```

If present, restoration found nothing. Also:

```
[C][rbamp:xxx]: Restored Wh from NVS: total[0.310, 0.000, 0.000] export[...]
```

If the restored value is noticeably lower than expected, the last 5-minute NVS save may have been skipped (worst-case loss: ≈ 5 Wh at an average of 60 W).

**Fix**:

- On a component upgrade that changes the NVS layout, old totals do not restore automatically. HA long-term statistics keep the history even if the "current reading" zeros out.
- To preserve Wh through a forced NVS wipe, read the current sensor values in HA Developer Tools before the wipe and record them. There is no way to inject a starting value into the component.

---

### Raw chip `REG_PERIOD_LATCH_MS` reads ~27% lower than the real period time (v1.3 diagnostic)

**What you see** (if you read raw registers via `lambda:` or via direct
i2c-debug):

```
[D][lambda:xxx]: REG_PERIOD_LATCH_MS = 14860  (chip-side period accumulator)
[D][lambda:xxx]: t_master_dt          = 20003 ms (millis() between latches)
```

That is, the chip-side counter shows ~14.9 s while the wall-clock `millis()`
shows ~20.0 s. The ~26–27% difference is **expected**, not a bug.

**What it means**: the chip-side period counter (`REG_PERIOD_LATCH_MS` 0xEC and
`REG_PERIOD_MS_FW` 0xCA) is counted on the microcontroller's SysTick timer,
which suffers from **SysTick starvation** under full ISR load (DMA + I²C +
period commit + zero-cross detector). This problem was measured on the bench: the chip
counter reads ~26–27% lower than the real wall-clock time.

**Canon L9**: the ESPHome component (like all sister libraries — esp-idf / arduino /
python) does **NOT use** the chip-side counter for energy integration.
Wh accumulation is computed on the master wall-clock via `millis()` (= ESP32
`esp_timer_get_time()`):

```cpp
// finish_latch_phase_() in src/rbamp_period.cpp:
uint32_t t_now = millis();
uint32_t dt_ms = t_now - last_latch_ms_;
e_wh[ch] += avg_p_w[ch] * (double)dt_ms / 3'600'000.0;
last_latch_ms_ = t_now;
```

Wh is monotonic, with billing-grade accuracy. **Validated on the bench**: with
master_dt/wall=0.999 vs latch_ms/wall=0.743, energy rel_err = 0.0000%.

**Anti-revert callout**: if you fork the component and want to "optimize" it by
switching integration to the chip-side `REG_PERIOD_LATCH_MS` — **DO NOT DO
THIS**. It will give a ~27% Wh undercount. The anti-revert comment is inlined in
`finish_latch_phase_` and in `rbamp_const.h`.

**User-side diagnostics**: `REG_PERIOD_LATCH_MS` remains accessible via a raw
register read (`lambda` access to the internal `RbAmpComponent::read_reg16_(0xEC)`).
This is **diagnostic only** — for checking chip health, not for billing.

---

### Energy in HA does not match a reference meter

**What you see**: the `energy` sensor in HA reads noticeably lower (for example 10–20%) than a reference smart meter or the utility meter over the same period.

**What it means**: the component uses the ESP32-side `millis()` interval between latch commands as the integration time, not the module's timer. That is, the module's clock drift does **not** affect the accuracy of the energy accumulated in the component.

If the energy reads low, look among these:

1. **Load character**. The period-average power register is genuinely time-averaged power over a closed period, not an instantaneous value. A pulsed load (motor, compressor) with a short on-time within the 60-second period averages below its peak power.
2. **Clamp placement and burden calibration**. If the factory CT model (`ct_model:`) does not match reality, the coefficient ranges will be offset. Check that `ct_model:` in the YAML matches the physical clamp on the conductor.
3. **BASIC firmware tier**. On the BASIC SKU, per-window P samples are clamped to 0 before integration. If the load has a regenerative component (for example a pump motor with back-EMF), BASIC undercounts.

---

## 4. Home Assistant integration

### HA shows "Unavailable" for all rbamp sensors

**What you see in HA**: all `sensor.rbamp_*` entities show "Unavailable".

**Causes and fixes**:

1. **ESP32 offline**. Run `esphome logs --device <IP>` — if you cannot connect, the ESP32 is offline. Check the WiFi credentials, and the fallback AP if one is configured.
2. **API connection lost**. The ESPHome native API (port 6053) may be blocked by a firewall. Check that the ESP32 is reachable by IP:

    ```
    ping <esp32-ip>
    ```

    If ping succeeds but HA still shows unavailable, the ESPHome integration in HA needs to be removed and re-added (Settings → Integrations → ESPHome → Delete, then re-discover).
3. **API encryption key mismatch**. If `api: encryption: key:` is configured in the YAML, the HA integration must know the same key. A mismatch makes HA treat the device as offline. Remove and re-add the integration after updating the key.
4. **mDNS does not work in your network topology**. If the ESP32 is on a different VLAN from HA, multicast mDNS may not cross the boundary. Add the device by IP manually in the ESPHome integration.

---

### OTA flashing fails

**What you see**:

```
ERROR Error resolving IP address: ...
# or
ERROR Upload Failed
```

**Common causes**:

1. **The IP address changed**. The ESP32 got a new DHCP lease. Check the router's DHCP table, find the device by MAC, and assign a static IP, or use the `wifi: use_address:` YAML key.
2. **The ESP32 is in safe mode**. After three failed boots in a row ESPHome enters safe mode (no custom components, only WiFi + OTA). Safe-mode OTA works, but the device runs a stripped-down firmware. The marker in `esphome logs`:

    ```
    [I][safe_mode:xxx]: Entering safe mode
    ```

    Force a clean boot: hold the BOOT button on power-up, or add `safe_mode: num_attempts: 10` to widen the threshold.
3. **Port 3232 (OTA) is blocked**. The OTA protocol uses TCP 3232. Check that no firewall rule blocks it between the machine running `esphome run` (or the HA host) and the ESP32 IP.
4. **A large firmware after adding rbamp**. The first OTA after adding an external component may exceed the OTA partition size on the default 4 MB ESP32 layout. Check the `esphome compile` output for binary-size warnings and switch to a layout with a larger OTA partition if needed.

---

### OTA right after `new_address:` provisioning does not load

**Symptom**: after a successful first boot with the `new_address:` key
(the log shows `Address change SUCCESS, new address 0x51`), the next OTA
fails with a compile error or a runtime warning `address 0x50 NACK`.

**What happened**: the module now lives on the new address (0x51), but the
YAML still points to the old one (`address: 0x50`). On the next `update()`
the component tries 0x50, gets nothing, and logs constant NACKs. The OTA
itself does not actually fail here — it is just an adjacent symptom: you
see a stream of rbamp errors in `esphome logs` and conclude the device
is broken.

**Recovery in 3 steps**:

1. Edit the YAML:

    ```yaml
    rbamp:
      id: meter1
      address: 0x51        # new address — set instead of 0x50
      # new_address: 0x51  # ← REMOVE this line. Do not comment it out,
      #                       delete it entirely, otherwise the component
      #                       tries to provision again and logs a warning
      update_interval: 60s
      ct_model: SCT_013_030
    ```

2. Run `esphome run <yaml>` (via the CLI) or Compile + Upload
   (via the HA Add-on). The node receives the new binary over WiFi OTA.

3. Open `esphome logs` and confirm that the `dump_config` startup message
   shows the correct address and that `update()` runs without NACKs.

**Rule** — `new_address:` is a **one-time provisioning operation**.
Once the module has moved to the new address and saved it to flash, the
key must be removed from the YAML. Otherwise, on every boot the component
will try to provision again (on v1.2+ the module ignores the repeated
write, but the warning remains in the log and clutters the HA `notify`).

**If the module does not respond on any address after the change**, see
["Address changed but module not responding"](#address-changed-but-module-not-responding) above.

---

## 5. Build and toolchain

### Compile error: 'esphome/core/component.h' file not found

**What you see in VS Code / clangd**: red squiggles everywhere in `rbamp.h` and `rbamp.cpp`. Popup:

```
'esphome/core/component.h' file not found
```

**What it means**: clangd (the C++ IntelliSense in VS Code) does not have access to the ESPHome + PlatformIO include paths. The build system resolves them correctly during `esphome compile`; clangd does not.

**This is not a build error**. If `esphome compile example/ui1.yaml` passes without errors, the code is correct.

**If you want to remove the red squiggles**: create a `.clangd` file in the component directory pointing to the generated include paths from the ESPHome build output (`.esphome/build/<name>/src/`). This is cosmetic and optional.

---

### PlatformIO crashes with 0xC0000001D (EXCEPTION_ILLEGAL_INSTRUCTION)

**What you see**:

```
Error: Failed to install Python dependencies (exit code: 3221225501)
```

(0xC0000001D = 3221225501 in decimal)

**What it means**: PlatformIO 6.1.19+ uses the `uv` Rust binary to install Python dependencies into its internal `penv`. On Intel Lunar Lake CPUs (Core Ultra 7 255H and similar) `uv` crashes with `EXCEPTION_ILLEGAL_INSTRUCTION` due to an instruction-set incompatibility. This affects the `penv_setup.py install` step on a fresh PlatformIO install.

**Workaround** (pre-install all dependencies via pip, bypassing `uv`):

```pwsh
# Step 1: install the ESP32 platform's Python dependencies directly
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install `
    "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14" "pyyaml>=6.0.2" `
    "rich-click>=1.8.6" "zopfli>=0.2.2" "intelhex>=2.3.0" "rich>=14.0.0" `
    "cryptography>=45.0.3" "certifi>=2025.8.3" "ecdsa>=0.19.1" `
    "bitstring>=4.3.1" "reedsolo>=1.5.3,<1.8" "esp-idf-size>=2.0.0" `
    "esp-coredump>=1.14.0" "pyelftools>=0.32"

# Step 2: bootstrap the nested ESP-IDF venv (see the next entry)
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m ensurepip --default-pip
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m pip install -r `
    "$env:USERPROFILE\.platformio\packages\framework-espidf\tools\requirements\requirements.core.txt"
```

Once these packages are installed, `get_packages_to_install()` in `penv_setup.py` returns empty and the pip install via `uv` is not called. Subsequent `esphome compile` runs work normally.

The workaround must be reapplied after a fresh PlatformIO install or after clearing the `~/.platformio` cache.

---

### ESP-IDF venv: no module `idf_component_manager`

**What you see**:

```
ModuleNotFoundError: No module named 'idf_component_manager'
```

It appears on the first `esphome compile` on a machine where PlatformIO downloaded the ESP-IDF framework but did not run the full bootstrap of its requirements.

**What it means**: the PlatformIO ESP-IDF framework is installed into the nested venv `~/.platformio/penv/.espidf-5.5.4/` (the version number may differ). That venv needs `requirements.core.txt` from the framework package. When the `uv` workaround above is applied, step 2 bootstraps pip into the venv and installs the requirements.

If the error appears after applying the `uv` workaround but step 2 was skipped:

```pwsh
# Find the IDF venv (the version number may differ):
ls "$env:USERPROFILE\.platformio\penv\" | Where-Object { $_.Name -like ".espidf-*" }

# Bootstrap pip into the IDF venv:
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m ensurepip --default-pip

# Install the IDF core requirements:
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m pip install -r `
    "$env:USERPROFILE\.platformio\packages\framework-espidf\tools\requirements\requirements.core.txt"
```

Substitute your own IDF version number for `5.5.4` (check `~/.platformio/penv/`).

---

### `esphome compile` fails: `get_object_id_hash` not found

**What you see**:

```
error: 'class esphome::rbamp::RbAmpComponent' has no member named 'get_object_id_hash'
```

**What it means**: an old version of `rbamp.cpp` called `this->get_object_id_hash()` to seed the NVS slot. This method was removed from `PollingComponent` in ESPHome 2025.x.

**Fix**: update the component to the current version. Modern code uses hash generation compatible with ESPHome 2025.x and newer.

---

### ESPHome too old — `set_i2c_address` not found

**What you see**:

```
error: 'class esphome::i2c::I2CDevice' has no member named 'set_i2c_address'
```

**What it means**: the `new_address:` address-change flow calls `i2c::I2CDevice::set_i2c_address()`, added in ESPHome 2023.6.

**Fix**: update to ESPHome 2023.6 or newer. The HA ESPHome Add-on tracks the current release automatically — the recommended installation path.

```sh
# If using a local Python venv:
pip install --upgrade esphome
```

---

## 6. Identity / Capability / Error (v1.3)

This section explains what the individual values of the identity entities
(a template `text_sensor` via `lambda:`), the capability bitmap, and the
REG_ERROR codes added in v1.3 mean.

### Identity reads return "UNK" / "UNREADABLE"

**What you see** (template text_sensor):

```yaml
text_sensor:
  - platform: template
    name: "Meter variant"
    lambda: 'return id(my_meter).get_variant_str();'  # → "UNK" in HA
```

```
[D][rbamp:xxx]: get_variant_str(): REG_HW_VARIANT read failed (NACK) — returning UNK
```

**What it means**:

- **`"UNK"`** — the component could not read `REG_HW_VARIANT` (0x55) after
  all retries. Causes: the module is on legacy firmware (v1.0–v1.1, before
  variant detection), or a marginal I²C bus with NACK exhaustion on this
  particular register.
- **`"UNREADABLE"`** — the read returned a value outside `0x55..0x5A`, beyond
  the range of valid variant codes. This indicates firmware corruption or a
  raw register collision (for example a stale buffer).

**Fix**:

- If the firmware is below v1.2, variant detection is unavailable. The `text_sensor`
  still publishes; use a YAML-side hint via `topology:` or reinitialize via an
  HA template.
- If the firmware is v1.3+ and the bus is marginal, see [i2c_master hung](#i2c_master-hung--esp32-boot-loop-or-rt-block-never-publishes).

### CAPABILITY bitmap = `0x0000`

**What you see**:

```yaml
text_sensor:
  - platform: template
    name: "Meter capability"
    lambda: 'return id(my_meter).get_capability_hex();'  # → "0x0000"
```

**What it means**: `REG_CAPABILITY` (0x57) does not exist on this firmware —
it reads as 0 (the standard "register not implemented" behavior on rbAmp
firmware). This is **legacy firmware v1.0–v1.2**.

**Component behavior**: all capability-gated features (fleet GC, two-phase
address commit, save_user_config, clear_error) automatically fall back to the
legacy path:

- `fleet_gc_enable: true` → warning + skip (capability bit not set)
- `new_address:` → single-phase fallback, requires factory-provisioning mode
- `clear_error()` → vendor opcode fallback (best effort)

**Zero regression**: existing v0.4.0 deployments keep working without
changes to YAML or behavior. This is canon C5 — capability-gated never
breaks legacy.

### `last_error_str()` shows `"ERR_PARAM"` after a `ct_model:` or `sensor_class:` change

**What you see**:

```
[W][rbamp:xxx]: REG_ERROR after CT_MODEL write: 0xFE (DEV_ERR_PARAM)
```

`get_last_error_str()` → `"ERR_PARAM"`.

**What it means**: the firmware rejected the written value. The most common
triggers:

1. **Code 5 (`SCT_013_100`)** — reserved in v1.3 firmware, not accepted.
   Replace it with `SCT_013_050` or `SCT_013_030`.
2. **CT model not in the per-class allowed set** — for example `WIRED_CT_1` with
   `sensor_class: SCT_013`. See [03_sensor_selection.md per-class table](03_sensor_selection.md#per-class-ct-validation-new-in-v13).
3. **Code 0 (UNSET)** — an attempt to clear the CT model via 0. Not supported;
   use `dev.factoryReset()` for a full reset.

**Recovery**: call `id(my_meter).write_clear_error();` via a YAML service
or via `lambda:` in `on_boot:` to clear `REG_ERROR` + `EVENT_FLAGS`
bit3. After that you can retry with a correct value.

```yaml
api:
  services:
    - service: my_meter_clear_error
      then:
        - lambda: 'id(my_meter).write_clear_error();'
```

### EVENT_FLAGS bit3 (ERR_LATCH) — re-latch behavior

**What you see**: after `clear_error()`, ~200–300 ms later bit3 in `EVENT_FLAGS`
is **set** again (sticky re-latch).

**What it means**: the firmware durable error channel **re-latches** bit3 after
a clear if the **original error condition** has not been resolved. This is by
design — the master may miss the first clear and must not assume the error is
gone forever if the parameter is still wrong.

**Fix**: full recovery requires **two** steps:

1. **Remove the cause** (replace the wrong CT model code, correct the YAML).
2. `write_clear_error()` — this call clears both `REG_ERROR` and `EVENT_FLAGS`
   bit3 in one operation.

If bit3 keeps re-latching, the parameter is still wrong; re-read
`get_last_error_str()` to find out exactly what the firmware is rejecting.