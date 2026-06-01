# 09 · Справочник YAML-схемы

Эта глава — полный справочник YAML-схемы внешнего компонента `rbamp` для ESPHome. Если какого-то ключа нет здесь — его нет в схеме.

Содержание:

1. [Блок компонента — `rbamp:`](#1-блок-компонента--rbamp)
2. [Сенсорная платформа — `sensor.platform: rbamp`](#2-сенсорная-платформа--sensorplatform-rbamp)
    - [Одиночные поля (single-phase)](#21-одиночные-поля)
    - [Phased поля (будущие SKU)](#22-phased-поля-будущие-sku)
    - [Общие поля](#23-общие-поля-topology-независимые)
    - [Правила валидации](#24-правила-валидации-схемы)
3. [Поток данных и тайминги](#3-поток-данных-и-тайминги)
4. [Настройки I²C-шины](#4-настройки-i²c-шины)

---

## 1. Блок компонента — `rbamp:`

Верхнеуровневый блок `rbamp:` регистрирует экземпляр компонента. Наследует от `PollingComponent` (даёт `update_interval`) и `i2c.I2CDevice` (даёт `address` и handle I²C-шины). `MULTI_CONF: True` — любое число блоков `rbamp:` может сосуществовать в одном YAML, каждый ссылается на свой I²C-slave.

```yaml
rbamp:
  id: meter1
  address: 0x50
  update_interval: 60s
  drdy_pin: GPIO4
  sensor_class: SCT_013
  ct_model: SCT_013_030
  # или для UI3 со смешанными клипсами:
  # ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  bidirectional: false
  new_address: 0x51
  broadcast_latch: false
  topology: SINGLE
```

---

### `id`

| Атрибут | Значение |
|---|---|
| Тип | `id` (идентификатор ESPHome) |
| По умолчанию | Авто-генерируется |
| Обязателен | Нет |

Назначает стабильный идентификатор экземпляру. Сенсоры ссылаются через `rbamp_id: meter1`. При нескольких блоках `rbamp:` явный `id` обязателен.

```yaml
rbamp:
  id: kitchen_meter
```

---

### `address`

| Атрибут | Значение |
|---|---|
| Тип | `i2c_address` (7-bit, 0x08..0x77) |
| По умолчанию | `0x50` |
| Обязателен | Нет |

Текущий I²C-адрес модуля rbAmp. Заводской по умолчанию — `0x50`. После применения смены адреса (через `new_address`) — обновите этот ключ на новое значение и удалите `new_address`.

Допустимый диапазон: `0x08..0x77` (зарезервированные 7-bit I²C-адреса исключены).

```yaml
rbamp:
  address: 0x52   # два модуля на шине: 0x50, 0x51, 0x52
```

Cross-reference: [SPEC.md §2](https://rbamp.com/docs/modules-basic-standard-api-reference) для bus protocol; [§10](https://rbamp.com/docs/modules-basic-standard-api-reference) для смены адреса.

---

### `update_interval`

| Атрибут | Значение |
|---|---|
| Тип | `time` (ESPHome duration string) |
| По умолчанию | `60s` |
| Обязателен | Нет |

Как часто запускается `update()`. Каждый вызов:

1. Отправляет latch-команду и планирует non-blocking 50 мс таймаут на чтение period-snapshot.
2. Проверяет статус-регистр; если модуль готов — публикует все привязанные мгновенные сенсоры.

Дефолт 60 с разумен: модуль внутренне обновляет среднюю мощность за период ~200 мс, но интегрирование энергии на стороне мастера требует достаточно длинного окна для корректности. При 60 с и средней нагрузке 60 Вт пропущенный latch теряет ≤ 1 Wh.

Минимально практичное значение — `10s`. Значения < `1s` не дают выгоды и засоряют шину.

```yaml
rbamp:
  update_interval: 30s   # более частые обновления дашборда, больше трафика
```

---

### `drdy_pin`

| Атрибут | Значение |
|---|---|
| Тип | `gpio_input_pin_schema` |
| По умолчанию | None (опционально) |
| Обязателен | Нет |

Подключает open-drain выход DRDY модуля к GPIO ESP32. При указании пин настраивается в `setup()` при загрузке.

> На текущей прошивке пин логируется в `dump_config`, но не используется как trigger чтения — мгновенные регистры опрашиваются по `update_interval`. Объявление пина не меняет поведение, но зарезервирует его под будущие ревизии прошивки с interrupt-driven чтениями.

```yaml
rbamp:
  drdy_pin: GPIO4
```

---

### `sensor_class`

| Атрибут | Значение |
|---|---|
| Тип | `enum` (`SCT_013`, `WIRED_CT`, `BUILTIN_CT`) |
| По умолчанию | `SCT_013` |
| Обязателен | Нет |

Фиксирует семейство датчика тока на стороне модуля. На прошивке v1.2+ значение записывается во flash и становится **предусловием** для записи CT-модели: модуль откажет в записи модели, если класс не выставлен. На ранних прошивках значение принимается схемой и применится автоматически при апгрейде.

| Значение | Состояние |
|---|---|
| `SCT_013` | Доступно сейчас, по умолчанию |
| `WIRED_CT` | Зарезервирован под будущие SKU |
| `BUILTIN_CT` | Зарезервирован под будущие SKU |

```yaml
rbamp:
  sensor_class: SCT_013   # дефолт; можно опустить
```

Подробнее о выборе клипсы и семейства — в [03_sensor_selection.md](03_sensor_selection.md).

---

### `ct_model`

| Атрибут | Значение |
|---|---|
| Тип | `enum` (`SCT_013_005`, `_010`, `_030`, `_050`, `_100`) |
| По умолчанию | None (опциональный) |
| Обязателен | Нет |

Записывает идентификатор модели CT-клипсы во flash модуля. На прошивке v1.2+ автоматически подгружает заводские коэффициенты под выбранную модель — никаких дополнительных calibration-шагов не требуется.

| YAML-значение | Номинальный ток |
|---|---|
| `SCT_013_005` | 5 А |
| `SCT_013_010` | 10 А |
| `SCT_013_030` | 30 А |
| `SCT_013_050` | 50 А |
| `SCT_013_100` | 100 А |

```yaml
rbamp:
  ct_model: SCT_013_030
```

Применяется один раз в `setup()`. Каждая запись сопровождается ~700 мс flash-write (модуль NACK'ает все I²C-операции в это время — компонент кормит watchdog автоматически).

**Взаимно исключающий** с `ct_models:` — за один блок `rbamp:` можно использовать только один из двух.

Подробнее — в [03_sensor_selection.md](03_sensor_selection.md). Cross-reference: [SPEC.md §11](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `ct_models`

| Атрибут | Значение |
|---|---|
| Тип | список из **1–3** enum-значений |
| По умолчанию | None (опциональный) |
| Обязателен | Нет |

Per-channel модели CT-клипс — для UI2/UI3 со смешанными клипсами на разных каналах. Принимает те же enum-значения что `ct_model:`. Схема валидирует длину массива через `cv.Length(min=1, max=3)`; количество элементов должно совпадать с количеством физических каналов модуля (1 для UI1, 2 для UI2, 3 для UI3).

```yaml
rbamp:
  id: ui3_meter
  ct_models: [SCT_013_005, SCT_013_030, SCT_013_100]
  # CH0=5A для дежурных нагрузок (макс. разрешение),
  # CH1=30A для основных бытовых нагрузок,
  # CH2=100A для общего ввода / EV-зарядки
```

На прошивке v1.2+ каждый канал получает свой набор заводских коэффициентов независимо. Общее время setup'а при загрузке — ~2-3 секунды (~700 мс flash-write на каждый канал).

**Взаимно исключающий** с `ct_model:`.

---

### `bidirectional`

| Атрибут | Значение |
|---|---|
| Тип | `bool` |
| По умолчанию | `false` |
| Обязателен | Нет |

Включает ветку чтения регистра экспортной энергии для тиров STANDARD / PRO. При `true` компонент пытается прочитать per-channel регистры экспортной мощности на каждом period-snapshot и накапливать Wh экспорта отдельно.

**Состояние на текущей прошивке**: ключ принимается схемой и резервирует слоты `energy_export_wh[]` в NVS, но регистр экспортной энергии пока не подключён в прошивке. Сенсоры `energy_exported_*` публикуют 0 до выхода прошивки с этим регистром.

Объявляйте `energy_exported` (или `_1` / `_2`) под `sensor.platform: rbamp` только когда выставлен `bidirectional: true`.

```yaml
rbamp:
  bidirectional: true   # имеет смысл на STANDARD / PRO
```

---

### `new_address`

| Атрибут | Значение |
|---|---|
| Тип | `i2c_address` (0x08..0x77) |
| По умолчанию | None (опциональный) |
| Обязателен | Нет |

Триггерит однократную смену I²C-адреса при загрузке. Должен отличаться от `address` — валидатор поднимет ошибку если они равны.

Полный поток выполняется один раз в `setup()`:

1. Опрос текущего `address`. Если модуль не отвечает там, но отвечает на `new_address` — компонент адаптируется к новому адресу с warning'ом (предполагает что предыдущая загрузка уже применила смену) и пропускает запись.
2. Проверка готовности модуля к провизионной операции. Если модуль не в provisioning-режиме — смена пропускается с warning'ом.
3. Запись нового адреса во flash (~700 мс), мягкий ресет модуля (~300 мс), повторный lock на AC-цикл.
4. Компонент переключает внутренний I²C-адрес на `new_address` и проверяет, что модуль отвечает.
5. Если модуль не отвечает после смены — компонент вызывает `mark_failed()` и останавливается.

**После успешной смены**: обновите YAML на `address: <new>` и удалите `new_address:`. Если модуль на новом адресе при следующей загрузке а `new_address:` всё ещё в YAML — boot-time probe старого адреса упадёт, probe нового адреса пройдёт, компонент адаптируется с warning'ом, ре-запись не произойдёт.

> **WARNING — `new_address:` требует factory-provisioning mode**
>
> Стандартные production-модули поставляются с отключённым provisioning-режимом. Запись `new_address:` на таком модуле будет отклонена (warning в логе ESPHome при загрузке), и адрес не сменится. Если вам нужна смена адреса в полевых условиях — обратитесь к документации модуля или поставщику за процедурой временного включения provisioning-режима.

```yaml
rbamp:
  address: 0x50
  new_address: 0x51   # удалите эту строку после первой успешной загрузки
```

**Восстановление**: если смена адреса применилась но модуль не отвечает — см. [10_troubleshooting.md](10_troubleshooting.md) → "Адрес сменился, но модуль не отвечает".

Cross-reference: [SPEC.md §10](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `broadcast_latch`

| Атрибут | Значение |
|---|---|
| Тип | `bool` |
| По умолчанию | `false` |
| Обязателен | Нет |

Включает I²C General-Call broadcast для latch-команды, синхронизирующий несколько модулей rbAmp на одной шине одной записью на широковещательный адрес.

**Ограничение на текущей прошивке**: I²C general-call отключён на стороне модуля — записи на широковещательный адрес тихо игнорируются. Компонент видит версию прошивки и логирует warning:

```
broadcast_latch: true requested but firmware v0x01 has I2C general-call
DISABLED — broadcasts will be dropped by the slave.
```

Когда `broadcast_latch: true` выставлен и warning сработал — компонент откатывается на последовательный latch для каждого модуля. Skew между модулями при 50 кГц шине ~ `270 мкс × N`; для трёх модулей на 60 с cadence это < 0,0015% относительной ошибки — невидимо в дневных суммах HA.

Оставляйте ключ в YAML при апгрейде прошивки — warning исчезнет автоматически когда прошивка добавит поддержку broadcast.

```yaml
rbamp:
  broadcast_latch: false   # выставьте true когда прошивка добавит broadcast
```

Cross-reference: [SPEC.md §9](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

### `topology`

| Атрибут | Значение |
|---|---|
| Тип | `enum` (`SINGLE`, `SPLIT_PHASE`, `THREE_PHASE`) |
| По умолчанию | `SINGLE` |
| Обязателен | Нет |

Объявляет физическую конфигурацию модуля.

| Значение | Текущая прошивка | Когда станет authoritative |
|---|---|---|
| `SINGLE` | Косметика (логируется в `dump_config`). Число каналов выводится из объявленных слотов `current[_1/_2]`. | Уже совпадает с любым текущим SKU; будет confirm'нуто из in-band регистра после его релиза в прошивке. |
| `SPLIT_PHASE` | Принимается схемой, в `dump_config` пишется. Phased keys (`voltage_a/b/c`, `current_a/b/c`, …) в `sensor.platform: rbamp` объявлять можно — компонент тогда зарезервирует слоты, но публиковать в них пока некого. | После релиза SKU rbAmp-U2I2 с in-band регистром топологии. |
| `THREE_PHASE` | То же что `SPLIT_PHASE`. | После релиза SKU rbAmp-U3I3 с in-band регистром топологии. |

На текущей прошивке нет in-band регистра топологии (зарезервирован под будущие ревизии). Подсказка информационная: идёт в строку `dump_config`, но реальное число каналов выводится независимо из объявленных слотов сенсоров `current[_1/_2]`.

```yaml
rbamp:
  topology: SINGLE         # UI1, UI2, UI3, I1, I2, I3 — текущие SKU
  # topology: SPLIT_PHASE  # US split-phase (U2I2) — будущий SKU
  # topology: THREE_PHASE  # европейский 3-phase (U3I3) — будущий SKU
```

Когда модуль начнёт публиковать топологию через свой регистр — компонент будет предпочитать значение от модуля и использовать YAML-подсказку только как fallback. Дефолт `SINGLE` совпадает с каждым текущим SKU — изменений в развёрнутых конфигах не потребуется.

Cross-reference: [SPEC.md §8](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 2. Сенсорная платформа — `sensor.platform: rbamp`

Каждый блок `sensor:` объявляет один набор именованных сенсоров, привязанных к родительскому компоненту `rbamp:`. Единственный обязательный ключ в блоке — `rbamp_id`.

```yaml
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

Все поля под блоком `sensor:` опциональны. Объявляйте только те величины, которые нужны вашему сценарию — компонент читает только регистры, соответствующие объявленным сенсорам.

### `rbamp_id`

| Атрибут | Значение |
|---|---|
| Тип | `use_id(RbAmpComponent)` |
| Обязателен | **Да** |

Ссылается на блок `rbamp:` к которому привязана эта группа сенсоров.

---

### 2.1 Одиночные поля

Используются для текущих SKU (UI1, UI2, UI3, I1, I2, I3). Все поля опциональны. Смешение с phased полями (`voltage_a` и т.д.) вызывает ошибку валидации.

Каждое поле принимает стандартные подключи `sensor.sensor_schema` ESPHome: `name`, `id`, `filters`, `unit_of_measurement`, `accuracy_decimals`, `icon` и т.д.

#### `voltage`

| Атрибут | Значение |
|---|---|
| Единица | V |
| `device_class` | `voltage` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Источник | RMS напряжение сети (мгновенный регистр модуля) |

Среднеквадратичное напряжение сети. Читается каждый `update()` когда модуль готов. 4-байтный float читается с per-byte retry (3 попытки × 5 мс) и проходит проверку `std::isfinite()` + `|val| < 10000`.

Ноль — валидное публикуемое значение: событие отключения mains или brownout даёт U ≈ 0 V и проходит в HA без фильтрации (см. [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)).

```yaml
voltage:
  name: "Mains Voltage"
  filters:
    - sliding_window_moving_average:
        window_size: 3
        send_every: 1
```

#### `current` / `current_1` / `current_2`

| Атрибут | Значение |
|---|---|
| Единица | A |
| `device_class` | `current` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 3 |

Среднеквадратичный ток для каналов 0, 1, 2 соответственно. Канал 0 — основная CT-клипса; каналы 1 и 2 присутствуют на SKU UI2 / UI3 и I2 / I3.

Активное число каналов выводится из числа объявленных слотов: если объявлен только `current` — `n_channels_ = 1`; если `current` и `current_1` — `2`; добавление `current_2` даёт `3`.

```yaml
current:
  name: "Phase Current"
current_1:
  name: "Load 1 Current"
current_2:
  name: "Load 2 Current"
```

#### `power` / `power_1` / `power_2`

| Атрибут | Значение |
|---|---|
| Единица | W |
| `device_class` | `power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Зависимости | `power` требует `current` + `voltage`; `power_1` требует `current_1` + `voltage`; `power_2` требует `current_2` + `voltage` |

Активная мощность в ваттах, **со знаком**. Отрицательные значения = реверсный поток (генерация в сеть) на тирах STANDARD / PRO. На BASIC отрицательные мгновенные значения внутри окна периода клампятся в 0 на уровне прошивки — period-средняя `≥ 0`. Мгновенная активная мощность всё равно может читаться отрицательной в момент генерации.

Объявление `power` без `current` + `voltage` поднимает ошибку валидации:
`power requires current to also be declared`.

```yaml
power:
  name: "Active Power"
```

#### `energy` / `energy_1` / `energy_2`

| Атрибут | Значение |
|---|---|
| Единица | Wh |
| `device_class` | `energy` |
| `state_class` | `total_increasing` |
| `accuracy_decimals` | 3 |
| Источник | Master-side accumulator (не регистр модуля) |
| Зависимости | Требует `current` + `voltage` |

Накопленная потреблённая энергия в Wh. Считается полностью на ESP32 по формуле:

```
E_Wh[ch] += avg_P_W[ch] * master_dt_s / 3600
```

где `avg_P_W[ch]` — средняя мощность за период, прочитанная с модуля после каждой latch-команды, а `master_dt_s` — wall-clock интервал ESP32 между latch'ами.

Значения сохраняются в NVS каждые 5 минут и восстанавливаются до первого `publish_state` при загрузке — это предохраняет Energy-дашборд HA от интерпретации мгновенного 0 как сброса счётчика. Худший случай потери при внезапном пропадании питания — до 5 минут энергии (≈ 5 Wh при средней 60 Вт).

`state_class: total_increasing` обязателен для Energy-дашборда HA. Значение монотонно растёт; не уменьшается при нормальной работе. При смене layout NVS (bump версии) счётчик стартует с 0.

```yaml
energy:
  name: "Mains Energy"
```

#### `energy_exported` / `energy_exported_1` / `energy_exported_2`

| Атрибут | Значение |
|---|---|
| Единица | Wh |
| `device_class` | `energy` |
| `state_class` | `total_increasing` |
| `accuracy_decimals` | 3 |
| Источник | Master-side export accumulator |
| Зависимости | Требует `current` + `voltage` + `bidirectional: true` |

Энергия экспорта (генерации) в Wh, накапливается отдельно от `energy`. Подключена в компоненте, но публикует 0 пока прошивка не добавит соответствующий регистр period-negative-power. Выставьте `bidirectional: true` в блоке `rbamp:` при объявлении этих сенсоров.

#### `power_factor` / `power_factor_1` / `power_factor_2`

| Атрибут | Значение |
|---|---|
| Единица | (безразмерная) |
| `device_class` | `power_factor` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 3 |
| Зависимости | Требует `current` + `voltage` для соответствующего слота |

Коэффициент мощности в диапазоне −1..+1. Отрицательный PF — leading или lagging нагрузка, знаковая конвенция определена прошивкой. Sanity-фильтр (§B.5) отбрасывает значения вне `|pf| > 10000`; нижней границы нет.

```yaml
power_factor:
  name: "Power Factor"
```

#### `reactive_power` / `reactive_power_1` / `reactive_power_2`

| Атрибут | Значение |
|---|---|
| Единица | VAr |
| `device_class` | `reactive_power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Зависимости | Требует `current` + `voltage` для соответствующего слота |

Реактивная мощность в VAr. Со знаком. Публикуется из блока мгновенных регистров каждый цикл `update()`.

```yaml
reactive_power:
  name: "Reactive Power"
```

> **Примечание о `device_class: reactive_power`** — ESPHome принимает его
> (константа `DEVICE_CLASS_REACTIVE_POWER`), и значение проходит в Home
> Assistant. Однако официальный список `device_class` Home Assistant
> исторически менялся: в одних версиях `reactive_power` доступен в UI
> и работает с unit_of_measurement `VAr`, в других — отображается как
> generic-сенсор без специализированной иконки или unit-конвертации.
> Сами данные публикуются всегда (это просто `state`), вопрос лишь в
> том, как HA отрендерит сенсор в Lovelace. Если в вашей версии HA
> сенсор отображается как unknown — это косметика, не функциональный
> регресс. Удалите ключ `reactive_power:` из YAML, если не пользуетесь
> сенсором.

#### `apparent_power`

| Атрибут | Значение |
|---|---|
| Единица | VA |
| `device_class` | `apparent_power` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 1 |
| Источник | Вычисляется на мастере: `S = V_rms × I_rms[0]` |
| Зависимости | Требует `current` + `voltage` |

Полная мощность в VA. Считается компонентом из значений `U_rms` и `I0_rms`, прочитанных в том же `update()` цикле — отдельный регистр не читается. Оба чтения должны успеть в одном цикле; если хотя бы одно упало (исчерпан retry) — состояние не публикуется в этом цикле, последнее значение в HA сохраняется.

`apparent_power` лежит в `SHARED_FIELDS` и работает с любой топологией, но зависит от `voltage` и `current` для V × I расчёта.

```yaml
apparent_power:
  name: "Apparent Power"
```

---

### 2.2 Phased поля (будущие SKU)

Зарезервированы под split-phase (U2I2, US-рынок) и three-phase (U3I3) будущие SKU. Схема принимает их сейчас — пользователь может заранее подготовить конфигурацию. На текущей прошивке объявление phased-слотов читает соответствующие адреса регистров (возвращающие `0.0` для нереализованных каналов) и публикует 0 — не ошибка, но и не полезные данные.

Одиночные и phased поля **взаимоисключающие** в одном блоке `sensor:`. Валидатор поднимает ошибку:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...).
```

Все phased-поля разделяют те же `sensor_schema` defaults, что и их single-phase аналоги (те же единицы, `device_class`, `state_class`, `accuracy_decimals`).

| Поле | Единица | `device_class` |
|---|---|---|
| `voltage_a` / `_b` / `_c` | V | voltage |
| `current_a` / `_b` / `_c` | A | current |
| `power_a` / `_b` / `_c` | W | power |
| `power_total` | W | power |
| `energy_a` / `_b` / `_c` | Wh | energy |
| `energy_exported_a` / `_b` / `_c` | Wh | energy |
| `power_factor_a` / `_b` / `_c` | — | power_factor |
| `reactive_power_a` / `_b` / `_c` | VAr | reactive_power |

> Per-phase валидация зависимостей не enforce'нута для phased полей (только `_SINGLE_SLOT_DEPS` для single-phase). Поддержка прошивкой split / three-phase вариантов зарезервирована под будущие SKU.

Пример для будущего трёхфазного deployment'а:

```yaml
rbamp:
  id: panel_meter
  address: 0x50
  topology: THREE_PHASE

sensor:
  - platform: rbamp
    rbamp_id: panel_meter
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
    power_total:
      name: "Total Active Power"
```

---

### 2.3 Общие поля (topology-независимые)

Два поля работают в любой топологической группе, включая случай когда ни одного single-phase или phased current-сенсора не объявлено (voltage-only или frequency-only deployment).

#### `frequency`

| Атрибут | Значение |
|---|---|
| Единица | Hz |
| `device_class` | `frequency` |
| `state_class` | `measurement` |
| `accuracy_decimals` | 0 |
| Тип | `uint8_t` (не float) |

Частота сети, читается одним байтом. Компонент публикует значение только если оно равно 50 или 60 — другие значения (`0` = ZC не пойман, неправдоподобно большие) отбрасываются без публикации. Это избегает перехода "Unknown" → 0 Hz → 50 Hz в сущности HA во время warm-up.

```yaml
frequency:
  name: "Mains Frequency"
```

#### `apparent_power` (общий)

См. идентичную запись в одиночных полях выше. `apparent_power` входит в `SHARED_FIELDS` и может появляться в single-phase или phased блоке сенсоров, при условии что `voltage` и `current` (или `current_a`) тоже объявлены.

---

### 2.4 Правила валидации схемы

Python-валидатор `_validate_topology_consistency` применяет два класса правил во время `esphome config` / compile time — до того, как любой C++ код запустится.

#### Topology mutual exclusion

Single-phase поля (`voltage`, `current`, `current_1` и т.д.) и phased поля (`voltage_a`, `voltage_b` и т.д.) не могут сосуществовать в одном блоке `sensor:`. Попытка смешения:

```
Cannot mix single-phase fields (voltage, current, current_1, ...) with
phased fields (voltage_a/b/c, current_a/b/c, ...). Pick one group based
on your rbAmp SKU.
```

#### Per-slot companion requirements (только single-phase)

Каждое производное поле требует входных полей, нужных модулю для расчёта:

| Объявленное поле | Требуемые компаньоны |
|---|---|
| `power` / `_1` / `_2` | `current` (или `_1` / `_2`) + `voltage` |
| `energy` / `_1` / `_2` | `current` (или `_1` / `_2`) + `voltage` |
| `power_factor` / `_1` / `_2` | `current` (или `_1` / `_2`) + `voltage` |
| `reactive_power` / `_1` / `_2` | `current` (или `_1` / `_2`) + `voltage` |
| `apparent_power` | `current` + `voltage` |

Отсутствующий компаньон поднимает:

```
`power` requires `current` to also be declared — the underlying chip
cannot compute it without that input.
```

#### Прочие правила на уровне блока `rbamp:`

| Правило | Описание |
|---|---|
| `ct_model:` ↔ `ct_models:` взаимно исключающие | За один блок `rbamp:` можно использовать только один из двух — иначе ошибка валидации. |
| `new_address` ≠ `address` | Если совпадают — `cv.Invalid`. |
| `address` в диапазоне `0x08..0x77` | Применяется `cv.i2c_address`. |

Все ошибки валидации сообщаются во время `esphome compile` с человекочитаемым сообщением, указывающим на проблемный ключ. Не нужно прошивать железо чтобы найти ошибку в конфигурации.

---

## 3. Поток данных и тайминги

Концептуальный поток одного цикла `update()`:

```
Модуль (автономно)                  ESP32 (ESPHome компонент)
─────────────────────────────────   ────────────────────────────────────────────
Внутренняя выборка ADC и
вычисление RMS / P / PF / Q
  ↓ (~200 мс на цикл)
Атомарно публикует блок             update() стреляет каждые update_interval (60 с)
мгновенных регистров                  ↓
  ↓                                 фаза 1 — latch:
                                      запись latch-команды
Закрывает накопитель за период,       таймаут 50 мс (non-blocking)
открывает новый                       ↓ (main loop работает дальше)
  ↓
  ожидание 50 мс                    фаза 2 — period-snapshot:
                                      чтение valid-флага
                                      чтение средних мощностей по каналам
                                      E_Wh[ch] += avg_P × dt_s / 3600
                                      сохранение в NVS если прошло 5 минут
                                      ↓
                                    фаза 3 — мгновенные значения:
                                      чтение статус-регистра
                                      чтение U_rms, I[0..n]_rms, P[0..n],
                                            PF[0..n], Q[0..n], частоты
                                      публикация всех привязанных сенсоров
                                      ↓
                                    Передача состояния в HA по native API
```

**Характерные тайминги**:

| Событие | Cadence |
|---|---|
| Внутренний commit мгновенного блока модулем | ~200 мс |
| Вызов `update()` | `update_interval` (по умолчанию 60 с) |
| Settle-таймаут после latch-команды | 50 мс (non-blocking) |
| Сохранение в NVS | Каждые 5 минут |
| Пауза между retry на байт | 5 мс, до 3 попыток |
| Окно flash-write при записи модели | ~700 мс (модуль NACK'ает всё это время) |

Таймаут на latch-settle non-blocking — кооперативный планировщик ESPHome продолжает обрабатывать WiFi, API и другие компоненты во время 50 мс ожидания. Остальные фазы `update()` (чтение мгновенных регистров) идут сразу после возврата из latch-фазы.

Warning `[W][component:522]: rbamp took a long time for an operation (XXX ms)` каждый цикл — ожидаемый и безвредный. Отражает wire-time для до 36 транзакций I²C (4 байта на каждый из 9 float-регистров) на 50 кГц. См. [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference).

---

## 4. Настройки I²C-шины

Компонент `rbamp` наследует I²C-шину, сконфигурированную в верхнеуровневом блоке `i2c:`. Рекомендуемые настройки на текущей прошивке:

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz   # 100 кГц вызывает периодические NACK; 50 кГц снижает ~в 5 раз
  scan: true         # опционально: логирует найденные адреса при загрузке
```

**Скорость шины**: используйте `50kHz` на текущей прошивке. I²C-периферия модуля периодически NACK'ает на 100 кГц (~20% транзакций) из-за известного поведения драйвера ESP-IDF `i2c_master` (подтверждено в [esp-idf issue #9426](https://github.com/espressif/esp-idf/issues/9426), помечен "Won't Do" со стороны Espressif). Трёхслойная митигация в компоненте (retry + sanity + 50 кГц) снижает эффективную частоту плохих чтений ниже 1%. Когда выйдет фикс в прошивке — скорость можно будет вернуть на 100 кГц одной строкой YAML (см. [SPEC §B.5](https://rbamp.com/docs/modules-basic-standard-api-reference)).

**Pull-up резисторы**: 4,7 кОм к 3,3 В на SDA и SCL рекомендуется. Большие значения (10 кОм) увеличивают rise time и поднимают NACK rate на 100 кГц.

**Несколько устройств**: I²C-шина ESP32 поддерживает до 112 устройств на разных 7-bit адресах. Используйте `address:` и `new_address:` для назначения уникальных адресов каждому модулю. `scan: true` в блоке `i2c:` логирует все ответившие адреса при загрузке — проверьте, что каждый модуль обнаружен до включения period-метеринга.

> Полный wire-level register map и описание команд модуля — в [SPEC.md](https://rbamp.com/docs/modules-basic-standard-api-reference). Для типового пользователя YAML-схемы эта глава достаточна — все регистры скрыты за декларативным интерфейсом компонента.


---

← [Home Assistant](08_has_integrations.md) · [Оглавление](README.md) · [Устранение неполадок](10_troubleshooting.md) →
