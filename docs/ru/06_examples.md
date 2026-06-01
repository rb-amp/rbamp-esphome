# 06 · YAML-кулинарная книга

Эта глава — полный набор работающих YAML-конфигураций для rbAmp ESPHome компонента. Каждый пример самодостаточный: скопируйте, впишите имя платы и WiFi-данные, скомпилируйте, прошейте. Где пример отступает от минимального шаблона — объяснение, зачем.

Cross-references:

- Схема компонента: [`09_api_reference.md`](09_api_reference.md)
- Period metering protocol: [SPEC.md §7](https://rbamp.com/docs/modules-basic-standard-api-reference)
- Multi-module topology: [SPEC.md §8](https://rbamp.com/docs/modules-basic-standard-api-reference)
- Broadcast LATCH: [SPEC.md §9](https://rbamp.com/docs/modules-basic-standard-api-reference)
- NACK loose-sanity rule: [SPEC.md §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)

---

## 1 · UI1 — одноканальный (минимальный)

Исходник: [`../example/ui1.yaml`](../../example/ui1.yaml)

Простейшая полезная конфигурация: один канал напряжения, один канал тока, шесть производных величин из них. Старт для любого нового deployment'а — расширяйте полями по мере необходимости.

```yaml
esphome:
  name: rbamp-ui1

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"
    password: !secret ap_password

captive_portal:
api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz    # 100 кГц вызывает периодические NACK от модуля;
                      # 50 кГц снижает их ~в 5-10 раз (см. SPEC §B.5).
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
  ct_model: SCT_013_030    # модель CT-клипсы — поменяйте под вашу

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

**Что демонстрирует**: минимальная жизнеспособная конфигурация. Шесть сенсоров покрывают основной кейс — учёт по всему дому или одна нагрузка. У сенсора `energy` уже выставлен `device_class: energy` + `state_class: total_increasing`, поэтому он сразу попадает в Energy-дашборд HA без дополнительной настройки. Подробнее про period-metering протокол — в [SPEC §7](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 1.1 · UI1 с кастомизацией — все стандартные ESPHome-крутилки

**rbAmp — это "good citizen" ESPHome.** Каждый слот (`voltage`, `current`, `power`, …) — это обычный `sensor:`-объект ESPHome. На нём работает весь стандартный набор свойств и фильтров. Не нужно ни `lambda:`, ни кастомных шаблонов — всё, что вы делаете с `pzemac` / `atm90e32` / `cse7766`, работает здесь точно так же. Пример показывает наиболее частые кастомизации одним блоком:

```yaml
esphome:
  name: rbamp-ui1-tuned

esp32:
  board: esp32dev
  framework:
    type: arduino

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

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
  update_interval: 10s        # ↑ Опрос 1 раз / 10 с вместо дефолтных 60 с —
                              # плотнее график, ~6× I²C-трафика.
  ct_model: SCT_013_030

sensor:
  - platform: rbamp
    rbamp_id: meter1

    # ──────────── Кастомизация имён + точности + фильтров ────────────
    voltage:
      name: "Напряжение в сети"          # Любое имя — попадёт в HA entity_id
      accuracy_decimals: 1               # 226.7 вместо дефолтных 226.70
      filters:
        # Линейная калибровка под референсный мультиметр:
        - calibrate_linear:
            - 0.0 -> 0.0
            - 230.0 -> 230.5
        # Сглаживание острых выбросов (стандарт ESPHome):
        - exponential_moving_average:
            alpha: 0.3
            send_every: 1

    current:
      name: "Ток нагрузки"
      accuracy_decimals: 3               # 0.755 вместо 0.75
      filters:
        # Mask noise floor — отбросить значения ниже 50 мА (50 мВт при 230 В).
        # Эквивалент опции `noise_floor` в pzemac / cse7766.
        - lambda: 'return x < 0.05 ? 0.0f : x;'

    power:
      name: "Мощность"
      accuracy_decimals: 1
      filters:
        # Throttle публикации: не чаще 1 раза / 30 с, даже если update_interval 10 с.
        - throttle: 30s

    energy:
      name: "Энергия (Wh)"
      # Стандартный device_class: energy + state_class: total_increasing
      # уже выставлены компонентом — Energy-дашборд работает.
      filters:
        # Конвертация Wh → kWh для более привычной шкалы в Lovelace.
        # (Energy-дашборд HA сам ожидает Wh, но если хочется график в kWh —
        #  объявите второй template-сенсор с делением, а не меняйте units здесь.)
        - multiply: 0.001
      unit_of_measurement: "kWh"         # ← переопределяет дефолтные "Wh"
      accuracy_decimals: 3

    frequency:
      name: "Частота сети"
      accuracy_decimals: 2

    power_factor:
      name: "Коэффициент мощности"
      accuracy_decimals: 3
      # Опционально: фильтр для скрытия PF при нулевой нагрузке (P ~ 0)
      filters:
        - lambda: 'return isnan(x) ? 0.0f : x;'
```

**Что демонстрирует**:

- **Кастомные имена** (`name:`) попадают в HA как `entity_id` — `sensor.rbamp_ui1_tuned_napryazhenie_v_seti`. Имена на любом языке, включая кириллицу, работают.
- **Точность вывода** (`accuracy_decimals:`) — независимо от модуля; модуль возвращает float32 с максимальной точностью, ESPHome обрезает для UI.
- **`update_interval:`** меняется поштучно на сенсор не нужно — это свойство **компонента** (`PollingComponent`), один раз в `rbamp:` блоке. Если хотите редкие публикации отдельных сенсоров — добавьте `filters: - throttle: <interval>`.
- **`filters:`** — полный набор ESPHome:
  - `calibrate_linear` — точечная калибровка под вашу опору (мультиметр, эталонный амперметр).
  - `exponential_moving_average` / `sliding_window_moving_average` — сглаживание.
  - `lambda` — произвольное преобразование (noise-floor mask, conditional zero).
  - `multiply` / `offset` — арифметика (например, Wh → kWh).
  - `throttle` / `delta` — снижение частоты публикации.
- **`unit_of_measurement:`** можно переопределить (например, на `kWh` с `multiply: 0.001`). HA подхватит новую единицу автоматически.
- **Никакого `lambda:` для базовой работы** — все вычисления RMS, мощности, частоты, PF делает модуль. ESP32 только переименовывает, форматирует, фильтрует.

> ⚠ **Не переопределяйте `device_class:` для `energy`** — Energy-дашборд HA жёстко ожидает `device_class: energy` + `state_class: total_increasing`. Конвертация единиц через `multiply` + `unit_of_measurement:` сохраняет совместимость, но смена `device_class` сломает дашборд.

Полный список стандартных свойств сенсора и фильтров — в [официальной документации ESPHome `Sensor Component`](https://esphome.io/components/sensor/). Эта документация **полностью применима к слотам rbAmp** — никаких отклонений от стандарта.

---

## 2 · UI3 — трёхканальный со смешанными клипсами

Исходник: [`../example/ui3.yaml`](../../example/ui3.yaml)

UI3 SKU — один вход напряжения и три независимых канала тока. Канонический сценарий — поставить **разные** модели клипс на разные каналы: малая клипса на низкоточные потребители (для максимального разрешения), крупная — на основной ввод (для headroom на пики).

```yaml
esphome:
  name: rbamp-ui3

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

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
  # drdy_pin: GPIO15
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  # CH0=5A для дежурных потребителей и LED-освещения,
  # CH1=30A для основных бытовых нагрузок,
  # CH2=100A для общего ввода или EV-зарядки

sensor:
  - platform: rbamp
    rbamp_id: meter1
    # Общие (не зависят от топологии)
    voltage:
      name: "Mains Voltage"
    frequency:
      name: "Mains Frequency"
    apparent_power:
      name: "CH0 Apparent Power"

    # Канал 0 — основной вход тока (малые потребители, 5A)
    current:
      name: "CH0 Current"
    power:
      name: "CH0 Power"
    energy:
      name: "CH0 Energy"
    power_factor:
      name: "CH0 Power Factor"
    reactive_power:
      name: "CH0 Reactive Power"

    # Канал 1 — основные нагрузки (30A)
    current_1:
      name: "CH1 Current"
    power_1:
      name: "CH1 Power"
    energy_1:
      name: "CH1 Energy"
    power_factor_1:
      name: "CH1 Power Factor"
    reactive_power_1:
      name: "CH1 Reactive Power"

    # Канал 2 — общий ввод / EV (100A)
    current_2:
      name: "CH2 Current"
    power_2:
      name: "CH2 Power"
    energy_2:
      name: "CH2 Energy"
    power_factor_2:
      name: "CH2 Power Factor"
    reactive_power_2:
      name: "CH2 Reactive Power"
```

**Что демонстрирует**: все три канала UI3 SKU + смешанные модели клипс через `ct_models:`. Валидатор схемы проверяет, что `power_N` требует `current_N` и `voltage` — если объявить `power_1` без `current_1`, компиляция упадёт с понятным сообщением. У каждого канала свой независимый NVS-сохраняемый накопитель `energy_*`. Если на всех каналах одна модель клипсы — используйте глобальный `ct_model:` вместо `ct_models:`.

---

## 3 · Multi-module шина (три rbAmp на 0x50 / 0x51 / 0x52)

Исходник: [`../example/multi_module.yaml`](../../example/multi_module.yaml)

Три модуля на одной I²C-шине — общий ввод дома, выход солнечной генерации, EV-зарядка. У каждого свой блок `rbamp:` (в схеме компонента `MULTI_CONF = True`).

```yaml
esphome:
  name: rbamp-multi

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: INFO

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz    # обязательно: на 100 кГц с тремя модулями периодические
                      # NACK почти неизбежны в каждом цикле
  scan: true

external_components:
  - source:
      type: local
      path: ../components
    components: [rbamp]

# Три независимых блока rbamp: — MULTI_CONF = True в схеме.
# broadcast_latch: true объявлен здесь для forward-совместимости: на
# текущей прошивке компонент логирует warning при загрузке (общевещательный
# адрес отключён) и откатывается на последовательный latch для каждого
# модуля. Warning исчезнет автоматически когда прошивка добавит поддержку
# broadcast — YAML менять не потребуется. См. SPEC §9.
rbamp:
  - id: meter_house
    address: 0x50
    update_interval: 60s
    broadcast_latch: true

  - id: meter_solar
    address: 0x51
    update_interval: 60s
    broadcast_latch: true
    bidirectional: true     # тиры STANDARD / PRO; energy_exported_*
                             # публикует 0 пока экспортный накопитель не
                             # появится в прошивке

  - id: meter_evcharger
    address: 0x52
    update_interval: 60s
    broadcast_latch: true

sensor:
  - platform: rbamp
    rbamp_id: meter_house
    voltage:
      name: "House Voltage"
    current:
      name: "House Current"
    power:
      name: "House Power"
    energy:
      name: "House Energy"
    frequency:
      name: "Mains Frequency"

  - platform: rbamp
    rbamp_id: meter_solar
    voltage:
      name: "Solar Voltage"
      internal: true        # скрыт от HA — та же сеть, что и House Voltage
    current:
      name: "Solar Current"
    power:
      name: "Solar Power"
    energy:
      name: "Solar Energy"
    energy_exported:
      name: "Solar Energy Exported"

  - platform: rbamp
    rbamp_id: meter_evcharger
    voltage:
      name: "EV Charger Voltage"
      internal: true
    current:
      name: "EV Charger Current"
    power:
      name: "EV Charger Power"
    energy:
      name: "EV Charger Energy"
```

**Что демонстрирует**: паттерн `MULTI_CONF = True` — объявляйте столько блоков `rbamp:`, сколько модулей на шине. Каждый ведёт собственный NVS-накопитель, key которого выводится из I²C-адреса — три счётчика остаются независимыми через перезагрузки. Флаг `internal: true` на повторных сенсорах напряжения прячет от HA три почти одинаковых mains-voltage сущности когда все модули измеряют одну фазу. Процедуру провизии адресов смотрите в Примере 4 ниже.

> **Семантика `internal: true`.** Флаг **не отключает** чтение
> регистра с модуля — компонент всё равно читает напряжение с каждого
> модуля каждый `update_interval` (это требуется для расчёта мощности).
> Флаг лишь скрывает сенсор от Home Assistant API: сущность
> не создаётся, в Lovelace не появляется, в Energy-дашборде не учтена.
> Если хотите снизить I²C-трафик, не объявляйте слот `voltage:` под
> `sensor.platform: rbamp` вообще — компонент тогда не запрашивает регистр.
> Если же объявили — флаг `internal:` только косметика.

Второе ограничение касается уникальности адресов на шине:

> ⚠ **Уникальные адреса — на ответственности конфигурации.** Схема
> компонента (`MULTI_CONF = True`) не валидирует пересечения `address:`
> между блоками `rbamp:`. Если два блока укажут один и тот же
> `address: 0x50`, оба объявления будут приняты, а на шине обе цели
> будут отвечать одновременно — ожидайте I²C-конфликты, искажённые
> ответы или симптомы похожие на отсутствующий модуль. Перед коммитом
> YAML проверяйте, что все `address:` уникальны (и не совпадают с
> другими I²C-устройствами на шине). После каждой `new_address:`
> провизии обновляйте раздел `rbamp:` под новый адрес — старая запись
> не аннулируется автоматически.

### Pull-up на multi-module шине

Все три модуля приходят с заводскими 4,7 кОм pull-up'ами. На шине из трёх модулей параллельная комбинация ~1,6 кОм — увеличенное потребление и краевой signal integrity на 50 кГц. Перережьте solder-перемычки pull-up на модулях #2 и #3 (оставьте только на #1 или поставьте одну внешнюю пару 4,7 кОм рядом с ESP32). Подробнее — в [04_hardware.md](04_hardware.md) раздел про многомодульную шину.

---

## 4 · Однократная смена I²C-адреса (0x50 → 0x51)

Исходник: [`../example/address_change.yaml`](../../example/address_change.yaml)

Все модули rbAmp поставляются с завода на адресе `0x50`. Чтобы поставить несколько модулей на одну шину, нужно перепрошить каждый на свой уникальный адрес **до** их параллельного подключения.

```yaml
esphome:
  name: rbamp-addr-change

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG    # DEBUG показывает строки про смену адреса

api:
ota:
  - platform: esphome

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
  address: 0x50          # текущий адрес — до смены
  new_address: 0x51      # одноразовая цель — УДАЛИТЕ эту строку после успеха

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "rbAmp Voltage (post-relocate)"
    current:
      name: "rbAmp Current (post-relocate)"
```

**Что демонстрирует**: однократный поток смены адреса. Прошивка этого YAML заставляет компонент один раз при загрузке выполнить процедуру:

1. Опрос `0x50`. Если `0x50` молчит а `0x51` уже отвечает — смена применена в предыдущей загрузке, компонент адаптируется и логирует warning с просьбой очистить YAML.
2. Проверка готовности модуля принять провизионную операцию.
3. Запись нового адреса во flash (~700 мс), мягкий ресет (~300 мс), повторный lock в AC-цикл.
4. Подтверждение на новом адресе. При успехе — лог `Address change confirmed at 0x51` и `IMPORTANT: update YAML to address: 0x51 and remove new_address:`.

**Требование**: модуль должен быть в **factory-provisioning mode** — режим для одноразовых провизионных операций. Стандартные production-модули поставляются с отключённым provisioning-режимом; для смены адреса в полевых условиях обратитесь к документации модуля или поставщику за процедурой временного включения.

**После успеха**: отредактируйте YAML на `address: 0x51` и удалите строку `new_address: 0x51`. Залейте снова. Со следующей загрузки компонент будет использовать `0x51` штатно и процедура смены больше не запустится.

### Чек-лист revert после успешной провизии

Чтобы не оставить «висящий» provisioning-вариант в репозитории:

```yaml
# ДО (provisioning):
rbamp:
  id: meter1
  address: 0x50               # текущий адрес модуля
  new_address: 0x51           # цель провизии
  update_interval: 60s

# ПОСЛЕ (production — applied next flash):
rbamp:
  id: meter1
  address: 0x51               # ← заменили на новый адрес
  # new_address: 0x51         # ← УДАЛИТЕ строку (не комментируйте)
  update_interval: 60s
```

Точно три правки:

1. Поменять значение `address:` со старого на новый.
2. Удалить (не закомментировать) строку `new_address:`.
3. Перелить через OTA или USB.

После шага 3 в логе должна появиться нормальная строка
`dump_config` без warning'а `address change requested but current address
matches new_address` — это значит, что revert прошёл корректно. См.
также [10_troubleshooting §4 "OTA сразу после `new_address:` provisioning не загружается"](10_troubleshooting.md#ota-сразу-после-new_address-provisioning-не-загружается)
если первый OTA после revert не стартует.

---

## 5 · Bench-конфиг с секретами

Исходник: `../example/bench-ui1.yaml`

Паттерн `bench-*.yaml` — для development-машин, где WiFi-креды нужны в конфиге, но не должны попасть в коммит. `example/.gitignore` исключает `bench-*.yaml` и `secrets.yaml` из version control.

```yaml
esphome:
  name: rbamp-ui1

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
  level: DEBUG

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"
    password: !secret ap_password

captive_portal:
api:
ota:
  - platform: esphome

web_server:
  port: 80
  version: 3           # опционально: локальный HTTP UI — полезно при bench-тестах

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

Соответствующий `secrets.yaml` (в `example/`, не tracked'нут):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

**Что демонстрирует**: отделение compile-time кредов от version-controlled YAML через `!secret` ESPHome. Запись `bench-*.yaml` в `.gitignore` позволяет иметь сколько угодно bench-вариантов без риска утечки. Используйте `esphome run --device 192.168.0.173 example/bench-ui1.yaml` для OTA после первого подключения к сети или `esphome run --device COM6 example/bench-ui1.yaml` для первой USB-прошивки.

---

## 6 · Brownout и потеря mains

При обрыве mains-питания изолированный аналоговый front-end модуля перестаёт принимать сигнал. Регистр напряжения падает в `0.0 V` — это валидный IEEE 754 finite float, проходящий sanity-фильтр `isfinite()` в компоненте. Из SPEC §B.5:

> NO physical lower bounds — brownout (U=0 V on mains disconnect), voltage sag, off-grid / UPS test, и breaker-trip MUST pass through to HA, чтобы пользователь видел реальное состояние.

Никакого специального YAML для детектирования потери mains не требуется — сенсор `voltage` сам прочитает `0.0 V`.

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"    # читает 0.0 V при отключении mains
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"
```

### Автоматизация HA: оповещение о потере mains

```yaml
# configuration.yaml или Automations UI
automation:
  - alias: "rbAmp mains loss alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_voltage
        below: 10          # U < 10 V = mains отключено или сильный brownout
        for:
          seconds: 5       # debounce: устойчивое, не transient
    action:
      - service: notify.mobile_app_your_phone
        data:
          title: "Mains power lost"
          message: >
            Voltage dropped to {{ states('sensor.rbamp_ui1_mains_voltage') }} V
            at {{ now().strftime('%H:%M:%S') }}.
```

**Что демонстрирует**: использование 0V passthrough поведения (SPEC §B.5) как источника события "потеря mains". Поскольку компонент публикует `0.0 V` а не пропускает publish — HA-автоматизации и Energy-дашборд видят событие brownout в реальном времени. Условие `for: 5 seconds` блокирует ложные срабатывания во время ~250 мс boot warm-up'а (когда регистры читают 0 до первого валидного измерения).

---

## 7 · Energy-дашборд HA

Сенсор `energy` (и `energy_1`, `energy_2`, `energy_exported`, и т.д.) автоматически сконфигурированы с `device_class: energy` и `state_class: total_increasing` — именно то, что HA Energy-дашборд ожидает для накопительных сенсоров потребления. Дополнительного YAML не нужно.

После discovery:

1. Settings → Dashboards → Energy.
2. Add Consumption → выберите "Mains Energy" (или нужную сущность учёта импорта).
3. Если есть солнечная — Add Production → выберите Solar Energy (из `meter_solar` в multi-module примере).

Значения энергии в **Wh**. HA сам конвертирует в кВт·ч для отображения. NVS-сохранение в компоненте гарантирует, что накопитель переживает перезагрузку ESP32 без ложного "сброса счётчика" в статистике HA.

```yaml
# Пример: потребление + солнечный экспорт для Energy-дашборда HA
sensor:
  - platform: rbamp
    rbamp_id: meter_house
    energy:
      name: "House Grid Import"    # → HA Energy: grid consumption

  - platform: rbamp
    rbamp_id: meter_solar
    energy:
      name: "Solar Production"     # → HA Energy: solar production
    energy_exported:
      name: "Solar Grid Export"    # → HA Energy: return to grid
                                   # читает 0 пока экспортный накопитель
                                   # STANDARD/PRO не появится в прошивке
```

---

## 8 · HA-автоматизации на сенсорах rbAmp

### Сброс нагрузки: реле отключается при превышении порога

```yaml
automation:
  - alias: "Load shedding — отключить некритичные нагрузки на 3 кВт"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_multi_house_power
        above: 3000        # порог 3 кВт
        for:
          seconds: 10
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.ev_charger_relay   # реле некритичной нагрузки

  - alias: "Load shedding — восстановить когда упало ниже 2 кВт"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_multi_house_power
        below: 2000
        for:
          seconds: 30
    action:
      - service: switch.turn_on
        target:
          entity_id: switch.ev_charger_relay
```

### Напоминание о коррекции коэффициента мощности

```yaml
automation:
  - alias: "Poor power factor notification"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_power_factor
        below: 0.7
        for:
          minutes: 5
    action:
      - service: persistent_notification.create
        data:
          title: "Low power factor"
          message: >
            Power factor is {{ states('sensor.rbamp_ui1_mains_power_factor') }}.
            Check for large inductive loads (motors, old lighting ballasts).
```

### Алерт превышения дневного бюджета

```yaml
automation:
  - alias: "Daily energy budget — предупреждение на 10 кВт·ч"
    trigger:
      - platform: template
        value_template: >
          {{ states('sensor.energy_daily') | float(0) > 10000 }}
    action:
      - service: notify.mobile_app_your_phone
        data:
          message: "Daily energy usage exceeded 10 kWh."
```

---

## 9 · Lovelace-карточки

### Gauge — живая мощность

```yaml
type: gauge
entity: sensor.rbamp_ui1_mains_power
min: 0
max: 3600
name: "Live Power"
segments:
  - from: 0
    color: green
  - from: 1500
    color: orange
  - from: 2500
    color: red
```

### Apex Charts — мощность во времени

```yaml
type: custom:apexcharts-card
header:
  title: "Power (W)"
  show: true
graph_span: 4h
series:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
    stroke_width: 2
    curve: smooth
```

### Apex Charts — сравнение энергии по трём каналам

```yaml
type: custom:apexcharts-card
header:
  title: "Channel Energy (Wh)"
  show: true
graph_span: 24h
series:
  - entity: sensor.rbamp_ui3_ch0_energy
    name: CH0
  - entity: sensor.rbamp_ui3_ch1_energy
    name: CH1
  - entity: sensor.rbamp_ui3_ch2_energy
    name: CH2
```

### Mini graph — напряжение

```yaml
type: custom:mini-graph-card
entities:
  - sensor.rbamp_ui1_mains_voltage
name: "Mains Voltage"
hours_to_show: 12
points_per_hour: 4
line_width: 2
```

---

## 10 · Utility meter — агрегация по дням / неделям / месяцам

ESPHome публикует накопительные суммы в Wh. Интеграция HA `utility_meter` сбрасывает счётчик в настраиваемые интервалы и публикует дельту как отдельный сенсор — удобно для дневных или месячных биллинговых разрезов.

```yaml
# configuration.yaml
utility_meter:
  energy_daily:
    source: sensor.rbamp_ui1_mains_energy
    cycle: daily
    name: "Daily Energy"

  energy_monthly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: monthly
    name: "Monthly Energy"

  energy_weekly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: weekly
    name: "Weekly Energy"
```

HA создаёт три сущности: `sensor.energy_daily`, `sensor.energy_monthly`, `sensor.energy_weekly`. Каждая сбрасывается в 0 в начале соответствующего периода и копит дельту от исходного `total_increasing` сенсора.

### Производный сенсор кВт·ч/день

`utility_meter` по умолчанию выдаёт Wh. Для отображения в кВт·ч добавьте `template` сенсор:

```yaml
template:
  - sensor:
      - name: "Daily Energy kWh"
        unit_of_measurement: "kWh"
        state_class: total_increasing
        device_class: energy
        state: >
          {{ (states('sensor.energy_daily') | float(0) / 1000) | round(3) }}
```

### Сенсор дневной стоимости

```yaml
template:
  - sensor:
      - name: "Daily Energy Cost"
        unit_of_measurement: "EUR"
        icon: mdi:currency-eur
        state: >
          {% set kwh = states('sensor.energy_daily') | float(0) / 1000 %}
          {% set rate = 0.28 %}    # EUR/kWh — установите свой тариф
          {{ (kwh * rate) | round(2) }}
```

---

## 11 · Per-load sub-метеринг (3 модуля на 0x50 / 0x51 / 0x52)

Установка трёх модулей rbAmp в распределительный щит позволяет вести per-circuit энергоучёт без smart-панели или дополнительного железа.

```yaml
# Расширение multi-module примера utility-метерами по контурам

utility_meter:
  kitchen_daily:
    source: sensor.rbamp_multi_house_energy
    cycle: daily
    name: "Kitchen Circuit Daily"

  solar_monthly:
    source: sensor.rbamp_multi_solar_energy
    cycle: monthly
    name: "Solar Production Monthly"

  ev_daily:
    source: sensor.rbamp_multi_ev_charger_energy
    cycle: daily
    name: "EV Charging Daily"
```

В сочетании с Energy-дашбордом HA получите помесячные выписки по контурам без дополнительных кВт·ч×тариф вычислений.

---

## 12 · rbAmp + реле — closed-loop управление

### Бойлер с управлением реле

```yaml
# ESPHome YAML на той же ESP32 что и rbAmp
switch:
  - platform: gpio
    pin: GPIO5
    id: heater_relay
    name: "Water Heater"
```

С rbAmp и реле на одной ESP32 можно сделать замкнутую петлю прямо в ESPHome (через Lambda или `on_value`) без round-trip через HA:

```yaml
sensor:
  - platform: rbamp
    rbamp_id: meter1
    power:
      name: "House Power"
      on_value:
        then:
          - if:
              condition:
                lambda: 'return x > 3000.0f;'
              then:
                - switch.turn_off: heater_relay
              else:
                - switch.turn_on: heater_relay
```

**Что демонстрирует**: использование callback'а `on_value` сенсора rbAmp для управления реле без участия HA. Callback срабатывает на ESP32 каждые `update_interval` (60 с) и обновляет состояние реле по последнему чтению мощности. Для более жёсткого контроля (< 60 с цикл) уменьшите `update_interval` до `10s` — это пропорционально увеличит нагрузку на I²C-шину. Более полный пример closed-loop для бойлера — в [07_diy_integrations.md](07_diy_integrations.md).

---

## 13 · Интеграция DRDY

Open-drain пин DRDY модуля даёт LOW-импульс ~10 мкс каждые ~200 мс после обновления мгновенных регистров. Можно завести его на GPIO ESP32 и использовать как interrupt hint вместо опроса по фиксированному таймеру.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s        # интегрирование энергии всё равно каждые 60 с
  drdy_pin:
    number: GPIO15
    mode:
      input: true
      pullup: true            # DRDY — open-drain; нужен pull-up
    inverted: true            # DRDY импульс LOW; inverted=true даёт rising edge
```

> **Примечание**: на текущей прошивке компонент логирует `drdy_pin` в `dump_config`, но не реализует interrupt-driven RT чтения — `update_interval` по-прежнему задаёт частоту опроса. Пин можно завести заранее под будущие ревизии прошивки без правок кода.

---

## 14 · Подготовка под будущие split-phase / three-phase SKU

Схема компонента уже принимает phased-поля сенсоров (`voltage_a/b/c`, `current_a/b/c`, `power_a/b/c`, `energy_a/b/c`, `power_factor_a/b/c`, `reactive_power_a/b/c`, `power_total`). Они зарезервированы под предстоящие SKU rbAmp-U2I2 (split-phase US) и rbAmp-U3I3 (трёхфазная Европа).

Если готовите YAML под будущий трёхфазный deployment уже сегодня — можете описать полный phased-блок и провалидировать сейчас. На текущей прошивке он скомпилируется чисто (просто не будет реальных регистров для чтения). Валидатор схемы проверяет, что одиночные и phased группы не смешиваются в одном блоке `sensor:`.

```yaml
# Forward-compat трёхфазный конфиг — компилируется сегодня, данные сенсоров
# станут доступны когда выйдет прошивка rbAmp-U3I3. НЕ ИСПОЛЬЗОВАТЬ на
# текущих одноканальных UI* — показания будут 0.
sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage_a:
      name: "Phase A Voltage"
    voltage_b:
      name: "Phase B Voltage"
    voltage_c:
      name: "Phase C Voltage"
    current_a:
      name: "Phase A Current"
    current_b:
      name: "Phase B Current"
    current_c:
      name: "Phase C Current"
    power_a:
      name: "Phase A Power"
    power_b:
      name: "Phase B Power"
    power_c:
      name: "Phase C Power"
    power_total:
      name: "Total Power"
    energy_a:
      name: "Phase A Energy"
    energy_b:
      name: "Phase B Energy"
    energy_c:
      name: "Phase C Energy"
    frequency:
      name: "Grid Frequency"
```

**Что демонстрирует**: phased-группа полей. Смешение одиночных и phased полей в одном блоке `sensor:` выдаёт compile-time ошибку — выбирайте группу по своему аппаратному SKU. Определение топологии — в [SPEC §8](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 15 · Миграция с других ESPHome компонентов учёта

### С `pzem004t`

Платформа `pzem004t` экспортирует те же имена полей что и `rbamp` для общих AC-величин. Замена — блочная с идентичными именами сенсоров:

```yaml
# До (pzem004t, UART)
uart:
  rx_pin: GPIO16
  tx_pin: GPIO17
  baud_rate: 9600

sensor:
  - platform: pzem004t
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

# После (rbamp, I²C) — те же имена сенсоров → те же entity ID в HA
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz

rbamp:
  id: meter1
  address: 0x50
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

Идентичные поля `name:` дают идентичные entity ID в HA. История Energy-дашборда сохраняется через swap: `total_increasing` сенсор подхватывает где оборвался. Уберите блок `uart:` если он не общий с другим компонентом.

### С `cse7766` (Sonoff POW R2)

Тот же паттерн. `cse7766` предоставляет `voltage`, `current`, `power`, `energy`, `power_factor`, `apparent_power` — все есть в `rbamp`. Замените `platform: cse7766` на `platform: rbamp`, добавьте `rbamp_id: meter1`, уберите `uart:`, добавьте `i2c:` и блок `rbamp:`.

---

## Свод правил валидации YAML

| Правило | Применение |
|---|---|
| `power` / `energy` / `power_factor` / `reactive_power` требуют `current` | Валидатор схемы (`_SINGLE_SLOT_DEPS`) |
| P/Q/PF/E для любого канала требуют `voltage` | Валидатор схемы |
| Одиночные и phased группы не смешиваются в одном `sensor:` | `_validate_topology_consistency` |
| `new_address` ≠ `address` | `_validate_new_address` |
| `address` в диапазоне `0x08..0x77` | `cv.i2c_address` |
| `update_interval` минимум: не enforce'нут; практический — `10s` | `cv.polling_component_schema` |
| `ct_model:` и `ct_models:` взаимоисключающие | Валидатор схемы |

Ошибки валидации сообщаются в `esphome compile` time с человекочитаемым сообщением, указывающим на проблемный ключ. Не нужно прошивать железо чтобы найти ошибку в конфигурации.



---

← [Быстрый старт](05_quickstart.md) · [Оглавление](README.md) · [DIY-интеграции](07_diy_integrations.md) →
