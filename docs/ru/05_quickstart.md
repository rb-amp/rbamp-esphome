# 05 · Quickstart — от нуля до Home Assistant

Эта глава за ~5 минут активной работы (плюс время компиляции и прошивки) проведёт вас от чистого ESP32 и модуля rbAmp до живых сенсоров в Home Assistant.

## Что понадобится

### Железо

- **Плата ESP32** — подойдёт любая с двумя свободными I²C-пинами. В примерах используются `GPIO21` (SDA) и `GPIO22` (SCL) — стандартные значения для DevKitC. Если у вас другая плата — поправьте номера пинов в YAML.
- **Модуль rbAmp** — любой UI* или I* SKU. Для первого пуска удобнее всего UI1: одно напряжение + один канал тока, минимум проводов.
- **Питание 5 В** для rbAmp — модуль работает на 4,5..5,5 В. USB-питание с ESP32 подходит, если плата выводит линию 5V.
- **Разводка**: VCC → +5 В, GND → GND, SDA → GPIO21, SCL → GPIO22. Внешние pull-up резисторы для одного модуля не нужны (встроенные 4,7 кОм уже на плате). Подробная схема — в [04_hardware.md](04_hardware.md).
- **USB-кабель** от ESP32 к компьютеру для прошивки.

### Софт

| Требование | Заметка |
|---|---|
| **ESPHome ≥ 2024.6** | Компонент использует современный синтаксис `ota:` (список платформ), доступный с 2024.6. Смена I²C-адреса использует `i2c::I2CDevice::set_i2c_address()` (доступен с 2023.6). Проверено вплоть до ESPHome 2026.5 и новее. |
| **Python 3.10..3.13** для CLI | Внутренний PlatformIO-слой ESPHome (`penv`) не принимает Python 3.14. Если используете HA Add-on — этот пункт неактуален. |
| **Home Assistant** | Любая свежая версия. Auto-discovery работает через mDNS (Avahi / Bonjour). |

Два пути компиляции:

1. **HA ESPHome Add-on** (рекомендуется большинству) — положите YAML в config-директорию add-on'а и нажмите Compile. Локальное Python-окружение не нужно.
2. **Локальный CLI venv** — для разработки и CI. Установка: `pip install esphome` в Python 3.11–3.13 venv.

### Настройка venv (только для CLI-пути)

```sh
# Windows PowerShell — путь к Python подстройте под свой 3.11 / 3.13
& "C:\Python311\python.exe" -m venv .venv-esphome
.\.venv-esphome\Scripts\Activate.ps1
pip install esphome
esphome version          # должно вывести 2024.6.x или новее
```

На Linux / macOS:

```sh
python3.11 -m venv .venv-esphome
source .venv-esphome/bin/activate
pip install esphome
esphome version
```

## Шаг 1 — Найдите компонент

Есть две формы для блока `external_components` — выберите по окружению.

### Форма A — github (по умолчанию, работает в HA Add-on)

```yaml
external_components:
  - source: github://rb-amp/rbamp-esphome@main
    components: [rbamp]
    refresh: 1d   # 1 day для production. На время активной разработки
                  # на @main поменяйте на `0s`, иначе ESPHome будет
                  # кэшировать копию и игнорировать новые коммиты.
```

Эта форма — единственная, которая работает в HA ESPHome Add-on (у Add-on
нет shell-доступа к sibling-директориям). Она же удобна для CLI: ничего
клонировать локально не нужно.

### Форма B — локальный путь (для разработки в монорепо rbAmp)

Если вы клонировали репозиторий и работаете напрямую с исходниками
компонента, укажите ESPHome локальный путь:

```yaml
external_components:
  - source:
      type: local
      path: /absolute/path/to/rbamp-repo/tools/esphome-rbamp/components
    components: [rbamp]
```

В `example/ui1.yaml` блок уже настроен на относительный путь (`../components`),
который работает при запуске `esphome compile` из директории `example/`:

```sh
cd tools/esphome-rbamp/example
esphome compile ui1.yaml
```

> ⚠ **Локальный путь не работает в HA Add-on** — Add-on держит YAML в
> `/config/esphome/` без shell-доступа к остальной файловой системе.
> Add-on пользователи выбирают форму A.

## Шаг 2 — Создайте YAML

Скопируйте `example/ui1.yaml` как стартовую точку. Откройте в любом редакторе и впишите WiFi-креды и имя устройства. Файл целиком приведён ниже с построчными комментариями:

