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
- **Both module generations on one YAML** — the component autodetects the new 16-bit register-map
  modules at boot and takes channel counts and features from the device itself; on modules with
  on-device energy counters the totals come straight from the device and survive restarts of both
  sides (grand-total sensors `energy_total` / `energy_net` / `reactive_energy` and the
  `start_current_ua:` key included).
- **Factory calibration stays safe** — the component never rewrites a sensor class or CT model the
  device already has, refuses changes on factory-paired modules, and when you switch an SCT module
  to a different clamp rating it rescales readings from the gain the module actually applies, so
  the per-unit factory accuracy carries over to the new sensor.

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

[`tests/`](tests/) ships a smoke config and a host-side test for the SCT rating-correction logic
for users who want to validate locally.

## License

MIT — see [LICENSE](LICENSE).

## Changelog

- **v1.9.0** — SCT rating-switch correction: switching `ct_model:` on an SCT module keeps the
  factory per-unit accuracy (scales from the applied gain; firmware 1.4.20+ does it on-device and
  is detected by capability bit). See [CHANGELOG.md](CHANGELOG.md).
- **v1.8.1** — sensor_class/ct_model write guard (factory-paired modules protected,
  `ct_model_force:` service key), UI5/UI7 out-of-bounds read fix.
- **v1.8.0** — on-device cumulative energy for the 16-bit line (grand-total sensors, µWh/µvarh
  with coarse fallback, `start_current_ua:`, voltage angles + phase sequence on 3φ).
- **v1.7.0** — 16-bit register-map module generation autodetected on the same YAML; energy via
  device latch snapshots; restart auto-recovery; NVS flush on clean shutdown.
- **v1.6.0** — per-channel `ct_models:` for all channels (0..15), sensor-model registry, persist
  fix (CT model binding now survives reboot on production modules). Firmware: v1.4.18+.
- **v1.5.0** — UI5/UI7 senior-SKU support, expanded per-channel period energy.
- **v1.4.0** — senior-SKU detection (5/7-channel variants), channel-access window for RT metrics.
- **v1.3.0** — initial public release. Multi-channel + multi-module fleet with general-call latch
  synchronization, two-phase address commit, read-compare-write boot writeback, capability-gated
  feature detection.
