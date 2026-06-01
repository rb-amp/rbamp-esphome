# 08 · Home Assistant — глубокое погружение

Эта глава о HA-специфичных деталях работы с rbAmp: discovery, нативный API, Energy-дашборд, long-term statistics, Lovelace-карточки, автоматизации и эксплуатация в окружениях где HA крутится в Docker или WSL.

Cross-references:

- Quickstart: [`05_quickstart.md`](05_quickstart.md)
- YAML-примеры (Lovelace, автоматизации): [`06_examples.md`](06_examples.md)
- DIY-интеграции (MQTT, InfluxDB, Lambda): [`07_diy_integrations.md`](07_diy_integrations.md)
- Документация HA: <https://www.home-assistant.io/>

---

## 1 · Discovery: mDNS или ручной IP

### mDNS auto-discovery (рекомендуется)

ESPHome-ноды объявляют о себе по mDNS (multicast DNS) когда в YAML есть блок `api:`. ESPHome-интеграция HA слушает эти объявления и автоматически предлагает новые устройства.

Что нужно чтобы auto-discovery сработал:

- ESP32 и HA должны быть в **одном Layer 2 сегменте сети** (один VLAN, без роутера между ними). mDNS — multicast, не пересекает роутер без дополнительной настройки (mDNS reflector / Avahi proxy).
- UDP multicast не должен блокироваться роутером или функцией изоляции клиентов на точке доступа. На бытовых AP часто есть "AP isolation" или "wireless isolation" — она блокирует device-to-device multicast. Отключите для IoT VLAN или конкретных устройств.
- HA должен корректно резолвить mDNS. В Docker / WSL deployment'ах это иногда требует дополнительной сетевой настройки (см. §10).

Поток discovery:

1. Прошейте ESP32 и дайте загрузиться. Он подключается к WiFi и начинает объявлять себя по mDNS на `<node_name>.local`.
2. В HA: Settings → Devices & Services → Integrations. Появится баннер "New devices found", или ищите ESPHome в списке интеграций с discovery-badge.
3. Нажмите "Configure" на новом entry. HA спросит ключ шифрования, если он сконфигурирован (§3).
4. Все сенсоры автоматически добавляются к устройству.

### Ручное добавление (если mDNS блокируется)

Settings → Devices & Services → Add Integration → ESPHome.

Введите IP ESP32 (посмотрите в DHCP-таблице роутера или в boot-логе ESPHome — строка `[I][wifi:189]: IP: 192.168.x.y`). Порт по умолчанию — `6053`.

Назначьте статический DHCP-lease для MAC-адреса ESP32 в роутере, чтобы IP не менялся после перезагрузки роутера.

---

## 2 · Native ESPHome API (порт 6053) vs MQTT

ESPHome поддерживает два транспорта. Оба работают с rbAmp.

| Свойство | Native API (порт 6053) | MQTT |
|---|---|---|
| Интеграция с HA | First-class: отдельная ESPHome-интеграция с auto-discovery | Generic: нужен MQTT-брокер, ручная настройка сущностей или MQTT discovery |
| Шифрование | Встроенное (noise protocol, опционально) | Зависит от TLS-настройки брокера |
| Латентность | Низкая — прямой TCP без брокера | Зависит от брокера |
| Надёжность | Авто-переподключение | Зависит от доступности брокера |
| Без HA | Бесполезно (API HA-специфичен) | Любой MQTT-subscriber может потреблять |
| Сложность настройки | Никакой дополнительной инфраструктуры | Нужен работающий MQTT-брокер |

**Рекомендация**: используйте native API (блок `api:`) когда HA — основной потребитель. Добавьте `mqtt:` только если нужен параллельный non-HA потребитель (Grafana, Node-RED, кастомные скрипты). Оба могут сосуществовать в одном YAML.

Native API — это путь, описанный в этом документе. Детали MQTT — в [`07_diy_integrations.md §2`](07_diy_integrations.md).

---

## 3 · Ключ шифрования

По умолчанию native API без шифрования — любой хост в локальной сети может подключиться к порту 6053. Это приемлемо для доверенной домашней сети. Для большей безопасности добавьте ключ:

```yaml
api:
  encryption:
    key: "BASE64_ENCODED_32_BYTE_KEY"
```

Генерация ключа:

```sh
python3 -c "import base64, os; print(base64.b64encode(os.urandom(32)).decode())"
```

При добавлении устройства в HA (Settings → Devices & Services → ESPHome → Add) введите тот же ключ. HA сохранит его в конфигурации интеграции и будет использовать автоматически для всех последующих подключений к этой ноде.