```yaml
esphome:
  name: rbamp-ui1          # имя ноды — определяет mDNS-хостнейм (rbamp-ui1.local)

esp32:
  board: esp32dev           # ваша плата (например nodemcu-32s, lolin_d32)
  framework:
    type: arduino           # arduino или esp-idf — оба работают с компонентом

logger:
  level: DEBUG              # DEBUG показывает I²C-чтения; после первого запуска можно INFO

wifi:
  ssid: !secret wifi_ssid   # см. secrets.yaml ниже
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"    # резервная AP при провале подключения
    password: !secret ap_password

captive_portal:             # config-страница на резервной AP

api:                        # Home Assistant native API на порту 6053
  # encryption:
  #   key: "BASE64_KEY"     # опционально — см. 08_has_integrations.md

ota:
  - platform: esphome       # обновления по воздуху

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz          # ВАЖНО: 100 кГц вызывает периодические NACK от модуля;
                            # 50 кГц снижает их ~в 5-10 раз (см. SPEC §B.5)
  scan: true                # логирует все найденные I²C-адреса при загрузке

external_components:
  - source:
      type: local
      path: ../components   # относительно этого YAML-файла
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50             # заводской адрес по умолчанию
  update_interval: 60s      # как часто закрывать период и публиковать сенсоры
  ct_model: SCT_013_030     # модель CT-клипсы — поменяйте под вашу
                            # (SCT_013_005 / _010 / _030 / _050 / _100)

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"  # entity ID в HA: sensor.rbamp_ui1_mains_voltage
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"   # state_class: total_increasing — готов к Energy-дашборду
    frequency:
      name: "Mains Frequency"
    power_factor:
      name: "Mains Power Factor"
```

> Ключ `ct_model:` подгружает заводские коэффициенты под выбранную модель клипсы. Если у вас не SCT-013-030 — поменяйте на нужную модель. Подробнее о выборе — в [03_sensor_selection.md](03_sensor_selection.md).

### secrets.yaml

Создайте `secrets.yaml` в той же директории что и YAML (он `.gitignore`-нут по умолчанию в `example/.gitignore`, чтобы креды не утекли в коммит):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

## Шаг 3 — Компиляция

```sh
# из директории example/ при активном venv
esphome compile ui1.yaml
```

Что произойдёт:

1. ESPHome развернёт `external_components`, загрузит Python-пакет `rbamp` из `../components`.
2. PlatformIO скачает ESP32-платформу при первом запуске (~1–2 ГБ, кешируется). Это самый медленный шаг — на слабом соединении до 15 минут.
3. C++ компонент компилируется и линкуется. Успешный финал — строка вроде:

```
SUCCESS Took 58.71 seconds
Wrote .esphome/build/rbamp-ui1/.pioenvs/rbamp-ui1/firmware.factory.bin
```

Если сборка валится с `Error: Failed to install Python dependencies` — скорее всего host Python 3.14. Используйте HA Add-on или venv с 3.11 / 3.13 (см. "Что понадобится"). Это несовместимость `uv`-резолвера PlatformIO с некоторыми моделями CPU, не баг компонента.

## Шаг 4 — Первая прошивка

`esphome upload` зальёт бинарник по USB. Сначала найдите нужный порт:

- **Windows**: Device Manager → Ports (COM & LPT). Обычно `COM3`–`COM9`.
- **Linux / macOS**: `ls /dev/tty*` — обычно `/dev/ttyUSB0` или `/dev/ttyACM0`.

```sh
esphome upload --device COM6 ui1.yaml             # Windows
esphome upload --device /dev/ttyUSB0 ui1.yaml     # Linux
```

Для пути через HA Add-on:

1. Откройте Home Assistant → Settings → Add-ons → ESPHome.
2. Откройте веб-UI ESPHome Add-on.
3. Найдите ваше устройство (или нажмите "New Device") → Install → Plug in to this computer.

Инструмент прошивки (`esptool.py` внутри) перезагружает ESP32 автоматически после записи. Полное время прошивки обычно 30–60 секунд.

## Шаг 5 — Что ожидать в boot-логе

Подцепите `esphome logs` чтобы наблюдать загрузку в реальном времени:

```sh
esphome logs ui1.yaml --device COM6
```

Здоровая первая загрузка даёт примерно такие строки (значения и тайминги приблизительны):

```
[D][i2c:136]: Found i2c device at address 0x50
[C][rbamp:442]: Setting up rbAmp at 0x50 ...
[C][rbamp:448]:   Firmware version: 0x01
[C][rbamp:598]: rbAmp:
[C][rbamp:599]:   Address: 0x50
[C][rbamp:600]:   Firmware version: 0x01
[C][rbamp:607]:   Topology: SINGLE, channels: 1, voltage: yes
[C][rbamp:609]:   Bidirectional: NO
[C][rbamp:610]:   Broadcast LATCH: NO
[C][rbamp:611]:   Wh persistence: NVS every 300s
[C][component:246]: Setup rbamp took 67ms
```

