# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.6.0] — 2026-09-04

Per-channel `ct_models:` for all channels (0..15), sensor-model registry, and
persist fix. Firmware: v1.4.18+ required for senior channels 3+.

### Added
- **Per-channel CT models on all channels.** `ct_models:` in YAML now accepts up
  to 16 entries. Module variant detection (`REG_TOPOLOGY`) determines how many
  are applied; extra entries produce a warning, not a failure.
- **Sensor-model registry** — the model list is generated from
  `libs/spec/sensor_models.yaml`. The two hardcoded lists (`CT_MODELS` dict +
  `CT_MODEL_ALLOWED_PER_CLASS`) are replaced by a single registry lookup. Adding
  a new CT SKU is now a registry edit — no library release needed.

### Fixed
- **v1.6.0 adds SAVE after the apply cycle.** Prior versions used a
  factory-gated persist path for CT model and sensor class — on a production
  module the binding applied to RAM but was lost on reboot. v1.6.0 persists via
  `CMD_SAVE_USER_CONFIG` (ungated). **Users of prior versions in production
  silently lost the CT model binding on every reboot — update before shipping
  devices.**

