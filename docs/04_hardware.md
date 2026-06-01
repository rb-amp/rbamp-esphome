# 04 · Wiring

This chapter covers the physical wiring of rbAmp to an ESP32: GPIO selection, pull-up rules, multi-module bus, power, the optional DRDY line, and address provisioning.

## LV-side pinout

The low-voltage side of the module exposes five signals. All are galvanically isolated from mains — the `GND` pin is the ESP32 GND, not mains neutral.

| Pin | Signal | Description |
|---|---|---|
| `VCC` | +4.5..+5.5 V DC | Module power. 5 V nominal; below 4.5 V the ADC may lose accuracy. |
| `GND` | Common | Shared with the master. Required. |
| `SDA` | I²C data | 3.3 V logic; 5 V-tolerant. |
| `SCL` | I²C clock | 3.3 V logic; 5 V-tolerant. |
| `DRDY` | Data ready (optional) | Open-drain output. A ~10 µs LOW pulse every ~200 ms after the instantaneous registers are updated. |

## GPIO selection on ESP32

Any pair of I²C-capable GPIOs will work. The ESP32 peripheral matrix lets I²C be routed to almost any pin. Practical constraints:

- **GPIO0** and **GPIO2** — boot-strapping pins. A low level on either at power-on blocks boot. Do not use.
- **GPIO 6..11** — wired to the internal flash or PSRAM on most ESP32-WROOM modules. Not available.
- **GPIO 34..39** — input-only. Not suitable for I²C (bidirectional drive required).
- Do not connect any ESP32 GPIO to a module reset line — the module is reset by an I²C command, no hardware reset pin on the master side is needed.

Recommended defaults that work on most ESP32-DevKitC boards:

| Signal | GPIO | Note |
|---|---|---|
| SDA | GPIO21 | Default in every example YAML |
| SCL | GPIO22 | Default in every example YAML |
| DRDY | GPIO15 | Optional; any input-capable pin works |

If GPIO21/22 are taken, pick any other valid pair and update the `i2c:` block in YAML.

## Single-module wiring

```
  ESP32                        rbAmp
  ------                       +-----------+
  GPIO22 (SCL) ────────────── SCL          |
  GPIO21 (SDA) ────────────── SDA          |
  5V           ────────────── VCC          |
  GND          ────────────── GND          |
                               |           |
  (DRDY optional)              |           |
  GPIO15 ◄──[10K to 3.3V]──── DRDY         |
                               +-----------+
```

External pull-up resistors are not needed for a single module — the rbAmp PCB carries built-in 4.7 kΩ pull-ups on SDA and SCL to 3.3 V.

## Multi-module bus

When two or more modules share a bus (or any other I²C devices with their own pull-ups), their pull-ups combine in parallel. The total resistance drops below the effective minimum, which causes:

- Excessive bus current draw.
- Possible overload of the ESP32 open-drain outputs (typically ≤ 3 mA at 3.3 V).
- Higher capacitive load on a long bus.

The underside of every rbAmp PCB carries solder jumpers labeled `Pull-Up`. Cutting the trace with a sharp blade disables the built-in 4.7 kΩ pair on that module.

| Modules on bus | Pull-up configuration | Effective R |
| --- | --- | --- |
| 1 | Keep built-ins. No externals. | ~4.7 kΩ |
| 2 | Keep on the first, cut on the second. | ~4.7 kΩ |
| 3 | Cut on all. External pair **2.7..3.3 kΩ** at the ESP32. | 2.7..3.3 kΩ |
| 4..6 | Cut on all. External pair **2.2..2.7 kΩ** at the ESP32. | 2.2..2.7 kΩ |
| 7..16 | Cut on all. External pair **1.5..2.2 kΩ** at the ESP32 + check bus capacitance (see below). | 1.5..2.2 kΩ |
| ESP32 already provides pull-ups | Cut on every module. The ESP32 internal pull-ups (≥50 kΩ) are weak — a long bus still needs external 2.2..4.7 kΩ. | as above |

**Selection logic**: the effective R_pullup is the parallel combination of every active pull-up. The target range is 1.5..4.7 kΩ (the floor is set by the ESP32 sink current ≤ 3 mA at 3.3 V; the ceiling by the bus RC time constant). Formula: 1/R_eff = N/R_unit. With the built-in 4.7 kΩ pair: 2 modules = 2.35 kΩ, 3 = 1.57 kΩ, 4 = 1.18 kΩ — at 3 modules the result is already too low, so the table cuts every built-in jumper starting at 3 modules and falls back to a single external pair.

