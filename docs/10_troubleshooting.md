# 10 · Troubleshooting

A symptom-oriented guide to debugging the `rbamp` component in ESPHome. Each entry is structured the same way: what you see in the log, what it means, how to diagnose, the specific fix.

Contents:

1. [I²C / hardware](#1-ic--hardware)
2. [Data quality](#2-data-quality)
3. [Energy and persistence](#3-energy-and-persistence)
4. [Home Assistant integration](#4-home-assistant-integration)
5. [Build and toolchain](#5-build-and-toolchain)

---

## 1. I²C / hardware

### Probe failed at 0x50 — no response

**What you see in the log**:

```
[E][rbamp:xxx]: Probe failed at 0x50 — no response. Check wiring/address.
[E][component:xxx]: Component rbamp was marked as failed.
```

The component calls `mark_failed()` after this and stops further `update()` calls. All bound sensors stay in "Unknown" in Home Assistant.

**What it means**:

The ESP32 sent the address phase to 0x50 (or whatever address is in `address:`) and got no ACK. The module is either absent, unpowered, or at a different address.

**How to diagnose**:

1. Make sure the `i2c:` block has `scan: true`. Reboot the ESP32 and look for:

    ```
    [I][i2c.arduino:069]: Found i2c device at address 0x50
    ```

    If the scan found a device — the `address:` key in your `rbamp:` block is wrong; update it.

2. Measure 3.3 V on the module's VCC pin with a multimeter. Zero or < 3.0 V — power is missing or marginal.

3. Check the continuity of SDA and SCL from the ESP32 GPIO to the module connector. A missing or swapped wire is the most common cause on fresh builds.

4. Check the pull-up resistors on SDA and SCL (4.7 kΩ to 3.3 V). Without pull-ups, the bus sits low and every address phase looks like a NACK.

5. If you recently performed an address change (`new_address:` key) — the module may be on the new address. Add `scan: true` and find the device; update `address:` accordingly and remove `new_address:`.

**Fix**: correct wiring / power, update `address:` if the module moved. If the module does not respond after the address change — see ["Address changed but module not responding"](#address-changed-but-module-not-responding) below.

---

### RT block not ready — publish skipped

**What you see**:

```
[D][rbamp:xxx]: RT block not ready (STATUS=0x00) — skipping live publish
```

**What it means**: the status register reported that the module is not ready. This happens when the module has not yet committed its first block of instantaneous values after startup, is in the middle of a flash write, or is in a soft reset.

**How to diagnose**:

1. If the entry appears once at boot (in the first 30 s after power-on) and then disappears — that's normal. The module needs one commit cycle (~200 ms) to set the READY bit after power-up. The startup latch in `setup()` also costs one skipped cycle.

2. If the message streams continuously after warm-up — the module may be stuck. Check whether there are repeated `ct_model:` / `ct_models:` writes (for example, the key was left in the YAML and is written on every boot, producing a constant flash-write window).

3. If the message appears occasionally (once every few cycles) — normal variance related to NACKs. The retry loop catches most transients; sometimes the status read still fails, and this DEBUG entry fires.

**Fix**:

- Warm-up skips at boot are expected; wait for two full `update_interval` cycles.
- If `ct_model:` / `ct_models:` is in the YAML — leave it: on each boot the repeated write is idempotent (the module does not write if the value is already in flash). If the warm-up skips bother you — comment out the key after the first successful run.
- If the module stays not-ready for 60+ s — power-cycle the module (not the ESP32) for a clean cold start.

---

### "rbamp took a long time for an operation (XXX ms)"

**What you see**:

```
[W][component:522]: rbamp took a long time for an operation (406 ms), max is 30 ms
```

**What it means**: this warning fires whenever any component's `update()` exceeds the default 30 ms budget of the cooperative ESPHome scheduler. For the rbamp component this is **expected and harmless** behavior.

The module has no I²C auto-increment. Each float32 register requires 4 separate I²C transactions. The instantaneous block on a full UI3 deployment reads up to 9 float registers (U, I0–I2, P0–P2, PF0–PF2, Q0–Q2) — up to 36 transactions at 50 kHz ≈ 90 ms of wire time plus overhead. With retries (up to 3 × 5 ms per byte on a NACK-heavy cycle), the total normally lands at 300–450 ms.

The latch-settle (`set_timeout("rbamp_period", 50, ...)`) is **non-blocking** — `update()` yields to the scheduler while the 50 ms passes. The warning reflects the wall clock of the whole blocking read of the instantaneous values, not WiFi or API starvation.

**No user action required**. The warning is cosmetic; HA sensors update correctly.

**If you want to reduce the warning frequency**: declare fewer sensors. A voltage-only or frequency-only deployment reads 1–2 registers per cycle and fits within 30 ms. On a full UI3 (three channels + PF + Q), 400+ ms is a property of the wire protocol.

Cross-reference: [SPEC §6](https://rbamp.com/docs/modules-basic-standard-api-reference) (no auto-increment), [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference) (50 kHz).

---

### Failed to read period snapshot

**What you see**:

```
[W][rbamp:xxx]: Failed to read PERIOD_AVG_P[0]
[W][rbamp:xxx]: Failed to read PERIOD_AVG_P[1]
```

**What it means**: after the latch command and the 50 ms wait, the component could not read the period-average power register for channel N after three retries. The energy accumulator for this channel is **not updated** in this cycle.

The most common causes:

1. **Flash-write blackout**. The module is writing a CT model (triggered by `ct_model:` or `ct_models:` at boot) or saving a new address. The window is ~700 ms; if the 50 ms latch-settle falls inside it, the read times out.
2. **Missed latch under dense polling**. Under dense back-to-back I²C polling, the latch may be missed on the module side. The snapshot stays empty, but the valid flag will be 0 — the component skips integration with the entry "Period snapshot not yet valid; keeping previous integration window". The warning fires only when the valid flag is set but the register read still failed.
3. **Transient NACK exhaustion**. All three retries on at least one of the four float bytes failed. At 50 kHz this is rare (< 1% of cycles); at 100 kHz it's more frequent.

**What happens to energy**: when the warning fires, `last_latch_ms_` is **not** updated. In the next successful cycle, `dt_ms` covers both the missed window and the new one — energy is recovered assuming approximately stable load over the interval. This is an acceptable small inaccuracy.

**Fix**:

- Remove address-change keys (`new_address:`) from the YAML after the first successful boot.
- If the warning fires more often than once an hour — check the bus speed (`50kHz` recommended) and the pull-up resistors (4.7 kΩ).
- If the warning fires on every cycle — check for a conflict with another device on the bus.

---

### Address changed but module not responding

**What you see**:

```
[E][rbamp:xxx]: Address change applied but slave not responding at 0x51 —
    check wiring and try power-cycling the rbAmp.
[E][component:xxx]: Component rbamp was marked as failed.
```

**What it means**: the address-change flow went through every step (write the new address, save to flash, soft reset, switch the component's internal I²C address), but the module did not respond at the new address on the first check.

Possible causes:

1. The address was written and saved, but the module entered a bad state (corruption of flash parameters, a power glitch within the 700 ms write window).
2. The module was not in factory-provisioning mode at the moment of the change attempt. Reading the mode status itself may have failed (NACK), and the component then performed the write speculatively with a warning in the log.
3. A wiring issue that existed before the address change (the probe of the old address succeeded only by coincidence on one of three retries).

**Recovery procedure**:

1. The module is at an unknown address. Use a test YAML with `i2c: scan: true` and a power cycle — find which address the module responds at.
2. If the module is found at the new address (e.g. 0x51) — update the YAML to `address: 0x51`, remove `new_address:`, reflash. The next boot will be clean.
3. If the module does not respond at any address — the flash write may have corrupted parameters. Recovery requires physical intervention: consult the module documentation or vendor.

Cross-reference: [SPEC §10](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 2. Data quality

### Voltage jumps — 228 V / 502 V / 1.96 V mixed

**What you see**:

```
[D][sensor:xxx]: 'Mains Voltage': Sending state 502.730 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 1.964 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 228.365 V
[D][sensor:xxx]: 'Mains Voltage': Sending state 502.730 V
```

In Home Assistant the voltage chart is sawtooth between ~228 V, ~2 V and ~503 V. Current may simultaneously show 0 A or −2 A.

**What it means**: "ghost" values from a buffer leak in the ESP-IDF `i2c_master` driver under NACK conditions at 100 kHz are surfacing. This **should be eliminated** by the default configuration of the current version of the component. If the symptom appears:

- The `i2c:` block has `frequency: 100kHz` — change it to `50kHz`.
- The component version is older than 2026-05-24 — update.

**Fix**:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz   # required for reliable operation
  scan: true
```

More on the mechanism — [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference).

> **Compatibility with `framework: type:`** — the ghost-value NACK problem
> manifests only under the ESP-IDF `i2c_master` driver. On
> `framework: type: arduino` (default), the Arduino-framework `Wire` stack
> is used, which behaves differently under NACK and does not reproduce
> this symptom. That means:
>
> - If the YAML has `esp32: framework: type: arduino` — in theory you
>   can hold `frequency: 100kHz` without visible ghost spikes, but the
>   per-byte retry loop in the component still runs on top of Wire and
>   **periodic NACKs at 100 kHz remain** — just without the chilling
>   ghosts. For stable HA charts we recommend 50 kHz regardless of
>   framework.
> - If the YAML has `esp32: framework: type: esp-idf` — `frequency: 50kHz`
>   is **required**. That's the configuration in which the `i2c_master`
>   driver shows the ghost symptom most vividly.
>
> The component itself does not choose the framework — it runs on top of
> both. This is purely your YAML's decision.

---

### Current is constantly 0 A under load

**What you see**:

```
[D][sensor:xxx]: 'Mains Current': Sending state 0.000 A
```

Power is 0 W, energy does not rise, but voltage reads correctly (e.g. 228 V).

**What it means**: one of two typical causes:

**Cause 1 — `ct_model:` not set or does not match the physical clamp**. On firmware v1.2+, the module uses factory coefficients for the model selected in the YAML. If `ct_model:` is not set, or is set to, say, `SCT_013_100` when the actual clamp is an SCT-013-005 — the measurement range is so much wider than the physical signal that the AC-ADC resolution gives zero counts.

**Cause 2 — the CT clamp is installed incorrectly** on the conductor (see polarity in [03_sensor_selection.md](03_sensor_selection.md) and coverage of only one conductor).

**How to diagnose**:

1. Confirm that the CT clamp is physically installed on a current-carrying conductor and that the conductor is actually carrying current (use a clamp meter as a reference if available).
2. Open the YAML and verify that `ct_model:` (or `ct_models:` for UI3) corresponds to the physical clamp model.
3. If on firmware v1.2+ `ct_model:` is set but `sensor_class:` is omitted or set to a value other than `SCT_013` — the module will refuse to write the model. The log at boot will show a warning indicating the missing precondition.

**Fix**:

```yaml
rbamp:
  id: meter1
  sensor_class: SCT_013       # default; required on v1.2+ before ct_model
  ct_model: SCT_013_005       # match the physical clamp
```

After boot, the module will write the model to flash once (~700 ms) and load the factory coefficients. The next instantaneous publish will give correct current.

**Low-current measurement accuracy on UI2 / UI3** — the dual-CT
pattern. If you want both high resolution for small loads (lighting,
standby) and a wide range for peaks (kettle, washing machine), enlist
two or three current channels on the same phase with different clamps:

```yaml
rbamp:
  id: home_meter
  sensor_class: SCT_013
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  # CH0 — SCT-013-005 for idle-load resolution (~5..50 W)
  # CH1 — SCT-013-030 for typical residential load (~50..7000 W)
  # CH2 — SCT-013-100 for peak transients (EV charging or generation headroom)
```

All three clamps wrap around **the same** phase L conductor.
SCT-013-005 gives ~10× better resolution on small currents relative
to SCT-013-030 but saturates near ~5 A. SCT-013-030 covers the "middle"
useful range; SCT-013-100 catches peak surges without saturation.
On the HA side, a template sensor selects the "active" reading by
threshold:

```yaml
# configuration.yaml — pick the active channel by range
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
small loads. Trade-off: three CT clamps on one conductor are
physically bulky, and each occupies one module channel (on UI3 this
takes up the whole module). For most deployments, a single clamp
sized to the load covers 99% of scenarios — the dual/triple-CT
pattern is needed only when wide dynamic range is required.

---

### Period snapshot not valid — energy is not rising

**What you see**:

```
[D][rbamp:xxx]: Period snapshot not yet valid; keeping previous integration window
```

The `energy` sensor in Home Assistant shows a constant value (or stays at 0 Wh after boot), even though power reads correctly.

**What it means**: the valid flag of the period snapshot was 0 when the component checked it 50 ms after the latch command. The module has not yet committed a valid period snapshot.

Normal causes:

1. **First latch after boot**. The startup latch in `setup()` always discards its result — the module's accumulator is reset on the first latch command. The second latch (one `update_interval` after boot) is the first one that integrates. Energy starts accumulating after the first full `update_interval` from boot.
2. **Module cold start**. The module needs at least one ~200 ms commit cycle for any data in the period accumulator. If the first latch arrives earlier — the valid flag is 0.
3. **Missed latch under dense polling**. Under heavy I²C load, the latch may be missed on the module side. The snapshot stays empty; the next latch should pass.

If the entry fires on **every** `update()` cycle indefinitely (not only the first two) — the module may be in a state where the period accumulator never commits. Power-cycling the module resets this state.

---

## 3. Energy and persistence

### `energy_total` reset to 0 after reboot

**What you see in the HA Energy dashboard**: a sharp drop to 0 Wh, followed by normal accumulation from 0. The old Wh history is preserved in HA long-term statistics, but the "current meter reading" looks like a reset.

**What it means**: NVS recovery did not find data or loaded invalid data. Possible causes:

1. **NVS layout version bump**. The NVS key is derived from the component ID XORed with a layout-version constant. If the struct layout changes (with a component upgrade) — the NVS slot key changes and old data is no longer found. Recovery silently starts from 0.
2. **NVS fragmentation or corruption**. The ESP32 NVS partition uses flash wear leveling. On very old deployments or after many flashes, fragmentation may cause save failures. Seen in the log: `[W][rbamp:xxx]: Failed to save Wh totals to NVS — will retry next cycle`.
3. **Partition layout change**. Firmware with a new `partitions.csv` that changes the size or position of the `nvs` partition invalidates all saved values.
4. **First boot after factory reset or full flash erase**. Expected: NVS is empty, the accumulator starts from 0.

**How to diagnose**:

Look at boot for:

```
[C][rbamp:xxx]: No saved energy state — starting from 0 Wh
```

If present — recovery found nothing. Also:

```
[C][rbamp:xxx]: Restored Wh from NVS: total[0.310, 0.000, 0.000] export[...]
```

If the restored value is noticeably below the expected one — the last 5-minute NVS save may have been skipped (worst-case loss: ≈ 5 Wh at average 60 W).

**Fix**:

- When upgrading the component with an NVS layout change, old totals do not restore automatically. HA long-term statistics preserve history even if the "current reading" zeros out.
- To preserve Wh through a forced NVS wipe — look at current sensor values in HA Developer Tools before the wipe and write them down. There is no way to inject a starting value into the component.

---

### Energy in HA does not match the reference meter

**What you see**: the `energy` sensor in HA reads noticeably lower (e.g. 10–20%) than a reference smart meter or utility meter for the same period.

**What it means**: the component uses the ESP32-side `millis()` interval between latch commands as the integration time, not a module timer. That is, drift of the module's clock does **not** affect the accuracy of accumulated energy in the component.

If energy reads low — look among:

1. **Load characteristics**. The period-average-power register is genuinely time-averaged power over a closed period, not an instantaneous value. A pulsed load (motor, compressor) with a short on-time inside the 60-second period averages below its peak power.
2. **Clamp position and burden calibration**. The factory CT model (`ct_model:`) does not match reality — coefficient ranges will be skewed. Check that `ct_model:` in the YAML matches the physical clamp on the conductor.
3. **BASIC firmware tier**. On the BASIC SKU, per-window P samples are clamped to 0 before integration. If the load has a regenerative component (e.g. a pump motor with back-EMF) — BASIC will under-count.

---

## 4. Home Assistant integration

### HA shows "Unavailable" for all rbamp sensors

**What you see in HA**: all `sensor.rbamp_*` entities show "Unavailable".

**Causes and fixes**:

1. **ESP32 offline**. Run `esphome logs --device <IP>` — if you cannot connect, the ESP32 is not on the network. Check WiFi credentials, fallback AP if configured.
2. **API connection lost**. The ESPHome native API (port 6053) may be blocked by a firewall. Check ESP32 reachability by IP:

    ```
    ping <esp32-ip>
    ```

    If ping passes but HA still shows unavailable — the ESPHome integration in HA needs to be deleted and re-added (Settings → Integrations → ESPHome → Delete, then re-discover).
3. **API encryption key mismatch**. If `api: encryption: key:` is configured in the YAML — the HA integration must know the same key. A mismatch — HA considers the device offline. Remove and re-add the integration after updating the key.
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

1. **IP address changed**. The ESP32 got a new DHCP lease. Check the router's DHCP table, find the device by MAC and assign a static IP, or use the YAML key `wifi: use_address:`.
2. **ESP32 in safe mode**. After three failed boots in a row, ESPHome enters safe mode (without custom components, only WiFi + OTA). Safe-mode OTA works, but the device runs a stripped-down firmware. Marker in `esphome logs`:

    ```
    [I][safe_mode:xxx]: Entering safe mode
    ```

    Force a clean boot: hold the BOOT button at power-on, or add `safe_mode: num_attempts: 10` to widen the threshold.
3. **Port 3232 (OTA) is blocked**. The OTA protocol uses TCP 3232. Make sure no firewall rule blocks it between the machine running `esphome run` (or the HA host) and the ESP32 IP.
4. **Large firmware after adding rbamp**. The first OTA after adding an external component may exceed the OTA partition size on the default 4 MB ESP32 layout. Check `esphome compile` output for binary-size warnings and switch to a layout with a larger OTA partition if needed.

---

### OTA right after `new_address:` provisioning does not load

**Symptom**: after a successful first boot with the `new_address:` key
(the log shows `Address change SUCCESS, new address 0x51`), the next OTA
fails with a compile error or runtime warning `address 0x50 NACK`.

**What happened**: the module now lives at the new address (0x51), but
the YAML still points at the old one (`address: 0x50`). On the next
`update()`, the component tries 0x50, gets nothing, and logs constant
NACKs. The OTA itself does not actually fail here — it's just a
neighboring symptom: you see a stream of rbamp errors in `esphome logs`
and conclude the device is broken.

**Recovery in 3 steps**:

1. Edit the YAML:

    ```yaml
    rbamp:
      id: meter1
      address: 0x51        # new address — set it instead of 0x50
      # new_address: 0x51  # ← REMOVE this line. Do not comment it out,
      #                       delete it entirely, otherwise the component
      #                       will try to provision again and log a warning
      update_interval: 60s
      ct_model: SCT_013_030
    ```

2. Run `esphome run <yaml>` (via CLI) or Compile + Upload (via HA Add-on).
   The node will receive the new binary over WiFi OTA.

3. Open `esphome logs` and verify that the startup `dump_config` message
   shows the correct address and that `update()` runs without NACKs.

**Rule** — `new_address:` is a **one-time provisioning operation**.
Once the module has moved to the new address and saved it to flash,
the key must be removed from the YAML. Otherwise, on every boot the
component will try to provision again (on v1.2+ the module ignores the
repeated write, but the warning stays in the log and clutters HA
`notify`).

**If the module does not respond at any address after the change** — see
["Address changed but module not responding"](#address-changed-but-module-not-responding) above.

---

## 5. Build and toolchain

### Compile error: 'esphome/core/component.h' file not found

**What you see in VS Code / clangd**: red squiggles everywhere in `rbamp.h` and `rbamp.cpp`. Popup:

```
'esphome/core/component.h' file not found
```

**What it means**: clangd (VS Code's C++ IntelliSense) does not have access to ESPHome + PlatformIO include paths. The build system resolves them correctly during `esphome compile`; clangd does not.

**This is not a build error**. If `esphome compile example/ui1.yaml` passes without errors — the code is correct.

**If you want to remove the red squiggles**: create a `.clangd` file in the component directory pointing to the generated include paths from the ESPHome build output (`.esphome/build/<name>/src/`). This is cosmetic, optional.

---

### PlatformIO fails with 0xC0000001D (EXCEPTION_ILLEGAL_INSTRUCTION)

**What you see**:

```
Error: Failed to install Python dependencies (exit code: 3221225501)
```

(0xC0000001D = 3221225501 decimal)

**What it means**: PlatformIO 6.1.19+ uses the `uv` Rust binary to install Python dependencies into its internal `penv`. On Intel Lunar Lake CPUs (Core Ultra 7 255H and similar), `uv` crashes with `EXCEPTION_ILLEGAL_INSTRUCTION` due to instruction-set incompatibility. This affects the `penv_setup.py install` step on a fresh PlatformIO install.

**Workaround** (pre-install all dependencies via pip, bypassing `uv`):

```pwsh
# Step 1: install ESP32-platform Python dependencies directly
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install `
    "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14" "pyyaml>=6.0.2" `
    "rich-click>=1.8.6" "zopfli>=0.2.2" "intelhex>=2.3.0" "rich>=14.0.0" `
    "cryptography>=45.0.3" "certifi>=2025.8.3" "ecdsa>=0.19.1" `
    "bitstring>=4.3.1" "reedsolo>=1.5.3,<1.8" "esp-idf-size>=2.0.0" `
    "esp-coredump>=1.14.0" "pyelftools>=0.32"

# Step 2: bootstrap the nested ESP-IDF venv (see next entry)
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m ensurepip --default-pip
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m pip install -r `
    "$env:USERPROFILE\.platformio\packages\framework-espidf\tools\requirements\requirements.core.txt"
```

After installing these packages, `get_packages_to_install()` in `penv_setup.py` returns empty, and pip-install via `uv` is not invoked. Subsequent `esphome compile` runs work normally.

The workaround must be reapplied after a fresh PlatformIO install or after clearing the `~/.platformio` cache.

---

### ESP-IDF venv: no `idf_component_manager` module

**What you see**:

```
ModuleNotFoundError: No module named 'idf_component_manager'
```

Appears on the first `esphome compile` on a machine where PlatformIO downloaded the ESP-IDF framework but did not run a full bootstrap of its requirements.

**What it means**: the ESP-IDF framework from PlatformIO is installed into a nested venv `~/.platformio/penv/.espidf-5.5.4/` (the version number may differ). This venv needs the `requirements.core.txt` from the framework package. When the `uv` workaround above is applied — step 2 bootstraps pip in the venv and installs the requirements.

If the error appears after applying the `uv` workaround but step 2 was skipped:

```pwsh
# Find the IDF venv (the version number may differ):
ls "$env:USERPROFILE\.platformio\penv\" | Where-Object { $_.Name -like ".espidf-*" }

# Bootstrap pip in the IDF venv:
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m ensurepip --default-pip

# Install IDF core requirements:
& "$env:USERPROFILE\.platformio\penv\.espidf-5.5.4\Scripts\python.exe" `
    -m pip install -r `
    "$env:USERPROFILE\.platformio\packages\framework-espidf\tools\requirements\requirements.core.txt"
```

Substitute your own IDF version number instead of `5.5.4` (check `~/.platformio/penv/`).

---

### `esphome compile` fails: `get_object_id_hash` not found

**What you see**:

```
error: 'class esphome::rbamp::RbAmpComponent' has no member named 'get_object_id_hash'
```

**What it means**: an old version of `rbamp.cpp` called `this->get_object_id_hash()` to seed the NVS slot. That method was removed from `PollingComponent` in ESPHome 2025.x.

**Fix**: update the component to the current version. Modern code uses hash formation compatible with ESPHome 2025.x and newer.

---

### ESPHome too old — `set_i2c_address` not found

**What you see**:

```
error: 'class esphome::i2c::I2CDevice' has no member named 'set_i2c_address'
```

**What it means**: the `new_address:` change flow calls `i2c::I2CDevice::set_i2c_address()`, added in ESPHome 2023.6.

**Fix**: upgrade to ESPHome 2023.6 or newer. The HA ESPHome Add-on tracks the current release automatically — the recommended install path.

```sh
# If using a local Python venv:
pip install --upgrade esphome
```


---

← [Schema Reference](09_api_reference.md) · [Docs index](README.md) · [Changelog](11_changelog.md) →
