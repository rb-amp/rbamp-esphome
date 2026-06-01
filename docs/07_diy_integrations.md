# 07 · DIY integrations

This chapter is about using rbAmp beyond the standard HA Energy dashboard: exporting to external time-series databases, publishing over MQTT without HA, closed-loop load control, and reading rbAmp from non-ESPHome masters.

Cross-references:

- YAML schema: [`09_api_reference.md`](09_api_reference.md)
- HA-specific topics: [`08_has_integrations.md`](08_has_integrations.md)
- Arduino client library: [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino)
- ESP-IDF client library: [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf)
- Python (CPython + MicroPython): [`rbamp-python`](https://github.com/rb-amp/rbamp-python)
- STM32 HAL: [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(in development)*

---

## 1 · InfluxDB and Grafana via Home Assistant

The simplest path to InfluxDB + Grafana requires no YAML changes. Home Assistant itself exports all entity states to InfluxDB through the native integration:

```yaml
# configuration.yaml in HA
influxdb:
  host: 192.168.0.100    # your InfluxDB host
  port: 8086
  database: homeassistant
  default_measurement: state
  include:
    entities:
      - sensor.rbamp_ui1_mains_voltage
      - sensor.rbamp_ui1_mains_current
      - sensor.rbamp_ui1_mains_power
      - sensor.rbamp_ui1_mains_energy
      - sensor.rbamp_ui1_mains_power_factor
      - sensor.rbamp_ui1_mains_frequency
```

Every rbAmp sensor publication (once per `update_interval`) becomes a point in InfluxDB. Grafana then queries InfluxDB and renders dashboards.

A minimal Grafana panel for live power:

```
SELECT mean("value") FROM "state"
WHERE "entity_id" = 'sensor.rbamp_ui1_mains_power'
AND $timeFilter
GROUP BY time(1m)
```

For InfluxDB 2.x (Flux):

```
from(bucket: "homeassistant")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r["entity_id"] == "sensor.rbamp_ui1_mains_power")
  |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
```

### High-resolution InfluxDB (bypassing HA)

The HA state machine only records state changes. For a dense series with a point every 60 s (the default `update_interval`) HA is fine. If you need sub-minute resolution — lower `update_interval` and let HA recorder + the InfluxDB integration capture at that rate:

```yaml
rbamp:
  id: meter1
  update_interval: 10s     # 10-second resolution — 6× I²C traffic, acceptable
```

---

## 2 · Direct MQTT publish (no HA)

ESPHome ships with a built-in `mqtt:` component that publishes all sensor states to a broker. rbAmp sensors are published on every `update_interval` like any other.

```yaml
esphome:
  name: rbamp-mqtt

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: INFO

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

# NOTE: use mqtt: OR api: — not both at once, unless you actually need
# both HA native API and MQTT in parallel (supported but unusual).
mqtt:
  broker: 192.168.0.200      # IP of your MQTT broker
  port: 1883
  username: !secret mqtt_user
  password: !secret mqtt_pass
  topic_prefix: rbamp/ui1    # all sensor topics under this prefix
  birth_message:
    topic: rbamp/ui1/status
    payload: online
  will_message:
    topic: rbamp/ui1/status
    payload: offline

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  ct_model: SCT_013_030

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
    power_factor:
      name: "Mains Power Factor"
```

With `topic_prefix: rbamp/ui1`, ESPHome publishes:

| MQTT topic | Payload |
|---|---|
| `rbamp/ui1/sensor/mains_voltage/state` | `226.70` |
| `rbamp/ui1/sensor/mains_current/state` | `0.755` |
| `rbamp/ui1/sensor/mains_power/state` | `92.80` |
| `rbamp/ui1/sensor/mains_energy/state` | `1024.341` |
| `rbamp/ui1/sensor/mains_frequency/state` | `50` |
| `rbamp/ui1/sensor/mains_power_factor/state` | `0.542` |
| `rbamp/ui1/status` | `online` / `offline` |

Any MQTT subscriber — Node-RED, InfluxDB Telegraf, OpenHAB, a custom Python script — can consume these topics directly.

### MQTT retained messages

Add `retain: true` per sensor if subscribers connecting after publication should immediately see the last value:

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
      # ESPHome MQTT does not set retain on sensor state by default. For
      # retain, use a manual publish action:
      on_value:
        - mqtt.publish:
            topic: "rbamp/ui1/voltage"
            payload: !lambda 'return to_string(x);'
            retain: true
```

### MQTT retain and OTA — what to know

Retained messages survive an ESP32 restart (that is their core purpose).
This creates two scenarios worth knowing about up front:

1. **OTA update of the node**. After flashing, the ESP32 reboots. Old
   retained values stay on the broker and remain visible to subscribers
   for ~5..30 seconds (until the node reconnects and publishes new
   ones). If the node never comes back (corrupted firmware, fatal
   crash on boot) — the stale retained values stay visible
   **indefinitely**. This masks the offline state from dashboards that
   only read `state` without `status`.

   **Mitigation**: always set a `will_message:` (LWT) on the `status`
   topic (as in the example above). The broker will set the payload
   to `offline` when the connection drops; subscribers can watch
   `status` to detect stuck nodes. Sensor topics will keep their old
   values — that is fine, but subscribers now know the data is stale.

2. **Changing `topic_prefix:` between flashes**. If you rename
   `topic_prefix:` (e.g. `rbamp/ui1` → `rbamp/kitchen`) and update the
   node over OTA — retained messages under the **old** prefix stay on
   the broker forever. Subscribers unaware of the rename will keep
   seeing stale data on zombie topics.

   **Mitigation**: before flashing OTA with a renamed prefix, clean the
   old retained messages by publishing an empty payload with
   `retain: true` on each old topic (CLI:
   `mosquitto_pub -h <broker> -t '<old>' -r -n`). Without that, the
   zombies hang around until someone clears them by hand.

3. **Birth message and `start_session: true`** for connection after
   broker restart — see the ESPHome `mqtt:` block documentation for
   the `clean_session:` and `keepalive:` options. These parameters
   control how the broker handles pending QoS 1/2 messages on
   disconnect.

---

## 3 · ESPHome Lambda actions

The ESPHome `on_value` callback fires on every new sensor reading. You can use it for threshold actions executed entirely on the ESP32 with no HA round-trip.

### Current threshold alarm

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    current:
      name: "Mains Current"
      on_value:
        then:
          - if:
              condition:
                lambda: 'return x > 15.0f;'    # 15 A overload threshold
              then:
                - logger.log:
                    level: WARN
                    format: "Over-current: %.2f A — firing alarm relay"
                    args: [x]
                - switch.turn_on: alarm_relay
```

### Load-step detection (custom Lambda)

```yaml
# Detecting a sharp load increase (e.g. compressor startup)
globals:
  - id: prev_power
    type: float
    initial_value: '0.0'

sensor:
  - platform: rbamp
    rbamp_id: meter1
    power:
      name: "Mains Power"
      on_value:
        then:
          - lambda: |-
              float delta = x - id(prev_power);
              if (delta > 500.0f) {
                ESP_LOGI("rbamp", "Large load step detected: +%.0f W", delta);
              }
              id(prev_power) = x;
```

### Publishing to a custom MQTT topic on threshold

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    power:
      name: "Mains Power"
      on_value:
        - if:
            condition:
              lambda: 'return x > 2500.0f;'
            then:
              - mqtt.publish:
                  topic: "alerts/high_power"
                  payload: !lambda >
                    return "HIGH_POWER:" + to_string((int)x) + "W";
```

---

## 4 · Closed-loop control — PID water heater

A typical use case for residential rbAmp metering is solar self-consumption: heat water with excess solar power instead of exporting it to the grid.

The pattern uses two rbAmp modules: one on the grid feed (import / export), one on the water heater circuit. A PWM relay or SSR on the heater enables variable power control.

```yaml
# Conceptual sketch — the actual PID implementation depends on your
# relay / SSR and load. The ESPHome PID controller component is the
# recommended building block.

globals:
  - id: solar_export_w
    type: float
    initial_value: '0.0'
  - id: heater_setpoint_w
    type: float
    initial_value: '0.0'

rbamp:
  - id: meter_grid
    address: 0x50
    update_interval: 10s     # tighter loop for responsive control

  - id: meter_heater
    address: 0x51
    update_interval: 10s

sensor:
  - platform: rbamp
    rbamp_id: meter_grid
    power:
      name: "Grid Power"
      # Positive = import, negative = export (STANDARD/PRO tiers with correct wiring)
      on_value:
        - lambda: |-
            // If grid power is negative — export (solar surplus).
            // Raise the heater setpoint to absorb the surplus.
            float export_w = -x;
            if (export_w > 100.0f) {
              float sp = std::min(export_w, 2000.0f);
              id(heater_setpoint_w) = sp;
            } else {
              id(heater_setpoint_w) = 0.0f;
            }

output:
  - platform: ledc          # PWM output to an SSR or 0-10 V controller
    pin: GPIO5
    id: heater_pwm
    frequency: 100Hz

interval:
  - interval: 10s
    then:
      - lambda: |-
          // Drive PWM proportionally to setpoint (0–2000 W → 0–100% duty)
          float duty = id(heater_setpoint_w) / 2000.0f;
          id(heater_pwm).set_level(duty);
```

> **Note**: bidirectional power (negative values = export) requires the STANDARD or PRO tier with correct voltage polarity wiring. On BASIC, firmware clamps the **average** active power per period to `P ≥ 0` — instantaneous P still reads negative during generation, but the period export accumulator stays empty. Per-tier behavior is in [`02_tiers.md`](02_tiers.md), polarity wiring in [`04_hardware.md`](04_hardware.md).

---

## 5 · Callback chaining through the ESPHome API

The ESPHome native API (port 6053) can trigger external Python scripts over the WebSocket API without a permanently running HA.

The `aioesphomeapi` Python library (`pip install aioesphomeapi`) gives direct async access to entity states:

```python
import asyncio
from aioesphomeapi import APIClient

async def main():
    cli = APIClient("rbamp-ui1.local", 6053, "")
    await cli.connect(login=True)

    def on_state(state):
        # Called on every sensor publish
        print(f"State: {state}")

    await cli.subscribe_states(on_state)
    await asyncio.Event().wait()  # runs forever

asyncio.run(main())
```

The pattern is useful for:

- Feeding rbAmp readings into a custom control loop that does not fit the YAML Lambda model in ESPHome.
- Logging to a custom format or database.
- Threshold logic in Python when the ESP32's 512 KB of RAM constrains complex math.

The API client runs on any Python 3.8+ host on the same network — Raspberry Pi, Docker container, development laptop.

---

## 6 · Apparent power — client-side calculation

When `apparent_power` is declared in the `sensor:` block, the component computes `S = U_rms × I_rms[ch0]` on the ESP32 from two fresh readings. If you need apparent power for multiple channels (CH1, CH2) or want to derive it from HA entities rather than from the component itself — use an HA `template` sensor:

```yaml
# configuration.yaml in HA
template:
  - sensor:
      - name: "CH1 Apparent Power"
        unit_of_measurement: "VA"
        device_class: apparent_power
        state_class: measurement
        state: >
          {% set u = states('sensor.rbamp_ui3_mains_voltage') | float(0) %}
          {% set i = states('sensor.rbamp_ui3_ch1_current') | float(0) %}
          {{ (u * i) | round(1) }}

      - name: "CH2 Apparent Power"
        unit_of_measurement: "VA"
        device_class: apparent_power
        state_class: measurement
        state: >
          {% set u = states('sensor.rbamp_ui3_mains_voltage') | float(0) %}
          {% set i = states('sensor.rbamp_ui3_ch2_current') | float(0) %}
          {{ (u * i) | round(1) }}
```

Note: `apparent_power` in the ESPHome schema uses only the CH0 current. The template above is the recommended approach for CH1/CH2 on UI2/UI3 deployments.

---

## 7 · Reading rbAmp without ESPHome

The rbAmp module is a plain I²C slave. Any I²C master can read it using the protocol from [`SPEC.md`](https://rbamp.com/docs/modules-basic-standard-api-reference). The sister client libraries (distribution repositories — placeholders until publication):

| Library | Distribution | Language | Target platform |
|---|---|---|---|
| Arduino | [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino) | C++ | ESP32 (Arduino framework), AVR, RP2040 |
| ESP-IDF | [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf) | C | ESP32 (IDF framework) |
| Python (CPython + MicroPython) | [`rbamp-python`](https://github.com/rb-amp/rbamp-python) | Python 3 | Raspberry Pi, Linux SBC, PC, ESP32/RP2040/STM32 on MicroPython |
| STM32 HAL | [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(in development)* | C | STM32 (HAL), other Cortex-M |

All libraries implement the same register map and period-metering protocol as the ESPHome component. They can be used independently or in parallel.

### When NOT to use ESPHome

- You already have a non-ESPHome ESP32 firmware and need to add rbAmp reads — use the ESP-IDF library and call its API directly.
- The master is a Raspberry Pi or another Linux SBC — use the Python library.
- You have a STM32-based PLC or custom board — STM32 HAL.
- A quick prototype on Arduino UNO or Leonardo — the Arduino library.

### Cross-platform energy accumulation — a note

The ESPHome component uses NVS-persisted `double` accumulators that survive reboots. Sister libraries may use other persistence mechanisms — see each library's `10_troubleshooting.md` (energy persistence section) for details and forward-compatibility specifics. If you are migrating from ESPHome to bare-metal firmware, plan the data migration: read the last Wh value out of HA before the swap and seed the new firmware's accumulator with it.

---

## 8 · Tasmota / WLED / OpenHAB bridges

### Tasmota

Tasmota has no native rbAmp driver. The cleanest bridge is to use ESPHome as the rbAmp reader and Tasmota as a separate I²C master for other devices, without mixing them.

If you really need Tasmota for rbAmp itself — use Berry scripting (Tasmota 12.x+) to write a custom driver performing the I²C reads per SPEC §6. The MicroPython library in `rbamp-python` is a useful reference for register addresses and float decoding.

### WLED

WLED (firmware for LED controllers) does not expose an I²C master to user scripts. rbAmp and WLED can be independent devices on the same bus only if they live on separate bus segments or have non-conflicting addresses. There is no official bridge.

### OpenHAB

ESPHome publishes data over MQTT (see Example 2) or the native API (`aioesphomeapi`). The OpenHAB MQTT binding can subscribe to ESPHome topics and deliver rbAmp readings into OpenHAB rules and Items. MQTT binding configuration is documented in the OpenHAB docs.



---

← [Examples](06_examples.md) · [Docs index](README.md) · [Home Assistant](08_has_integrations.md) →
