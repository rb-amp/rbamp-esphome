# rbAmp ESPHome component — documentation

Declarative rbAmp energy-monitoring integration for ESPHome. Wire the I²C module to
two ESP32 GPIOs, describe it in YAML, and Home Assistant sees ready-to-use entities
for voltage, current, power, energy, frequency and power factor.

| # | Document | Contents |
|---|---|---|
| 01 | [Overview](01_overview.md) | What rbAmp is, what the component does, data flow, NVS design, NACK discipline |
| 02 | [Module Tiers](02_tiers.md) | BASIC / STANDARD / PRO lineups and the YAML keys each exposes |
| 03 | [Sensor Selection](03_sensor_selection.md) | Choosing a CT clamp; `ct_model` / `ct_models` behavior |
| 04 | [Wiring](04_hardware.md) | GPIO pinout, pull-ups, multi-module bus, DRDY, address provisioning |
| 05 | [Quickstart](05_quickstart.md) | First flash in five minutes; boot log; HA discovery |
| 06 | [Examples](06_examples.md) | YAML cookbook: UI1 / UI3 / multi-module / provisioning + automations |
| 07 | [DIY Integrations](07_diy_integrations.md) | MQTT, InfluxDB / Grafana, Lambda actions beyond HA |
| 08 | [Home Assistant](08_has_integrations.md) | Native API, Energy dashboard, statistics, Lovelace |
| 09 | [Schema Reference](09_api_reference.md) | Every YAML key, type, default, side effect |
| 10 | [Troubleshooting](10_troubleshooting.md) | Symptom-driven debugging: NACK, OTA, NVS, toolchain |
| 11 | [Changelog](11_changelog.md) | Version history, upgrade guide |

Runnable configs live in [`../example/`](../example/). The rbAmp wire-protocol
specification (registers, commands, errors, NACK discipline) is published at
<https://rbamp.com/docs/modules-basic-standard-api-reference>.