Положите ключ в `secrets.yaml`:

```yaml
# secrets.yaml (untracked, рядом с YAML устройства)
api_key: "BASE64_ENCODED_32_BYTE_KEY"
```

```yaml
# YAML устройства
api:
  encryption:
    key: !secret api_key
```

При смене ключа нужно удалить и заново добавить устройство в HA.

---

## 4 · Конфигурация Energy-дашборда

Сенсоры rbAmp `energy` (и `energy_1`, `energy_2`, `energy_exported`, и т.д.) приходят с уже выставленными:

```
device_class: energy
state_class: total_increasing
unit_of_measurement: Wh
```

Это ровно та комбинация, которую Energy-дашборд HA ожидает для накопительных сенсоров потребления или генерации. Дополнительного YAML не нужно.

### Добавление сенсоров в Energy-дашборд

Settings → Dashboards → Energy. Дашборд состоит из четырёх секций:

| Секция | Какой сенсор rbAmp использовать |
|---|---|
| Grid consumption | `energy` (импорт из сети) |
| Return to grid | `energy_exported` (тиры STANDARD / PRO; на BASIC и v1 прошивке читает 0) |
| Solar panels | `energy` с модуля, считающего выход солнечного инвертора |
| Home battery | Неприменимо (rbAmp не измеряет DC) |
| Individual devices | `energy_1`, `energy_2` и т.д. с многоканального модуля |

Для однокомпонентной установки на весь дом:

1. Нажмите "Add consumption" под Grid consumption.
2. Выберите сущность `Mains Energy`.
3. Опционально выставьте тариф (EUR/кВт·ч или ваша локальная валюта) для учёта стоимости.

Для установки из трёх модулей (grid + solar + EV из Примера 11 в [`06_examples.md`](06_examples.md)):

1. Grid consumption: `House Energy` (из `meter_house`).
2. Return to grid: `Solar Energy Exported` (из `meter_solar`, тиры STANDARD / PRO; на BASIC оставьте пустым).
3. Solar panels: `Solar Energy` (из `meter_solar`).
4. Individual device: `EV Charger Energy` (из `meter_evcharger`).

### NVS-сохранение и Energy-дашборд

Компонент ESPHome сохраняет суммарную энергию в NVS ESP32 каждые 5 минут. При загрузке значения восстанавливаются из NVS и публикуются сразу — **до** подключения HA и **до** первого срабатывания `update_interval`.

Это спасает Energy-дашборд от интерпретации мгновенного `0 Wh` как сброса счётчика. Если HA увидит, что `total_increasing` упал в 0, он трактует разницу как границу биллингового периода и сбросит накопленную историю.

Худший случай потери данных при внезапном пропадании питания — до 5 минут накопления (≈5 Wh при средней нагрузке 60 Вт — невидимо в дневных суммах).

### `last_reset` после OTA или перезагрузки ESP32

HA Energy-дашборд использует device_class `energy` + state_class
`total_increasing` без явной привязки к `last_reset` — это
рекомендованная для ESPHome конфигурация. При перезагрузке ESP32
компонент восстанавливает значение из NVS **до** первого `publish_state`,
поэтому HA видит непрерывную монотонно растущую серию и **не вставляет**
sentinel-сброс в график. Это работает по дизайну.

Edge-cases, в которых вы можете увидеть видимый «сброс»:

- **NVS повреждён** (магия не совпадает или CRC не проходит). Компонент
  стартует с 0 Wh, и HA фиксирует разрыв. Симптом: dashboard показывает
  «утренний пик» в момент загрузки узла после долгого простоя.
  *Фикс*: проверьте лог ESP32 при загрузке на сообщения
  `[E][rbamp_nvs]` — там укажет причина (несовместимый layout NVS,
  низкий заряд в момент write, и т. п.). Восстановите из бэкапа
  HA-историй или прочно вычистите фантомный пик через утилиты HA.

- **Замена железа** (новый ESP32, тот же модуль). Компонент стартует
  с 0 Wh — модуль не передаёт накопленный счёт через I²C (модуль и
  компонент имеют отдельные накопители; модуль предоставляет только
  мгновенные значения для интегрирования). HA фиксирует разрыв.
  *Фикс*: после прошивки нового ESP32 ручным `mqtt.publish` (для MQTT)
  или временной правкой YAML с lambda-инициализацией внесите начальное
  значение энергии. Или примите разрыв как одноразовое событие.

