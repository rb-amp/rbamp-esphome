# rbAmp — ESPHome external component

ESPHome external component for the **rbAmp** I²C AC energy monitor. Read RMS voltage, current,
active and reactive power (reactive power is unsigned, computed as √(S² − P²), includes distortion; no accuracy stated), power factor, line frequency and energy from one or several rbAmp
modules on a shared I²C bus — as native Home Assistant sensors, with no `lambda:` math required.
One YAML works for both module lines (BASIC and STANDARD (STD)); the component identifies the
connected module at boot. BASIC = consumption-only metering, energy integrated on the ESP32;
STANDARD (STD) = import and export metered separately, energy kept in counters inside the module
(see [02 Module tiers](docs/02_tiers.md)).

> **Words used in these docs**
> - **Module line** — BASIC or STANDARD (STD). To see which you have, check the boot log: a STANDARD (STD)
>   module logs a line starting `STD module:`; a BASIC module logs `Topology:` and no `STD module:` line.
> - **CT clamp** — the split-core current sensor that clips around one wire; its rating (e.g. 30 A)
>   is printed on the clamp label.
> - **Period / snapshot** — the module closes a measuring period on a snapshot command; the component uses the
>   period average to add up energy (BASIC) or reads the module's counters (STANDARD (STD)).
> - **NVS** — the ESP32's flash storage; energy is saved there so it survives a reboot.
> - **DRDY** — optional "data ready" pin of the module; you can leave it unconnected.

- **Native HA entities** — `voltage · current · power · power_factor · reactive_power · apparent_power ·
  frequency · energy` with `device_class` / `state_class` / units preset; the `energy` sensor drops
  straight into the HA Energy Dashboard.
- **Multi-channel** — per-channel `current[_1.._6]`, `power[_N]`, `energy[_N]`, `power_factor[_N]`;
  mixed CT models per channel with `ct_models:`.
- **Both module lines on one YAML** — the component identifies BASIC and STANDARD modules at
  boot and takes channel count, variant and features from the module itself; no `topology:` key and
  no tier setting are needed.
- **On-device energy on STANDARD modules** — the module keeps its own cumulative energy counters; the
  totals survive restarts of both the ESP32 and the module and stay monotonic. Sensors:
  `energy_total`, `energy_exported_total`, `energy_net`, `reactive_energy`, `reactive_energy_exported`,
  per-channel `energy` / `energy_exported`; on three-voltage variants also `voltage_angle_b`,
  `voltage_angle_c`, `phase_sequence`. `start_current_ua:` (0 or 10..2540 µA in steps of 10) keeps
  noise at no load out of the counters.
- **Energy persistence** — energy is saved to the ESP32 NVS every 5 minutes and on a clean shutdown,
  and restored before the first publish, so Home Assistant never sees a false counter reset.
- **Multi-module fleet** — declare several `rbamp:` blocks on one ESP32; on BASIC modules the optional
  **General-Call snapshot** (`fleet_gc_enable:` + `group_id:`) synchronizes accumulation across sub-meters.
- **Safe configuration** — two-phase I²C address change (`new_address:`), read-compare-write boot
  writeback (an equal value is never written), schema-validated keys (`update_interval` >= 1 s,
  `start_current_ua`, `ct_model_force` only with a model key).
- **Factory calibration stays safe** — the component refuses sensor-class / CT-model changes on
  modules calibrated as a pair with their fitted sensor (with an explicit log line;
  `ct_model_force: true` is the deliberate service override), and when you switch an SCT module to a
  different clamp rating the factory per-unit accuracy carries over to the new sensor (a module
  restart in the flow is normal on firmware up to 1.4.19; newer firmware does it on the module).

## Requirements

- ESPHome **>= 2025.3.0**
- ESP32 (ESP-IDF framework recommended; Arduino framework also supported)
- One or more I²C-connected rbAmp modules (BASIC firmware v1.0+, **v1.3+ recommended** for fleet GC sync,
  two-phase address commit and read-compare-write boot writeback; or STANDARD modules)
- I²C bus at `frequency: 50kHz` on ESP32 (see [wiring](docs/04_hardware.md))

## Install

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/rb-amp/rbamp-esphome
      ref: v1.9.0
    components: [rbamp]
```

## Quick start (single UI1 module)

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz

rbamp:
  id: meter1
  address: 0x50
  ct_model: SCT_013_030     # match your clamp: read the rating (e.g. 030 = 30 A) off the clamp label

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
    current:                 # power and energy need voltage AND current in the same block
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"
```