**Diagnostic check**: measure the resistance between SDA and VCC with the power off. The result should be in the **1.5..4.7 kΩ** range. Below 1.0 kΩ — too many pull-ups in parallel; above 10 kΩ — no active pull-up (check the jumpers).

**Bus capacitance** (for ≥7 modules or total wiring length > 50 cm): the I²C spec caps total C_b at ≤ 400 pF. A short bus with 2-3 modules is typically 50..100 pF; a 1 m bus with 10 modules can climb to 300..400 pF. When approaching the limit, drop to `frequency: 50kHz` (if you were on 100 kHz) or use an active-pullup IC (for example PCA9517).

## Bus speed

Set `frequency: 50kHz` in the ESPHome `i2c:` block. On the current firmware this is **mandatory** for reliable operation with ESP-IDF v5:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz    # mandatory — do not use 100 kHz on ESP32 with v1 firmware
```

The module periodically NACKs reads from an ESP32 at 100 kHz (~20% of transactions) because of the interaction between the ESP-IDF v5 `i2c_master` driver timing and the post-STOP recovery window of the module's I²C peripheral (details in [`libs/spec/SPEC.md` Appendix B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)). At 50 kHz the NACK rate drops by about a factor of 5, and the per-byte retry inside the component recovers > 99% of valid reads.

Do not push to 100 kHz on ESP32 until the firmware ships the stop-handler hardening fix — once it does, the limit can be lifted with a single YAML line.

Other I²C masters (STM32 HAL, Raspberry Pi smbus2, Arduino UNO / Nano) are not affected and can run at 100 kHz.

## Reference schematic — one ESP32 + one rbAmp

```
                                         rbAmp
                                     +-------------------+
  ESP32                              |   LV side         | Mains side
  -----                              |                   |
  GPIO22 (SCL) ──────────────────── SCL                  |◄[terminal L]── L
  GPIO21 (SDA) ──────────────────── SDA                  |◄[terminal N]── N
  GPIO15 (input) ◄──[10K to 3.3V]── DRDY (open-drain)    |
  3.3V ─────────────────────────── (pull-up reference)   |◄[CT jack]── SCT-013
  5V ──────────────────────────── VCC                    |
  GND ─────────────────────────── GND                    |
                                     +-------------------+
```

Notes:

- The 10 kΩ resistor on DRDY belongs on the master side (near the GPIO), not on the module side. DRDY is open-drain with no internal pull-up.
- The CT jack must be fully inserted; the clamp must fully enclose a single conductor with the arrow pointing toward the load.
- L/N polarity on the terminals affects the sign of P (see [03_sensor_selection.md](03_sensor_selection.md)).

## Power

### Module power (VCC)

The module requires +4.5..+5.5 V DC. Current draw:

| Mode | Typical |
|---|---|
| Steady-state measurement | ~15 mA |
| Flash write (during `ct_model:` / address provisioning) | ~25 mA peak, ~700 ms |
| Boot warm-up (~250 ms) | ~15 mA |

The 5V pin on the ESP32-DevKitC (usually tied to USB VBUS) can power the module directly — the USB host must supply the combined ESP32 (~200 mA) plus rbAmp (~15 mA peak) = ~215 mA. For installs where the ESP32 is fed from an external 5 V SMPS, connect VCC to the same 5 V rail.

### ESP32 brownout

The ESP32 brownout detector trips at about 2.43 V on VDD. When it fires, the I²C bus goes idle while the module keeps measuring on its own (it has independent 5 V power). When the ESP32 recovers, `setup()` restores the accumulated energy from NVS before the first `update()`. Home Assistant sees neither a gap nor a reset in the energy graph.

## Optional DRDY

The DRDY pin on the module is open-drain. It pulses LOW for ~10 µs every ~200 ms immediately after the instantaneous-register block is refreshed. It does not fire on a period snapshot.

With `update_interval: 60s` you can omit DRDY — the component checks the status register at the start of every `update()` and skips the read if the module is not ready. DRDY is only useful when you need sub-second delivery of instantaneous data with minimal latency (interrupt-driven reads).

### DRDY wiring

DRDY is open-drain: it pulls LOW but cannot drive HIGH. An external pull-up is required:

```
  rbAmp DRDY  ──────────────────┬── ESP32 GPIO (input)
                                 |
                               [10K]
                                 |
                               3.3V
```

10 kΩ to 3.3 V. **Do not connect DRDY to 5 V**, even though SDA/SCL are 5 V-tolerant — DRDY is a separate signal and the absolute maximum on an ESP32 input GPIO is 3.6 V.

Declare the pin in YAML:

```yaml
rbamp:
  id: meter1
  drdy_pin: GPIO15