- **`update_interval:` изменён между прошивками с заметным расхождением**
  (например 60 → 600 с). Изменение само по себе ничего не сбросит, но
  длительные циклы дают более грубые «ступеньки» в графике. Visually
  выглядит как смена режима накопления; функционально — ОК.

- **HA recorder обрезан или восстановлен из старого бэкапа**. Это не
  на стороне ESPHome — это HA-сторона: HA может посчитать какие-то
  будущие значения как «прошлое» и перерисовать график. К ESPHome
  отношения не имеет.

При штатной OTA-перепрошивке `last_reset` не задаётся, NVS-snapshot
восстанавливается за ~50 мс до подключения HA, и Energy-дашборд не
видит разрыв. Это базовый сценарий.

---

## 5 · Utility meter

Интеграция HA `utility_meter` выводит сенсоры с периодическим сбросом из любого `total_increasing` сенсора. Используйте её чтобы получить дневные, недельные и месячные кВт·ч помимо сырого накопителя.

```yaml
# configuration.yaml
utility_meter:
  energy_daily:
    source: sensor.rbamp_ui1_mains_energy
    cycle: daily

  energy_monthly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: monthly

  energy_yearly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: yearly
```

HA создаёт три новые сущности (`sensor.energy_daily` и т.д.), сбрасывающиеся в 0 в начале каждого периода. Они отлично подходят для individual device views в Energy-дашборде и как основа для template-сенсоров учёта стоимости.

Примеры стоимости и кВт·ч/день — в [`06_examples.md §10`](06_examples.md).

---

## 6 · Statistics sensor для min / max / average

Сенсор HA `statistics` считает скользящие min, max, mean и стандартное отклонение по конфигурируемому временному окну на основе истории любого сенсора:

```yaml
# configuration.yaml
sensor:
  - platform: statistics
    name: "Mains Power 1h Average"
    entity_id: sensor.rbamp_ui1_mains_power
    state_characteristic: mean
    sampling_size: 60          # последние 60 точек
    max_age:
      hours: 1

  - platform: statistics
    name: "Mains Power 24h Peak"
    entity_id: sensor.rbamp_ui1_mains_power
    state_characteristic: value_max
    max_age:
      hours: 24
```

Производные сенсоры полезны для demand management (окно пикового потребления, месячный максимум для тарифного биллинга) и детектирования аномалий (если 24-часовой максимум мощности внезапно превысил исторические нормы).

---

## 7 · Long-term statistics

HA пишет сенсоры с `state_class: total_increasing` и `state_class: measurement` в свою long-term statistics базу (LTS). LTS-данные хранятся бессрочно (независимо от retention'а краткосрочного recorder'а).

Проверка что сенсор rbAmp попадает в LTS:

1. Developer Tools → Statistics.
2. Найдите сущность (например `sensor.rbamp_ui1_mains_energy`).
3. Убедитесь, что в строке есть `statistic_id` и свежий `last_stats_ts`.

LTS используется Energy-дашбордом для исторических view'ов и экспортируется через "Download statistics" HA.

При замене rbAmp-модуля (новое железо, новый entity ID) старая история LTS не мигрируется автоматически. Чтобы сохранить историю — оставьте в новом YAML те же `name:` для каждого сенсора. ESPHome генерирует entity ID из имени ноды + имени сенсора; совпадающие имена дают тот же `statistic_id`.

---

## 8 · Lovelace-карточки

### Energy flow card (встроенная)

Встроенный Energy-дашборд HA уже содержит energy flow card сверху страницы настроек Energy. Он рисуется когда Energy-дашборд сконфигурирован (grid consumption, solar, battery). Дополнительной настройки не нужно.

### Gauge

```yaml
type: gauge
entity: sensor.rbamp_ui1_mains_power
min: 0
max: 5000
name: "Live Power"
unit: W
severity:
  green: 0
  yellow: 2000
  red: 3500
```

### Карточка списка сенсоров

```yaml
type: entities
title: "rbAmp UI1"
entities:
  - entity: sensor.rbamp_ui1_mains_voltage
    name: Voltage
  - entity: sensor.rbamp_ui1_mains_current
    name: Current
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
  - entity: sensor.rbamp_ui1_mains_power_factor
    name: Power Factor
  - entity: sensor.rbamp_ui1_mains_frequency
    name: Frequency
  - entity: sensor.rbamp_ui1_mains_energy
    name: Energy (total)
```

### Mini graph card (HACS)

Устанавливается через HACS (§11):

```yaml
type: custom:mini-graph-card
entities:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
hours_to_show: 6
points_per_hour: 4
line_width: 2
show:
  extrema: true
  average: true
```

