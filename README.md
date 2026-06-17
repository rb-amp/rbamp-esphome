# rbAmp — ESPHome external component

ESPHome external component for the **rbAmp** I²C AC energy monitor. Read RMS voltage, current,
active/reactive power, power factor, line frequency and tariff energy from one or several rbAmp
modules on a shared I²C bus — as native Home Assistant sensors, with no `lambda:` math required.

- **Native HA entities** — `voltage · current · power · power_factor · frequency · energy` with
  `device_class` / `state_class` / units preset; the `energy` sensor drops straight into the HA Energy Dashboard.
- **Multi-channel** — UI3 modules expose per-channel current/power; mixed CT models per channel.
- **Multi-module fleet** — declare several `rbamp:` blocks on one ESP32; optional **General-Call latch**
  gives billing-grade synchronized accumulation across sub-meters.
- **v1.3 protocol** — two-phase address commit, read-compare-write boot writeback, capability-gated
  feature detection, NVS energy persistence. Older firmware gracefully falls back to legacy paths.

## Requirements

- ESPHome **>= 2025.3.0**
- ESP32 (ESP-IDF framework recommended; Arduino framework also supported)
- One or more I²C-connected rbAmp modules (firmware v1.0+; **v1.3+ recommended** for fleet GC sync,
  two-phase address commit, and read-compare-write boot writeback)

## Install

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/rb-amp/rbamp-esphome
      ref: v1.3.0
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

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"
```

See [`example/`](example/) for UI3 mixed-CT (`ui3.yaml`), a synchronized multi-module fleet
(`multi_module.yaml`), and one-shot address provisioning (`address_change.yaml`).

## Documentation

Full guides — quickstart, sensor selection, hardware, multi-module fleets, Home Assistant
integration, API reference, troubleshooting:
**https://www.rbamp.com/docs/modules-basic-standard-esphome-overview**

## Testing

[`tests/`](tests/) ships a smoke config and a bench harness (`bench_v13.yaml`) for users who want to
hardware-validate locally.

## License

MIT — see [LICENSE](LICENSE).

## Changelog

- **v1.3.0** — initial public release. ESPHome external component for the rbAmp v1.3 wire protocol:
  multi-channel + multi-module fleet with general-call latch synchronization, two-phase address
  commit, read-compare-write boot writeback, capability-gated feature detection. Validated 21/21 on
  bench hardware (mixed UI1/I2/I3 fleet).