```

### Multi-module DRDY

DRDY signals from several modules can be wire-OR'd onto a single ESP32 GPIO (open-drain outputs combine naturally). The shared signal goes LOW when **any** module refreshes its instantaneous registers. The component uses this only as a hint and does not distinguish which module fired.

If you need separate signals, route each module's DRDY to its own GPIO and declare `drdy_pin:` separately in each `rbamp:` block.

## Address provisioning on a multi-module bus

All rbAmp modules ship from the factory at address `0x50`. Before connecting several modules to one bus, every module needs a unique address.

Serial provisioning:

1. Connect only module 1 to the ESP32 (one module on the bus).
2. Flash the configuration from [`example/address_change.yaml`](../example/address_change.yaml) with `address: 0x50` and `new_address: 0x51`. At boot the component detects the mismatch and runs the address-change sequence: write the new address, save to flash (~700 ms), soft reset (~300 ms), verify on the new address.
3. Confirm success in the ESPHome logs. Update the YAML: `address: 0x51`, remove `new_address:`.
4. Disconnect module 1. Connect module 2 (still at factory `0x50`).
5. Repeat for `0x52`, `0x53`, and so on.
6. Once every module has a unique address, connect them all to the bus. Cut the pull-up jumpers on all but one (see the rules above).

Serial provisioning avoids address collisions during the change. Writing a new address requires **factory-provisioning mode** on the module side — see the dedicated section below.

## Multi-module YAML example

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

rbamp:
  - id: meter_house
    address: 0x50
    update_interval: 60s
  - id: meter_solar
    address: 0x51
    update_interval: 60s
    bidirectional: true
  - id: meter_evcharger
    address: 0x52
    update_interval: 60s

sensor:
  - platform: rbamp
    rbamp_id: meter_house
    voltage: { name: "House Voltage" }
    current: { name: "House Current" }
    power:   { name: "House Power" }
    energy:  { name: "House Energy" }
  - platform: rbamp
    rbamp_id: meter_solar
    voltage: { name: "Solar Voltage", internal: true }
    current: { name: "Solar Current" }
    power:   { name: "Solar Power" }
    energy:  { name: "Solar Energy" }
    energy_exported: { name: "Solar Energy Exported" }
  - platform: rbamp
    rbamp_id: meter_evcharger
    voltage: { name: "EV Charger Voltage", internal: true }
    current: { name: "EV Charger Current" }
    power:   { name: "EV Charger Power" }
    energy:  { name: "EV Charger Energy" }
```

A fully annotated version with `broadcast_latch:` forward-compat is in [`example/multi_module.yaml`](../example/multi_module.yaml).

## Bus length

I²C is a short-haul protocol. Verified lengths for rbAmp:

| Cable type | Maximum length | Speed |
|---|---|---|
| Standard JST / 4-conductor flat | 0.3 m | 50 kHz |
| Twisted pair (SDA+GND and SCL+GND in separate pairs) | 1 m | 50 kHz |
| Twisted pair + I²C buffer (PCA9515 / TCA9617) | 3 m | 50 kHz |
| Differential bus (PCA9615 / LTC4332) | 100 m | 50 kHz |

For runs longer than 0.3 m, use twisted pair, and route SDA and SCL in **separate** pairs (for example blue + blue-white for SDA, green + green-white for SCL). If SDA and SCL share a pair, cross-coupling distorts the edges even at 50 kHz.

## Factory-provisioning mode

Some module operations (I²C address change, factory reset) require the module to be in **factory-provisioning mode** — a special mode for one-shot provisioning actions. Standard production modules ship with provisioning **disabled**: an address-change attempt against them returns an error visible in the ESPHome logs as a `WARNING` at boot.

If you need to change an address in the field, consult the module documentation or your supplier for the procedure that temporarily enables provisioning mode. Once provisioning is done, the module returns to normal production mode.

> Configuring the current sensor (`ct_model:` / `ct_models:`) does **not** require factory-provisioning mode — on firmware v1.2+ the module accepts CT-model writes directly (see [03_sensor_selection.md](03_sensor_selection.md)).

## What's next

- [05_quickstart.md](05_quickstart.md) — first boot in 5 minutes
- [06_examples.md](06_examples.md) — full scenarios: UI1 / UI3 / multi-module / address provisioning
- [10_troubleshooting.md](10_troubleshooting.md) — symptom-driven diagnostics



---

← [Sensor Selection](03_sensor_selection.md) · [Docs index](README.md) · [Quickstart](05_quickstart.md) →