### Apex Charts (HACS)

```yaml
type: custom:apexcharts-card
header:
  title: "Power (W)"
  show: true
graph_span: 24h
span:
  end: now
series:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
    type: area
    stroke_width: 2
    fill_raw: last
  - entity: sensor.rbamp_ui1_mains_power
    name: 1h avg
    type: line
    stroke_width: 1
    color: orange
    group_by:
      func: avg
      duration: 1h
```

---

## 9 · HA-автоматизации vs ESPHome on_value

И HA-автоматизации, и callback'и `on_value` в ESPHome реагируют на изменения сенсоров rbAmp. Выбор инструмента:

| Сценарий | HA-автоматизация | ESPHome on_value |
|---|---|---|
| Push-уведомление | Да — доступ ко всем сервисам HA (notify, TTS и т.д.) | Нет — у ESP32 нет notification service |
| Управление реле на той же ESP32 | Возможно (HA → switch), но добавляет латентность | Да — runs on-device, без сетевой зависимости |
| Срабатывание когда HA офлайн | Нет | Да — ESP32 действует автономно |
| Сложные условия (комбинация сущностей, история, календарь) | Да | Ограниченно — Lambda на C++, без доступа к state HA |
| Латентность реакции | ~1–5 с (WiFi + API round-trip) | ~0 мс (тот же event loop) |
| Реакция на MQTT-публикацию | Да (MQTT trigger) | N/A |

**Практический совет**: используйте `on_value` ESPHome для низколатентных локальных действий (переключение реле, LED-индикатор, buzzer), а HA-автоматизации — для всего, что требует HA-сервисов (notifications, scene activation, календарный расписание, комбинации с другими сущностями).

Пример HA-автоматизации — комбинация rbAmp и HA-switch:

```yaml
automation:
  - alias: "Cut dishwasher when peak tariff starts"
    trigger:
      - platform: time
        at: "16:00:00"              # начало пикового тарифа
    condition:
      - condition: numeric_state
        entity_id: sensor.rbamp_ui1_mains_power
        above: 1500                 # отключаем только если нагрузка высокая
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.dishwasher_socket
```

---

## 10 · WSL и Docker

HA часто крутится в Docker (через образ `homeassistant/home-assistant` или Home Assistant OS в VM). ESPHome работает либо как Add-on HA (в том же container environment), либо как отдельный Docker-контейнер.

### mDNS в Docker / WSL

mDNS (multicast UDP 5353) не пересекает Docker network boundaries по умолчанию. Если HA в Docker, а ESP32 на физической LAN:

- **Docker host networking** (`--network host` на Linux) даёт контейнеру доступ к multicast и обычно решает проблему.
- **mDNS reflector**: инструменты вроде `avahi-daemon` с корректным `allow-interfaces` или `mdns-repeater` мостят multicast между Docker bridge (`docker0`) и физическим LAN-интерфейсом.
- **Ручное добавление**: обойдите mDNS полностью — добавьте ESP32 в HA-интеграцию ESPHome вручную по IP.

На Windows с WSL2 у VM WSL2 свой виртуальный сетевой адаптер. Docker-контейнеры внутри могут быть за двумя NAT-слоями (Windows → WSL2 → Docker bridge). mDNS почти наверняка не сработает — добавляйте по IP.

### ESPHome Add-on vs standalone ESPHome

При использовании HA ESPHome Add-on контейнер Add-on'а делит сетевой namespace с HA (предполагая `host` networking, что дефолт для HA OS). mDNS и discovery устройств работают без дополнительной настройки.

При запуске ESPHome как standalone Docker-контейнера убедитесь, что у контейнера есть доступ к физической LAN (`--network host` на Linux) и что `/dev/ttyUSB0` (или ваш COM-порт) проброшен для USB-прошивки:

```sh
docker run -it --rm --network host \
  -v /path/to/configs:/config \
  --device /dev/ttyUSB0:/dev/ttyUSB0 \
  ghcr.io/esphome/esphome compile /config/ui1.yaml
```

### Статический IP для надёжного OTA

В Docker / VLAN окружении статические DHCP-lease'ы для ESP32-устройств с rbAmp настоятельно рекомендуются. Это спасает от:

- Сбоя записанного в HA IP-адреса для entry ESPHome-интеграции.
- Сбоя OTA-прошивки (`esphome upload --device 192.168.x.y`).
- Сбоя любой автоматизации, использующей API по IP.

---

## 11 · HACS (Home Assistant Community Store)

