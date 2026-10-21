"""rbamp — ESPHome external component for the rbAmp AC energy monitor.

Exposes the `rbamp:` top-level config block and the `sensor.platform: rbamp`
sensor platform for Home Assistant integration via ESPHome's native API.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import i2c
from esphome.const import (
    CONF_ID,
    CONF_ADDRESS,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]
MULTI_CONF = True

# Component version — synced with git tag on the public external-component
# repository (rb-amp/rbamp-esphome). Bump on any user-facing change (schema delta,
# behaviour change, new public method). Tracks the wire-protocol contract version
# at major.minor for wire-level bumps; minor bumps here can also cover
# component-only feature additions (e.g. v1.4 adds `latch_interval:` YAML key
# without any wire-protocol change).
VERSION = "1.9.0"

rbamp_ns = cg.esphome_ns.namespace("rbamp")
RbAmpComponent = rbamp_ns.class_(
    "RbAmpComponent", cg.PollingComponent, i2c.I2CDevice
)

CONF_DRDY_PIN = "drdy_pin"
CONF_CT_MODEL = "ct_model"
CONF_CT_MODELS = "ct_models"
# v1.8.1 service option. A module calibrated with its fitted sensor (device
# sensor class WIRED_CT/BUILTIN_CT) refuses CT model writes from YAML — the
# write would replace its individual calibration with the generic preset.
# `ct_model_force: true` overrides the refusal AND the read-compare skip
# (deliberate preset re-apply). Service use only; remove after the operation.
CONF_CT_MODEL_FORCE = "ct_model_force"
CONF_SENSOR_CLASS = "sensor_class"
CONF_BIDIRECTIONAL = "bidirectional"
CONF_NEW_ADDRESS = "new_address"
CONF_BROADCAST_LATCH = "broadcast_latch"
CONF_TOPOLOGY = "topology"
# v1.3 fleet / multi-module keys — opt-in. `fleet_gc_enable: true` writes
# REG_FLEET_CONFIG.bit0 (capability-gated; needs CAP_GC_LATCH on the device,
# which means v1.3+ firmware). `group_id:` filters GC frames device-side —
# 0 (default) = respond to all-call broadcasts; non-zero = respond only to
# frames whose group byte matches. See README.md "Multi-module" section.
CONF_FLEET_GC_ENABLE = "fleet_gc_enable"
CONF_GROUP_ID = "group_id"

# v1.4 — decouple LATCH cadence from RT-read cadence. When set, the ESPHome
# component still reads the RT block (V/I/P/PF/Q) every `update_interval` but
# only sends CMD_LATCH_PERIOD (and integrates the Wh accumulator on the
# returned period average) every `latch_interval`. Wh remains monotonic and
# accurate — master-side integration uses actual wall-clock dt between
# LATCH events. Bus load falls proportional to `latch_interval / update_interval`.
#
# When unset (v1.3.0 default), latch cadence equals update_interval — the
# original behaviour, backwards-compatible with existing YAML.
#
# Forward-compat note: STANDARD-tier firmware exposes 8 independent period
# accumulators (windowed via REG_PERIOD_SEL/COMMIT). A future release may
# accept `latch_intervals:` as a list of `{period_id, interval}` entries;
# scalar form here normalises to period_id=0 today and will continue to
# work as the primary period on STANDARD firmware.
CONF_LATCH_INTERVAL = "latch_interval"

# v1.8 — starting current for energy accounting (16-bit-map devices with
# µ-resolution energy counters). While a channel's current is below this
# value the device counts no energy on that channel (at no load the measured
# power is noise of either sign and would otherwise creep into both the
# import and export counters); instantaneous readings are not affected.
# Value in µA: 0 = off, else 10..2540 in 10 µA steps (device register
# granularity). Device default is 400 µA. Range is enforced here so an
# invalid value never reaches the bus. On devices without the capability the
# key is ignored with a warning at boot.
CONF_START_CURRENT_UA = "start_current_ua"


def _validate_start_current_ua(value):
    value = cv.positive_int(value)
    if value == 0:
        return value
    if not 10 <= value <= 2540 or value % 10 != 0:
        raise cv.Invalid(
            f"`start_current_ua` must be 0 (off) or 10..2540 in steps of 10 µA "
            f"(got {value})."
        )
    return value

# CT model identifiers — v1.6: generated from libs/spec/sensor_models.yaml
# (single source of truth for code→meaning across firmware + all libraries).
# Hardcoded model lists were retired 2026-09-04 after the six-model drift
# incident. See registry.py for parser + fallback logic.
#
# Symbol convention (kept backwards-compat with pre-v1.6 YAMLs):
#   * SCT_013 clamps: SCT_013_<3-digit-A>  e.g. SCT_013_005, SCT_013_030
#   * WIRED_CT slots: WIRED_CT_<code>      e.g. WIRED_CT_1 (legacy), WIRED_CT_6 (1A clamp)
#
# IMPORTANT — writing `ct_model:` / `ct_models:` on v1.4.18+ firmware applies
# the preset (GAIN + NF from the factory table) immediately on the target
# channel. On older firmware the byte is metadata-only; per-unit calibration
# via the manufacturer's tooling remains required for accurate readings.
from .registry import (
    CT_MODELS,
    CT_MODEL_ALLOWED_PER_CLASS,
    CT_MODEL_STATUS,
    CT_MODEL_DESCRIPTORS,
    REGISTRY_SOURCE,
)

# Sensor classes — v1.2+ firmware. Pinned via `sensor_class:` YAML key (writes
# REG_SENSOR_CLASS + CMD_SAVE_GAINS). On v1.2+ firmware this is a PRECONDITION
# for `ct_model:` / `ct_models:` writes — calling either while the class is
# still UNSET yields a runtime warning and skips the write. On v1.0 / v1.1
# firmware the key is metadata-only (same status as `ct_model:`).
RBAMP_SENSOR_CLASSES = {
    "UNSET":      0,
    "SCT_013":    1,
    "WIRED_CT":   2,
    "BUILTIN_CT": 3,
}

# CT_MODEL_ALLOWED_PER_CLASS is now provided by registry.py (imported above).
# The set is derived from sensor_models.yaml: `characterized` + `legacy`
# rows are on-firmware and accepted; `reserved` rows return ERR_PARAM and
# are rejected at YAML-parse time so the user sees the constraint before
# flash + boot.
CT_MODEL_ALLOWED_PER_CLASS.setdefault("UNSET", set())

# Topology hint. v1 firmware exposes no REG_TOPOLOGY byte (planned for v1.1) and
# never NACKs unmapped reads, so the master cannot probe channel count reliably
# in-band. This key lets the user state it explicitly. The component ALSO derives
# n_channels_ from how many `current[_1/_2]` sensor slots the YAML declares —
# so for the typical single-phase deployment the default is fine and this key is
# purely cosmetic (drives the dump_config log line). Use it when running the
# component against a future split/three-phase SKU before v1.1 firmware ships.
TOPOLOGIES = {
    "SINGLE": 0,
    "SPLIT_PHASE": 1,
    "THREE_PHASE": 2,
}

def _validate_new_address(config):
    if CONF_NEW_ADDRESS in config and config[CONF_NEW_ADDRESS] == config[CONF_ADDRESS]:
        raise cv.Invalid(
            f"`new_address` (0x{config[CONF_NEW_ADDRESS]:02X}) must differ from "
            f"the current `address` (0x{config[CONF_ADDRESS]:02X}). "
            "Remove `new_address` after the change has been applied."
        )
    return config


def _validate_ct_model_mutex(config):
    """`ct_model:` (global) and `ct_models:` (per-channel array) are
    mutually exclusive — picking either is fine, both is a config error."""
    if CONF_CT_MODEL in config and CONF_CT_MODELS in config:
        raise cv.Invalid(
            "`ct_model:` (single global CT model) and `ct_models:` (per-channel "
            "CT model array) cannot both be set on the same `rbamp:` instance. "
            "Use `ct_model:` for uniform multi-channel deployments, or "
            "`ct_models:` for mixed-CT installations (UI3 with different clamp "
            "sizes per channel)."
        )
    return config


def _validate_ct_model_force(config):
    """`ct_model_force:` only has an effect on a CT model write — require one
    so a stale force flag can't sit silently in YAML waiting for a later
    `ct_model:` addition to do a destructive preset re-apply."""
    if config.get(CONF_CT_MODEL_FORCE) and (
        CONF_CT_MODEL not in config and CONF_CT_MODELS not in config
    ):
        raise cv.Invalid(
            "`ct_model_force: true` has no effect without `ct_model:` or "
            "`ct_models:`. It is a service option: set it together with the "
            "model to re-apply, run once, then remove both from YAML."
        )
    return config


def _validate_ct_model_per_class(config):
    """Reject CT codes outside the shipping-firmware allowed set for the pinned
    sensor_class. Enforcement is by-registry (registry.py loaded from
    libs/spec/sensor_models.yaml at import time):

      * ``characterized`` + ``legacy`` codes are on shipping firmware — accepted.
      * ``reserved`` codes have no preset row → firmware returns ERR_PARAM 0xFE
        and the channel keeps its previous model. Rejected here with an
        explicit "reserved" hint so the user doesn't see a generic cv.enum
        "unknown value" error.
      * Class mismatch (e.g. sensor_class: SCT_013 with a WIRED_CT_<n> code)
        rejected with the "allowed for <class>" list.

    ``legacy`` codes pass but emit a codegen-time warning (see
    ``_check_code`` below) — they shipped historically and are still valid,
    but were not re-verified on the current op-amp revision.
    """
    import logging
    _log = logging.getLogger("rbamp")

    sensor_class = config.get(CONF_SENSOR_CLASS, "UNSET")
    allowed_codes = CT_MODEL_ALLOWED_PER_CLASS.get(sensor_class, set())

    def _check_code(code, location):
        # `code` is the cv.enum KEY string (e.g. "SCT_013_030"); map to the
        # firmware int via CT_MODELS before the int-set membership test.
        code_int = CT_MODELS[code]
        status = CT_MODEL_STATUS.get((sensor_class, code_int), "reserved")

        if code_int not in allowed_codes:
            # Filter hint by class prefix: SCT_013_* symbols belong to SCT_013,
            # WIRED_CT_* to WIRED_CT. Symbol codes collide across classes
            # (SCT_013_005 = 1, WIRED_CT_1 = 1) so a plain "code in allowed_codes"
            # lookup would list the wrong class's symbols too.
            symbol_prefix = f"{sensor_class}_"
            allowed_names = sorted(
                name for name, c in CT_MODELS.items()
                if name.startswith(symbol_prefix)
                and c in allowed_codes
                and CT_MODEL_STATUS.get((sensor_class, c)) in ("characterized", "legacy")
            )
            hint = ", ".join(allowed_names) if allowed_names else (
                "(none — this class has no CT model selection)"
            )
            if status == "reserved":
                raise cv.Invalid(
                    f"CT model `{code}` is RESERVED — the code is allocated in "
                    f"the model registry but no preset row exists on shipping "
                    f"firmware, so a write returns ERR_PARAM 0xFE. Location: "
                    f"`{location}` under sensor_class `{sensor_class}`. "
                    f"Allowed for `{sensor_class}`: {hint}."
                )
            raise cv.Invalid(
                f"CT model `{code}` is not valid for sensor_class "
                f"`{sensor_class}` ({location}). Firmware will reject the "
                f"write with ERR_PARAM 0xFE. Allowed for `{sensor_class}`: "
                f"{hint}."
            )

        if status == "legacy":
            _log.warning(
                "rbamp: `%s` (%s) is a LEGACY model — shipped historically "
                "but NOT re-verified on the current op-amp revision. Measured "
                "G may diverge from the published table; per-unit calibration "
                "recommended. See libs/spec/sensor_models.yaml for the note.",
                code, location,
            )

    if CONF_CT_MODEL in config:
        _check_code(config[CONF_CT_MODEL], "ct_model")
    if CONF_CT_MODELS in config:
        for idx, code in enumerate(config[CONF_CT_MODELS]):
            _check_code(code, f"ct_models[{idx}]")

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RbAmpComponent),
            cv.Optional(CONF_DRDY_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_CT_MODEL): cv.enum(CT_MODELS, upper=True),
            cv.Optional(CONF_CT_MODELS): cv.All(
                cv.ensure_list(cv.enum(CT_MODELS, upper=True)),
                # v1.6: 16-channel reserve (window selector nibble is 4 bits).
                # Runtime code cross-checks against n_channels_ detected from
                # REG_TOPOLOGY and logs a mismatch warning rather than failing
                # to boot — a UI7 YAML with 7 slots must still validate on a
                # UI1 board where only ch0 will actually be written.
                cv.Length(min=1, max=16),
            ),
            cv.Optional(CONF_SENSOR_CLASS, default="SCT_013"): cv.enum(
                RBAMP_SENSOR_CLASSES, upper=True
            ),
            cv.Optional(CONF_CT_MODEL_FORCE, default=False): cv.boolean,
            cv.Optional(CONF_BIDIRECTIONAL, default=False): cv.boolean,
            cv.Optional(CONF_NEW_ADDRESS): cv.i2c_address,
            cv.Optional(CONF_BROADCAST_LATCH, default=False): cv.boolean,
            cv.Optional(CONF_TOPOLOGY, default="SINGLE"): cv.enum(
                TOPOLOGIES, upper=True
            ),
            cv.Optional(CONF_FLEET_GC_ENABLE): cv.boolean,
            cv.Optional(CONF_GROUP_ID): cv.All(cv.uint8_t, cv.Range(min=0, max=255)),
            cv.Optional(CONF_LATCH_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_START_CURRENT_UA): _validate_start_current_ua,
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x50)),
    _validate_new_address,
    _validate_ct_model_mutex,
    _validate_ct_model_force,
    _validate_ct_model_per_class,
)


def _clamp_update_interval(config):
    """Reject `update_interval` < 1s (docs #5). The latch-rotation cadence
    needs at least one full RT window (~100 ms on current firmware; ~200 ms
    on pre-v1.2 images — the 1s floor is sized to the older bound, which is
    a safe upper bound everywhere) plus settle time per cycle;
    sub-second update_interval starves the LATCH→snapshot→read pipeline
    and freezes energy integration without diagnostics."""
    interval = config.get("update_interval")
    if interval is not None and interval.total_milliseconds < 1000:
        raise cv.Invalid(
            f"`update_interval` must be at least 1s "
            f"(got {interval.total_milliseconds} ms). The latch-rotation "
            "state machine needs the full RT cycle to complete; shorter "
            "intervals starve LATCH→snapshot→read and freeze energy "
            "integration without surfacing a diagnostic. Use 60s default "
            "for standard deployments, 1-5s for low-latency RT scenarios."
        )
    return config


def _validate_latch_interval(config):
    """`latch_interval:` (v1.4) must be >= `update_interval`. Setting it
    shorter would mean LATCH cycles faster than we read the RT block —
    the extra LATCHes get no observable data, just waste bus time.
    """
    latch = config.get(CONF_LATCH_INTERVAL)
    update = config.get("update_interval")
    if latch is not None and update is not None:
        if latch.total_milliseconds < update.total_milliseconds:
            raise cv.Invalid(
                f"`latch_interval` ({latch.total_milliseconds} ms) must be "
                f">= `update_interval` ({update.total_milliseconds} ms). "
                "Setting a shorter latch cadence than the RT-read cadence "
                "makes no sense — the extra LATCHes would produce no data "
                "the component reads before the next LATCH overwrites them. "
                "Either drop `latch_interval` (defaults to update_interval) "
                "or set it to a value >= update_interval to slow the LATCH "
                "cycle down for period accumulation while keeping RT fast."
            )
    return config


# Apply validators on top of the per-class validator chain.
CONFIG_SCHEMA = cv.All(
    CONFIG_SCHEMA,
    _clamp_update_interval,
    _validate_latch_interval,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_DRDY_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_DRDY_PIN])
        cg.add(var.set_drdy_pin(pin))

    # Sensor class is always written (default SCT_013) — v1.2+ firmware
    # requires it before any CT-model write. On older firmware it's a no-op.
    cg.add(var.set_sensor_class(config[CONF_SENSOR_CLASS]))

    if CONF_CT_MODEL in config:
        cg.add(var.set_ct_model(config[CONF_CT_MODEL]))

    if config[CONF_CT_MODEL_FORCE]:
        cg.add(var.set_ct_model_force(True))

    if CONF_CT_MODELS in config:
        # Per-channel array. Schema validates length 1..3; codegen registers
        # each non-empty slot. The C++ apply path orders writes correctly
        # (highest channel first) — see apply_ct_models_per_channel_().
        for ch, code in enumerate(config[CONF_CT_MODELS]):
            cg.add(var.set_ct_model_ch(ch, code))

    cg.add(var.set_bidirectional(config[CONF_BIDIRECTIONAL]))
    cg.add(var.set_broadcast_latch(config[CONF_BROADCAST_LATCH]))
    cg.add(var.set_topology(config[CONF_TOPOLOGY]))

    if CONF_NEW_ADDRESS in config:
        cg.add(var.set_address_change_request(config[CONF_NEW_ADDRESS]))

    # v1.3 fleet keys (capability-gated at runtime via CAP_GC_LATCH)
    if CONF_FLEET_GC_ENABLE in config:
        cg.add(var.set_fleet_gc_enable(config[CONF_FLEET_GC_ENABLE]))
    if CONF_GROUP_ID in config:
        cg.add(var.set_group_id(config[CONF_GROUP_ID]))

    # v1.4 latch-cadence decoupling. When unset, C++ side keeps the field at
    # 0 which the update() gate reads as "use update_interval" (backwards
    # compatible with v1.3 single-cadence behaviour).
    if CONF_LATCH_INTERVAL in config:
        cg.add(var.set_latch_interval_ms(
            int(config[CONF_LATCH_INTERVAL].total_milliseconds)
        ))

    # v1.8 start current (µA, pre-validated; capability-gated at runtime)
    if CONF_START_CURRENT_UA in config:
        cg.add(var.set_start_current_ua(config[CONF_START_CURRENT_UA]))