> **DANGER:** the L/N terminals carry mains voltage. De-energise and lock out the circuit before
> connecting, have a qualified person do mains wiring and any work inside a panel, clamp ONE
> conductor only (L), and never leave the CT clamp on a conductor with the CT jack unplugged.
> Wiring details: [04 Wiring](docs/04_hardware.md).

On a STANDARD module add the on-device energy totals (no other change):

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    energy_total:
      name: "Energy Imported"
    energy_exported_total:
      name: "Energy Exported"
    energy_net:
      name: "Energy Net"
```

See [`example/`](example/) for UI3 mixed-CT (`ui3.yaml`), a synchronized multi-module fleet
(`multi_module.yaml`), and one-shot address provisioning (`address_change.yaml`).

## Examples

Ready-to-use YAML configurations under [`example/`](example/). Adjust the board, Wi-Fi credentials (`!secret`) and GPIO pins to your hardware. More recipes: [06 Examples](docs/06_examples.md).

| File | What it shows |
|---|---|
| [`ui1.yaml`](example/ui1.yaml) | minimal configuration: one voltage and one current channel |
| [`ui3.yaml`](example/ui3.yaml) | voltage and three current channels, all single-phase fields |
| [`multi_module.yaml`](example/multi_module.yaml) | three modules on one bus with a shared General-Call snapshot (`fleet_gc_enable`, `group_id`) |
| [`address_change.yaml`](example/address_change.yaml) | one-shot I²C address change at boot |

## Documentation

Full guides — quickstart, sensor selection, hardware, multi-module fleets, Home Assistant
integration, API reference, troubleshooting:
**https://www.rbamp.com/docs/modules-basic-standard-esphome-overview**

The same chapters are in this repository, in [`docs/`](docs/):

| Chapter | Contents |
|---|---|
| [01 Overview](docs/01_overview.md) | What the component does, data flow, energy persistence |
| [02 Module tiers](docs/02_tiers.md) | BASIC and STANDARD modules, YAML keys per module line |
| [03 Sensor selection](docs/03_sensor_selection.md) | CT clamp models, rating switch, write guards |
| [04 Wiring](docs/04_hardware.md) | Pinout, pull-ups, multi-module bus, address provisioning |
| [05 Quickstart](docs/05_quickstart.md) | First flash, boot log, Home Assistant discovery |
| [06 Examples](docs/06_examples.md) | YAML cookbook |
| [07 DIY integrations](docs/07_diy_integrations.md) | MQTT, InfluxDB / Grafana, lambdas |
| [08 Home Assistant](docs/08_has_integrations.md) | Native API, Energy dashboard, Lovelace |
| [09 Schema reference](docs/09_api_reference.md) | Every YAML key, type, range, default |
| [10 Troubleshooting](docs/10_troubleshooting.md) | Symptom-driven debugging |
| [11 Changelog](docs/11_changelog.md) | Version history |

Register-level (native) protocol documentation, for integrations without this library: [rbamp-docs](https://github.com/rb-amp/rbamp-docs).

## Testing

[`tests/`](tests/) ships a smoke config and a host-side test for the SCT rating-correction logic
for users who want to validate locally.

## License

MIT — see [LICENSE](LICENSE).

## Changelog

- **v1.9.0** — SCT rating-switch correction: switching `ct_model:` on an SCT module keeps the
  factory per-unit accuracy (scales from the applied gain; firmware 1.4.20+ does it on-device and
  is detected automatically). Public releases resume with this version; it includes the 1.7.x and
  1.8.x changes below. See [CHANGELOG.md](CHANGELOG.md).
- **v1.8.1** — sensor_class/ct_model write guard (factory-paired modules protected,
  `ct_model_force:` service key), UI5/UI7 out-of-bounds read fix.
- **v1.8.0** — on-device cumulative energy for the 16-bit line (grand-total sensors, µWh/µvarh
  with coarse fallback, `start_current_ua:`, voltage angles + phase sequence on 3φ).
- **v1.7.0** — 16-bit register-map module line autodetected on the same YAML; energy via
  device snapshots; restart auto-recovery; NVS flush on clean shutdown.
- **v1.6.0** — per-channel `ct_models:` for all channels (0..15), sensor-model registry, persist
  fix (CT model binding now survives reboot on production modules). Firmware: v1.4.18+.
- **v1.5.0** — UI5/UI7 senior-SKU support, expanded per-channel period energy.
- **v1.4.0** — senior-SKU detection (5/7-channel variants), channel-access window for RT metrics.
- **v1.3.0** — initial public release. Multi-channel + multi-module fleet with general-call latch
  synchronization, two-phase address commit, read-compare-write boot writeback, capability-gated
  feature detection.
