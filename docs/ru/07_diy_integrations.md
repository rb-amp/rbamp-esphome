# 07 · DIY-интеграции

Эта глава о использовании rbAmp за пределами стандартного Energy-дашборда HA: экспорт в внешние time-series базы данных, публикация по MQTT без HA, closed-loop управление нагрузкой и чтение rbAmp с не-ESPHome мастеров.

Cross-references:

- YAML-схема: [`09_api_reference.md`](09_api_reference.md)
- HA-специфика: [`08_has_integrations.md`](08_has_integrations.md)
- Arduino клиентская библиотека: [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino)
- ESP-IDF клиентская библиотека: [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf)
- Python (CPython + MicroPython): [`rbamp-python`](https://github.com/rb-amp/rbamp-python)
- STM32 HAL: [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(в разработке)*

---

## 1 · InfluxDB и Grafana через Home Assistant

Самый простой путь к InfluxDB + Grafana не требует изменений YAML. Home Assistant сам экспортирует все entity states в InfluxDB через нативную интеграцию:

```yaml
# configuration.yaml в HA
influxdb:
  host: 192.168.0.100    # ваш хост InfluxDB
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

Каждая публикация сенсора rbAmp (раз в `update_interval`) превращается в точку в InfluxDB. Grafana потом запрашивает InfluxDB и строит дашборды.

Минимальная Grafana-панель для живой мощности:

```
SELECT mean("value") FROM "state"
WHERE "entity_id" = 'sensor.rbamp_ui1_mains_power'
AND $timeFilter
GROUP BY time(1m)
```

Для InfluxDB 2.x (Flux):

```
from(bucket: "homeassistant")
  |> range(start: v.timeRangeStart, stop: v.timeRangeStop)
  |> filter(fn: (r) => r["entity_id"] == "sensor.rbamp_ui1_mains_power")
  |> aggregateWindow(every: 1m, fn: mean, createEmpty: false)
```

### Высокое разрешение InfluxDB (минуя HA)

State machine HA пишет только изменения состояния. Для плотной серии с точкой каждые 60 с (дефолтный `update_interval`) HA подходит. Если нужно sub-минутное разрешение — уменьшите `update_interval` и пусть HA recorder + InfluxDB интеграция захватят с этой частотой:

```yaml
rbamp:
  id: meter1
  update_interval: 10s     # 10-секундное разрешение — ×6 I²C-трафика, приемлемо
```

---

## 2 · Прямой MQTT-publish (без HA)

В ESPHome есть встроенный компонент `mqtt:`, публикующий все состояния сенсоров в брокер. Сенсоры rbAmp публикуются на каждом `update_interval` как любые другие.

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

# ПРИМЕЧАНИЕ: используйте mqtt: ИЛИ api: — не оба одновременно, если только
# вам не нужны и HA native API, и MQTT параллельно (поддерживается, но
# нестандартно).
mqtt:
  broker: 192.168.0.200      # IP вашего MQTT-брокера
  port: 1883
  username: !secret mqtt_user
  password: !secret mqtt_pass
  topic_prefix: rbamp/ui1    # все sensor-топики под этим префиксом
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

С `topic_prefix: rbamp/ui1` ESPHome публикует:

| MQTT topic | Payload |
|---|---|
| `rbamp/ui1/sensor/mains_voltage/state` | `226.70` |
| `rbamp/ui1/sensor/mains_current/state` | `0.755` |
| `rbamp/ui1/sensor/mains_power/state` | `92.80` |
| `rbamp/ui1/sensor/mains_energy/state` | `1024.341` |
| `rbamp/ui1/sensor/mains_frequency/state` | `50` |
| `rbamp/ui1/sensor/mains_power_factor/state` | `0.542` |
| `rbamp/ui1/status` | `online` / `offline` |

Любой MQTT-subscriber — Node-RED, InfluxDB Telegraf, OpenHAB, кастомный Python-скрипт — может потреблять эти топики напрямую.

### MQTT retained messages

Добавьте `retain: true` на каждый сенсор, если subscriber'ы, подключившиеся после публикации, должны сразу видеть последнее значение:

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"
      # ESPHome MQTT по умолчанию не ставит retain на sensor state. Для retain
      # используйте ручной publish action:
      on_value:
        - mqtt.publish:
            topic: "rbamp/ui1/voltage"
            payload: !lambda 'return to_string(x);'
            retain: true
```

### MQTT retain и OTA — что нужно знать

Retained-сообщения переживают рестарт ESP32 (это их основное назначение).
Это создаёт два сценария, о которых стоит знать заранее:

1. **OTA-обновление узла**. После прошивки ESP32 перезагружается. Старые
   retained-значения остаются в брокере и видны subscriber'ам в течение
   ~5..30 секунд (пока узел снова не подключится и не опубликует новые).
   Если узел не вернётся (повреждённая прошивка, фатальный crash на boot)
   — старые retained-значения будут отображаться **бесконечно**. Это
   маскирует офлайн-состояние от dashboards, которые читают только
   `state` без `status`.

   **Митигация**: всегда выставляйте `will_message:` (LWT) на топик
   `status` (как в примере выше). Брокер выставит payload `offline` при
   разрыве соединения; subscriber'ы могут отслеживать `status` для
   обнаружения зависших узлов. Сенсорные топики при этом останутся со
   старыми значениями — это нормально, но subscriber'ы знают что данные
   stale.

2. **Смена `topic_prefix:` между прошивками**. Если вы переименовали
   `topic_prefix:` (например `rbamp/ui1` → `rbamp/kitchen`) и обновили
   узел через OTA — retained-сообщения под **старым** префиксом
   останутся в брокере навсегда. Subscriber'ы, не знающие о смене,
   продолжат видеть устаревшие данные с zombie-топиков.

   **Митигация**: перед OTA с переименованным префиксом очистите старые
   retained-сообщения, опубликовав пустой payload с `retain: true` на
   каждый старый топик (CLI: `mosquitto_pub -h <broker> -t '<old>' -r -n`).
   Без этого зомби висят, пока кто-то вручную их не сбросит.

3. **Birth-message и `start_session: true`** для подключения после
   рестарта брокера — см. документацию ESPHome `mqtt:` блока для опций
   `clean_session:` и `keepalive:`. Эти параметры контролируют, как
   брокер обращается с pending QoS 1/2 сообщениями при разрыве.

---

## 3 · ESPHome Lambda actions

Callback `on_value` ESPHome срабатывает на каждое новое чтение сенсора. Можно использовать его для threshold-actions, исполняемых полностью на ESP32 без round-trip через HA.

### Threshold-аларм на ток

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
                lambda: 'return x > 15.0f;'    # порог перегрузки 15 А
              then:
                - logger.log:
                    level: WARN
                    format: "Over-current: %.2f A — срабатывает alarm relay"
                    args: [x]
                - switch.turn_on: alarm_relay
```

### Детектирование скачка нагрузки (custom Lambda)

```yaml
# Детектирование резкого роста нагрузки (например, старт компрессора)
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

### Публикация в кастомный MQTT-топик по порогу

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

## 4 · Closed-loop управление — водонагреватель PID

Типичный кейс для домашнего учёта rbAmp — солнечное самопотребление: греть воду излишком солнечной мощности вместо экспорта в сеть.

Паттерн использует два модуля rbAmp: один на вводе grid (импорт / экспорт), один на цепи водонагревателя. PWM-реле или SSR на нагревателе позволяет переменное управление мощностью.

```yaml
# Концептуальный набросок — конкретная реализация PID зависит от вашего
# реле / SSR и нагрузки. Компонент PID controller в ESPHome — рекомендуемый
# building block.

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
    update_interval: 10s     # более плотная петля для отзывчивого управления

  - id: meter_heater
    address: 0x51
    update_interval: 10s

sensor:
  - platform: rbamp
    rbamp_id: meter_grid
    power:
      name: "Grid Power"
      # Положительная = импорт, отрицательная = экспорт (тиры STANDARD/PRO с правильной разводкой)
      on_value:
        - lambda: |-
            // Если мощность в сети отрицательная — экспорт (избыток солнечной).
            // Поднимаем setpoint нагревателя чтобы поглотить излишек.
            float export_w = -x;
            if (export_w > 100.0f) {
              float sp = std::min(export_w, 2000.0f);
              id(heater_setpoint_w) = sp;
            } else {
              id(heater_setpoint_w) = 0.0f;
            }

output:
  - platform: ledc          # PWM-выход на SSR или 0-10V контроллер
    pin: GPIO5
    id: heater_pwm
    frequency: 100Hz

interval:
  - interval: 10s
    then:
      - lambda: |-
          // Тянем PWM пропорционально setpoint (0–2000 Вт → 0–100% duty)
          float duty = id(heater_setpoint_w) / 2000.0f;
          id(heater_pwm).set_level(duty);
```

> **Примечание**: двунаправленная мощность (отрицательные значения = экспорт) требует тира STANDARD или PRO с правильной разводкой полярности напряжения. На BASIC прошивка клампит **среднюю** активную мощность за период в `P ≥ 0` — мгновенная P всё равно читается отрицательной в момент генерации, но period-аккумулятор экспорта остаётся пустым. Поведение по тирам — в [`02_tiers.md`](02_tiers.md), разводка полярности — в [`04_hardware.md`](04_hardware.md).

---

## 5 · Цепочка callback'ов через ESPHome API

ESPHome native API (порт 6053) можно использовать для триггера внешних Python-скриптов через WebSocket API без постоянно работающего HA.

Python-библиотека `aioesphomeapi` (`pip install aioesphomeapi`) даёт прямой async доступ к состояниям сущностей:

```python
import asyncio
from aioesphomeapi import APIClient

async def main():
    cli = APIClient("rbamp-ui1.local", 6053, "")
    await cli.connect(login=True)

    def on_state(state):
        # Вызывается на каждую публикацию сенсора
        print(f"State: {state}")

    await cli.subscribe_states(on_state)
    await asyncio.Event().wait()  # работает бесконечно

asyncio.run(main())
```

Паттерн полезен для:

- Передачи чтений rbAmp в кастомную control-петлю, не помещающуюся в YAML Lambda модель ESPHome.
- Логирования в кастомный формат или базу данных.
- Threshold-логики на Python, когда 512 КБ RAM ESP32 — ограничение для сложной математики.

API-клиент работает на любом Python 3.8+ хосте в той же сети — Raspberry Pi, Docker-контейнер, лэптоп для разработки.

---

## 6 · Полная мощность — расчёт на стороне клиента

Когда `apparent_power` объявлен в блоке `sensor:`, компонент считает `S = U_rms × I_rms[ch0]` на ESP32 из двух свежих чтений. Если нужна полная мощность для нескольких каналов (CH1, CH2) или хотите её вывести из HA-сущностей а не из самого компонента — используйте `template`-сенсор HA:

```yaml
# configuration.yaml в HA
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

Примечание: `apparent_power` в ESPHome-схеме использует только ток CH0. Шаблон выше — рекомендованный подход для CH1/CH2 на UI2/UI3 deployment'ах.

---

## 7 · Чтение rbAmp без ESPHome

Модуль rbAmp — обычный I²C-slave. Любой I²C-мастер может его читать по протоколу из [`SPEC.md`](https://rbamp.com/docs/modules-basic-standard-api-reference). Сестринские клиентские библиотеки (distribution-репозитории — placeholder'ы до публикации):

| Библиотека | Дистрибутив | Язык | Целевая платформа |
|---|---|---|---|
| Arduino | [`rbamp-arduino`](https://github.com/rb-amp/rbamp-arduino) | C++ | ESP32 (Arduino framework), AVR, RP2040 |
| ESP-IDF | [`rbamp-esp-idf`](https://github.com/rb-amp/rbamp-esp-idf) | C | ESP32 (IDF framework) |
| Python (CPython + MicroPython) | [`rbamp-python`](https://github.com/rb-amp/rbamp-python) | Python 3 | Raspberry Pi, Linux SBC, PC, ESP32/RP2040/STM32 на MicroPython |
| STM32 HAL | [`rbamp-stm32-hal`](https://github.com/rb-amp/rbamp-stm32-hal) *(в разработке)* | C | STM32 (HAL), другие Cortex-M |

Все библиотеки реализуют один и тот же register map и protocol period-метеринга что и ESPHome-компонент. Их можно использовать независимо или параллельно.

### Когда НЕ использовать ESPHome

- У вас уже есть не-ESPHome ESP32 firmware и нужно добавить чтение rbAmp — используйте ESP-IDF библиотеку и вызывайте API напрямую.
- Мастер — Raspberry Pi или другой Linux SBC — используйте Python-библиотеку.
- У вас STM32-PLC или кастомная плата — STM32 HAL.
- Быстрый прототип на Arduino UNO или Leonardo — Arduino-библиотека.

### Кросс-платформенное накопление энергии — заметка

ESPHome-компонент использует NVS-сохраняемые `double` аккумуляторы, переживающие перезагрузку. Сестринские библиотеки могут использовать другие механизмы сохранения — детали и форвард-совместимость по конкретной библиотеке смотрите в её `10_troubleshooting.md` (секция energy persistence). Если переходите с ESPHome на bare-metal firmware — планируйте миграцию данных: выгрузите последнее значение Wh из HA до swap'а и зарядите им аккумулятор новой прошивки.

---

## 8 · Tasmota / WLED / OpenHAB-мосты

### Tasmota

В Tasmota нет нативного драйвера rbAmp. Чистый bridge — использовать ESPHome как считыватель rbAmp и Tasmota как отдельный I²C-мастер для других устройств, не смешивая.

Если Tasmota нужна именно для rbAmp — используйте Berry scripting (Tasmota 12.x+) для написания кастомного драйвера, выполняющего I²C-чтения по SPEC §6. MicroPython-библиотека из `rbamp-python` — полезный референс по адресам регистров и float-декодированию.

### WLED

WLED (firmware для LED-контроллеров) не предоставляет I²C-мастера в пользовательских скриптах. rbAmp и WLED могут быть независимыми устройствами на одной шине только если они на разных сегментах шины или имеют неконфликтующие адреса. Официального моста нет.

### OpenHAB

ESPHome публикует данные через MQTT (см. Пример 2) или native API (`aioesphomeapi`). OpenHAB MQTT binding может подписаться на ESPHome-топики и доставлять чтения rbAmp в правила и Items OpenHAB. Конфигурация MQTT binding'а — в документации OpenHAB.



---

← [Примеры](06_examples.md) · [Оглавление](README.md) · [Home Assistant](08_has_integrations.md) →