Строка `i2c: scan: true` появляется до строк компонента и подтверждает что модуль физически виден на шине. Если `Found i2c device at address 0x50` отсутствует — см. раздел "Что делать если" ниже.

В пределах одного `update_interval` (по умолчанию 60 с) появятся публикации сенсоров:

```
[D][sensor:094]: 'Mains Voltage': Sending state 226.70 V
[D][sensor:094]: 'Mains Current': Sending state 0.755 A
[D][sensor:094]: 'Mains Power': Sending state 92.80 W
[D][sensor:094]: 'Mains Power Factor': Sending state 0.542
[D][sensor:094]: 'Mains Frequency': Sending state 50.0 Hz
[D][sensor:094]: 'Mains Energy': Sending state 0.026 Wh
```

Эталонные значения на bench с UI1 SKU и индуктивной нагрузкой: U ≈ 225 В, I ≈ 0,78 А, P ≈ 110 Вт, PF ≈ 0,62. Конкретные значения зависят от подключённой нагрузки.

## Шаг 6 — Discovery в Home Assistant

### mDNS (автоматически)

HA сам находит ESPHome-ноды с настроенным блоком `api:` в том же локальном сетевом сегменте. В течение 30–60 секунд после первой загрузки ESP32:

1. HA показывает уведомление: **New devices discovered**.
2. Settings → Devices & Services → ESPHome (или клик по уведомлению).
3. Выберите новую ноду (например `rbamp-ui1`).
4. Введите локальный IP устройства (в логах виден как `[I][wifi:189]: IP: 192.168.0.xxx`), если HA не нашёл его сам.
5. Add.

Все сенсорные сущности регистрируются мгновенно. Entity ID следует паттерну `sensor.<node_name>_<sensor_name>` (пробелы заменены на подчёркивания, всё в нижнем регистре). Для YAML выше: `sensor.rbamp_ui1_mains_voltage`, `sensor.rbamp_ui1_mains_energy` и т.д.

### Ручное добавление (если mDNS блокируется)

Settings → Devices & Services → Add Integration → ESPHome → IP-адрес и API-порт (6053). Если в `api: encryption: key:` стоит ключ — введите при запросе.

### Energy-дашборд

Сенсор `energy` уже сконфигурирован под HA Energy:

- `device_class: energy`
- `state_class: total_increasing`
- `unit_of_measurement: Wh`

Settings → Dashboards → Energy → Add Consumption → выберите "Mains Energy". HA сразу начинает учитывать накопительное потребление.

## Что делать если

| Симптом | Вероятная причина | Что делать |
|---|---|---|
| `Found i2c device at address 0x50` отсутствует | Модуль не запитан или неправильно подключён, не те GPIO-пины | Проверьте VCC (должно быть 4,5–5,0 В), GND, SDA / SCL; убедитесь что pull-up перемычки на модуле целы |
| `Probe failed at 0x50` в логах | Модуль виден в скане, но не отвечает на чтения регистров | Проверьте `frequency: 50kHz` в YAML; убедитесь что модуль не в середине flash-write (~700 мс) |
| Сенсоры показывают 0 или NaN | Модуль ещё грузится; первые ~250 мс после питания регистры читают 0 | Дождитесь первого цикла `update_interval`; компонент пропускает чтение пока статус-регистр не готов |
| HA не находит устройство | mDNS блокируется роутером / VLAN; блок `api:` отсутствует | Добавьте `api:`; попробуйте ручное добавление по IP |
| Warning `update() took 406ms` | Нормально — модуль не имеет I²C auto-increment, 36 однобайтных транзакций за цикл на 50 кГц | Не проблема; 30 мс budget ESPHome — рекомендация, не строгое ограничение |
| Fallback в AP-режим | Неверные SSID / пароль; AP вне зоны действия | Проверьте `secrets.yaml`; ближе к AP; 2,4 ГГц vs 5 ГГц |

Для проблем не из этого списка — [10_troubleshooting.md](10_troubleshooting.md).

## Что дальше

- **Трёхканальная установка**: [06_examples.md](06_examples.md) — UI3 YAML с тремя независимыми каналами тока и всеми производными величинами.
- **Несколько модулей на одной шине**: [06_examples.md](06_examples.md) — раздел про multi-module.
- **Глубокое погружение в HA Energy**: [08_has_integrations.md](08_has_integrations.md).
- **DIY-интеграции** (Grafana, MQTT, автоматизации): [07_diy_integrations.md](07_diy_integrations.md).
- **Полный референс конфигурации**: [09_api_reference.md](09_api_reference.md).
- **Диагностика проблем** при первой работе с rbAmp: [10_troubleshooting.md](10_troubleshooting.md).



---

← [Подключение](04_hardware.md) · [Оглавление](README.md) · [Примеры](06_examples.md) →
