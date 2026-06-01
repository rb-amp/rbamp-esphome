# esphome-rbamp — ESPHome external component for the rbAmp module

[![protocol: 1.2](https://img.shields.io/badge/protocol-1.2-blue)](docs/02_tiers.md)
[![esphome: 2024.6+](https://img.shields.io/badge/esphome-2024.6%2B-brightgreen)](docs/05_quickstart.md)
[![license: MIT](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

Declarative rbAmp integration for ESPHome. Wire the I²C module to two ESP32 GPIOs, describe it in YAML — and Home Assistant immediately sees ready-to-use entities for voltage, current (1–3 channels), active/reactive/apparent power, power factor, frequency, and accumulated Wh energy.

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz

rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s

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
```

Compile, flash — and the sensors appear in HA within a few seconds of boot. There is no code to write: the entire configuration lives in YAML, and `device_class` / `state_class` for the Energy dashboard are filled in automatically.

## Installation

### Via GitHub (HA ESPHome Add-on + local CLI)

The recommended form for most users — the HA ESPHome Add-on has no shell access to the
file system, so the GitHub form works identically in both environments:

```yaml
external_components:
  - source: github://rb-amp/rbamp-esphome@main
    components: [rbamp]
    refresh: 1d   # 1 day — for production. Use `0s` while you are
                  # actively iterating on @main or your own fork.
```

### Local installation (development only)

If you have cloned this repository for development and want to point ESPHome at a
local path:

```yaml
external_components:
  - source:
      type: local
      path: <absolute-path-to>/rbamp-esphome/components
    components: [rbamp]
```

> ⚠ **A local path does not work in the HA ESPHome Add-on.** The Add-on keeps YAML in
> `/config/esphome/` with no shell access to sibling directories. Add-on users should
> use the GitHub form above.

### Minimum ESPHome version

**ESPHome ≥ 2024.6** — the component uses the modern `ota:` syntax (a list of platforms),
available from this version. If you need to work with earlier ESPHome releases, use the
legacy scalar form of `ota:` (see the ESPHome docs).

## What you get

- **HA-native sensors out of the box** — every slot already has the correct `device_class`, `state_class`, and units set. The `energy` sensor lands in the Home Assistant Energy dashboard immediately, with no extra configuration.
- **Wh persistence across reboots** — per-channel total energy is written to the ESP32's NVS flash every 5 minutes and restored before the first `publish_state`. A reboot or an OTA update does not reset the counter and does not leave a gap in the Energy dashboard history.
- **Resilience on a noisy I²C bus** — a three-layer discipline is implemented: 50 kHz bus by default, per-byte retry with a 5 ms pause, and a soft sanity filter on NaN/Inf. Occasional NACKs on a busy bus do not produce spikes in the HA charts.
- **Up to 16 modules on one ESP32** — declare several `rbamp:` blocks via MULTI_CONF: each with its own address, its own `id`, and its own sensors. One ESP32 polls them all in turn and publishes them under different HA names.
- **One-shot I²C address provisioning** — the `new_address:` key changes the module's address straight from YAML at startup. After one boot cycle the key is dropped and the module remembers the new address in flash. Handy for bulk-configuring a batch of devices.
- **Current-sensor configuration** — the `ct_model:` key accepts one of 5 preconfigured SCT-013 profiles (-005 / -010 / -030 / -050 / -100). On firmware v1.2+ the module loads the factory coefficients for the chosen model itself. For mixed UI3 installations with different clamps on different channels, use the `ct_models:` key (an array `[SCT_013_005, SCT_013_030, SCT_013_100]`): a low-current clamp on one channel, the main load on the others, all in a single YAML block.
- **Sensor class** — the `sensor_class:` key fixes the family (default `SCT_013`). On firmware v1.2+ this is a precondition for writing the CT model; on earlier firmware the value is accepted by the schema and applied automatically on upgrade.
- **Topology hint** — the `topology:` key (SINGLE / SPLIT_PHASE / THREE_PHASE) declares the module's physical configuration. On current firmware it is cosmetic (the component derives the channel count from YAML itself); it becomes authoritative once the module publishes its topology through a register.
- **DRDY-pin support** — the optional `drdy_pin:` key names the GPIO the module drives HIGH when a measurement period is ready. It reduces I²C traffic: the ESP32 does not poll the module until the ready signal arrives.
- **Bidirectional accounting** *(scaffolded)* — the `bidirectional:` flag is accepted by the schema and reserved for planned firmware revisions with export energy. On current firmware it carries no semantic weight.

## Supported platforms

| Host | Support | Notes |
|---|---|---|
| ESP32 (classic) | ✅ Full | Primary target platform |
| ESP32-S2 / S3 / C3 | ✅ Full | The I²C-master driver is identical; same discipline |
| ESP8266 | ⚠ Limited | I²C is bit-banged; the sanity filter works and retry makes sense, but 50 kHz is a recommendation, not a guarantee |
| RP2040 / SAMD | ❌ Not supported | ESPHome does not natively target these platforms — use the Arduino / Python libraries instead |

### Choosing `framework:` (arduino vs esp-idf)

| Framework | State | When to choose |
|---|---|---|
| `arduino` (default) | ✅ Production | The default. The Wire stack behaves predictably under NACK; the component is validated on this framework across all of our bench tests. |
| `esp-idf` | 🧪 Experimental | Lower RAM usage (-40..60 KB), faster boot. It works, but the `i2c_master` driver shows a NACK symptom with ghost values at 100 kHz (mitigated at 50 kHz, see [the wire-protocol spec §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)). If you choose it, `frequency: 50kHz` in the `i2c:` block is mandatory. More detail in [docs/10_troubleshooting.md §1](docs/10_troubleshooting.md). |

In most deployments, leave the default `framework: type: arduino`. Move to `esp-idf` only
if RAM becomes a bottleneck (many parallel components on one node) and you are prepared to
keep `50kHz`.

## Examples

Ready-to-run YAML configs live in [`example/`](example/):

| File | Purpose |
|---|---|
| [`ui1.yaml`](example/ui1.yaml) | Single-channel deployment — the minimum for a first run |
| [`ui3.yaml`](example/ui3.yaml) | Three-channel installation with every sensor type |
| [`multi_module.yaml`](example/multi_module.yaml) | Three rbAmp modules on one ESP32 bus |
| [`address_change.yaml`](example/address_change.yaml) | One-shot I²C address provisioning for a new device |

Every file compiles with `esphome compile <name>.yaml` without additional configuration.

## Documentation

| Document | Contents |
|---|---|
| [01 · Overview](docs/01_overview.md) | What rbAmp is, what the component does, data flow, NVS design, NACK discipline |
| [02 · Tiers (BASIC / STANDARD / PRO)](docs/02_tiers.md) | Module lineups and the YAML keys available to each |
| [03 · Current-sensor selection](docs/03_sensor_selection.md) | Choosing a CT clamp and the behavior of the `ct_model:` key |
| [04 · Wiring](docs/04_hardware.md) | GPIO pinout, pull-ups, multi-module bus, bench setup |
| [05 · Quickstart](docs/05_quickstart.md) | First flash in five minutes, a walk through the boot log, HA discovery |
| [06 · Examples](docs/06_examples.md) | YAML cookbook: UI1 / UI3 / multi-module / address provisioning + automations |
| [07 · DIY integrations](docs/07_diy_integrations.md) | Beyond HA: MQTT, InfluxDB / Grafana, Lambda actions |
| [08 · Home Assistant deep dive](docs/08_has_integrations.md) | Native API, Energy dashboard, automations, Lovelace cards |
| [09 · YAML schema reference](docs/09_api_reference.md) | Every key, type, default, and side effect |
| [10 · Troubleshooting](docs/10_troubleshooting.md) | Symptom-driven debugging: NACK, OTA, NVS, toolchain |
| [11 · Changelog](docs/11_changelog.md) | Version history, upgrade guide |

The wire-protocol specification is published at
<https://rbamp.com/docs/modules-basic-standard-api-reference>.

## Compatibility

The component targets **rbAmp protocol v1.0 / v1.1 / v1.2** with transparent backward compatibility.

| Component version | Firmware version | Behavior |
|---|---|---|
| 0.3.x | 1.0 / 1.1 / 1.2 | Base sensors work. `ct_model:` stores the model marker in the module. |
| 0.4.0+ | 1.0 / 1.1 | `sensor_class:` and `ct_models:` are accepted by the schema and ignored on the module side — forward compatibility with no functional regression. |
| 0.4.0+ | 1.2 | Full parity: `sensor_class:` fixes the sensor family, `ct_models:` loads factory coefficients for each channel independently. |

## Sister libraries

This component is part of the cross-platform rbAmp family:

- **Arduino** — [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino)
- **ESP-IDF** — [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf) *(coming soon)*
- **Python (CPython + MicroPython)** — [`rbamp-python`](https://github.com/rb-amp/rbamp-python) *(coming soon)*
- **STM32 HAL** — [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(coming soon)*

All libraries implement the same wire protocol and work with the same module without reconfiguration.

## License

MIT — see [LICENSE](LICENSE).