HACS — сторонний integration manager для HA, дающий доступ к community-разработанным карточкам, интеграциям и автоматизациям. Не обязателен для базовой работы rbAmp, но полезен для richer Lovelace-карточек из §8.

Установка — с <https://hacs.xyz/>. Потом установите эти community-карты:

| Карточка | Категория HACS | Применение с rbAmp |
|---|---|---|
| `mini-graph-card` | Frontend | График мощности / тока по времени |
| `apexcharts-card` | Frontend | Multi-series графики |
| `lovelace-card-mod` | Frontend | Кастомный CSS на любой карточке |
| `energy-flow-card-plus` | Frontend | Улучшенная диаграмма потоков энергии |

Установка и управление HACS — в его документации. Карточки документированы в своих репозиториях на GitHub.

---

## 12 · Push-уведомления по порогу

Мобильное приложение HA (`home-assistant.io/integrations/mobile_app`) даёт push-уведомления на iOS и Android. В сочетании с сенсорами rbAmp — мгновенные алерты на over-current, over-power, потерю mains или любой другой порог:

```yaml
automation:
  - alias: "rbAmp over-current alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_current
        above: 14.0           # алерт на 14 А (ниже типового автомата 16 А)
        for:
          seconds: 10
    action:
      - service: notify.mobile_app_your_phone_name
        data:
          title: "Over-current warning"
          message: >
            Current is {{ states('sensor.rbamp_ui1_mains_current') | round(2) }} A
            — approaching 16 A breaker limit.
          data:
            push:
              sound:
                name: default
                critical: 1    # critical-алерт обходит Do Not Disturb (iOS)
              interruption-level: critical

  - alias: "rbAmp mains loss alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_voltage
        below: 10
        for:
          seconds: 5
    action:
      - service: notify.mobile_app_your_phone_name
        data:
          title: "Mains power lost"
          message: "Voltage dropped to {{ states('sensor.rbamp_ui1_mains_voltage') }} V."
```

Для доступности сервиса `notify.mobile_app_*` приложение HA Companion должно быть установлено на целевом устройстве и mobile-app интеграция настроена в HA. Подробности — на <https://companion.home-assistant.io/>.

---

## 13 · Настройка HA ESPHome Add-on

При использовании HA ESPHome Add-on (рекомендуется большинству):

1. Add-on хранит YAML-конфиги в `/config/esphome/` (доступно через файловую систему HA или editor UI add-on'а).
2. `secrets.yaml` живёт рядом с YAML-устройствами в `/config/esphome/`.
3. Путь `external_components` должен быть доступен изнутри контейнера add-on'а. Варианты:
    - Скопируйте директорию `components/rbamp/` в `/config/esphome/components/` и укажите `path: components/` (относительный).
    - После публикации компонента на GitHub используйте удалённую форму: `source: github://rb-amp/rbamp-esphome@main`.
4. OTA: add-on сам выполняет OTA-прошивку — нажмите "Install" на карточке устройства, и add-on попробует OTA на последний известный IP. USB-прошивка тоже доступна через serial-port add-on'а, если ESP32 подключена к хосту HA.

Add-on включает встроенный log-viewer (кнопка "Logs" на карточке устройства) — эквивалент `esphome logs` в CLI.

---

## 14 · Именование сущностей и стабильность entity ID

Entity ID в HA выводятся из имени ноды ESPHome и поля `name:` сенсора:

```
sensor.<node_name>_<sensor_name_lowercased_spaces_to_underscores>
```

Для ноды `rbamp-ui1` и сенсора `name: "Mains Voltage"`:

```
sensor.rbamp_ui1_mains_voltage
```

Entity ID появляются в автоматизациях, template-сенсорах, Lovelace-дашбордах и конфиге Energy-дашборда. Смена `name:` сенсора в YAML создаёт **новый** entity ID и ломает все ссылки на старый. Планируйте имена сенсоров до первого deployment'а.

Чтобы поменять имя сенсора без поломки entity ID:

1. Переименуйте сенсор в YAML.
2. Перепрошейте устройство.
3. В HA: Settings → Devices & Services → ESPHome → ваше устройство → клик по сущности сенсора → клик по gear-иконке → переименуйте entity ID обратно на старое значение (или поменяйте на новое и обновите все ссылки).

Альтернатива — функция HA "Entity ID rename" (Settings → Entities → search → клик по сущности → edit name / entity ID) — позволяет отвязать display name от entity ID. Можно свободно переименовывать display name без влияния на автоматизации.


---

← [DIY-интеграции](07_diy_integrations.md) · [Оглавление](README.md) · [Справочник схемы](09_api_reference.md) →
