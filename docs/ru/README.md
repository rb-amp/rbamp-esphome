# Компонент rbAmp для ESPHome — документация

Декларативная интеграция rbAmp для ESPHome. Подключите I²C-модуль к двум GPIO ESP32,
опишите его в YAML — и Home Assistant увидит готовые сущности для напряжения, тока,
мощности, энергии, частоты и коэффициента мощности.

| # | Документ | Содержание |
|---|---|---|
| 01 | [Обзор](01_overview.md) | Что такое rbAmp, что делает компонент, поток данных, дизайн NVS, дисциплина NACK |
| 02 | [Тиры модулей](02_tiers.md) | Линейки BASIC / STANDARD / PRO и YAML-ключи, доступные каждой |
| 03 | [Выбор датчика тока](03_sensor_selection.md) | Выбор CT-клипсы; поведение ключа `ct_model:` |
| 04 | [Подключение](04_hardware.md) | Распиновка GPIO, pull-up'ы, многомодульная шина, bench-сетап |
| 05 | [Быстрый старт](05_quickstart.md) | Первая прошивка за пять минут, разбор boot-лога, discovery в HA |
| 06 | [Примеры](06_examples.md) | YAML-кулинарная книга: UI1 / UI3 / multi-module / провизия адреса + автоматизации |
| 07 | [DIY-интеграции](07_diy_integrations.md) | За пределами HA: MQTT, InfluxDB / Grafana, Lambda-действия |
| 08 | [Home Assistant](08_has_integrations.md) | Native API, Energy-дашборд, статистика, Lovelace |
| 09 | [Справочник YAML-схемы](09_api_reference.md) | Каждый ключ, тип, дефолт, побочный эффект |
| 10 | [Диагностика](10_troubleshooting.md) | Симптом-ориентированная отладка: NACK, OTA, NVS, тулчейн |
| 11 | [Changelog](11_changelog.md) | История версий, гайд по апгрейду |

Готовые рабочие конфиги — в [`../../example/`](../../example/). Спецификация
wire-протокола rbAmp (регистры, команды, ошибки, дисциплина NACK) опубликована на
<https://rbamp.com/docs/modules-basic-standard-api-reference>.

> English version of these docs is in [`../`](../).
