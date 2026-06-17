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

# Component version — synced with git tag `v1.3.0` on the public external-component
# repository (rb-amp/rbamp-esphome). Bump on any user-facing change (schema delta,
# behaviour change, new public method). Tracks the wire-protocol contract version
# at major.minor; the patch position covers component-only bug fixes.
VERSION = "1.3.0"

rbamp_ns = cg.esphome_ns.namespace("rbamp")
RbAmpComponent = rbamp_ns.class_(
    "RbAmpComponent", cg.PollingComponent, i2c.I2CDevice
)

CONF_DRDY_PIN = "drdy_pin"
CONF_CT_MODEL = "ct_model"
CONF_CT_MODELS = "ct_models"
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

# CT model identifiers — match REG_CT_MODEL values supported by the device
# (0=unset, 1=-005, 2=-010, 3=-030, 4=-050, 5=-100).
#
# IMPORTANT — writing `ct_model:` alone does NOT calibrate the device. It only
# stores a metadata byte. The actual noise-floor and gain calibration tables
# must be tuned per-device against the user's CT clamp + burden network using
# the manufacturer's calibration tooling and reference meter (consult the
# device documentation for the procedure). Upcoming firmware revisions will
# add an auto-populate-on-CT_MODEL-write path; until then this key is
# informational.
# CT model codes — full v1.3 space (REG_CT_MODEL accepts 0..7):
#   0 = unset
#   1 = SCT_013-5A    (5 A primary)
#   2 = SCT_013-10A   (10 A primary)
#   3 = SCT_013-30A   (30 A primary)
#   4 = SCT_013-50A   (50 A primary)
#   5 = SCT_013-100A  (100 A primary; RESERVED — see CT_MODEL_ALLOWED_PER_CLASS)
#   6 = SCT_013-20A   (20 A primary; v1.3 new)
#   7 = SCT_013-60A   (60 A primary; RESERVED — see CT_MODEL_ALLOWED_PER_CLASS)
#
# For WIRED_CT class, codes 1..3 select factory-calibrated wired-CT presets
# (PCB-mounted current transformer with known turns ratio); codes 4..7 are
# reserved. For BUILTIN_CT class (fixed integrated CT, e.g. shunt-based
# variants), NO ct_model selection is valid — the device's onboard sensor is
# the only option.
CT_MODELS = {
    "SCT_013_005": 1,
    "SCT_013_010": 2,
    "SCT_013_020": 6,   # v1.3 new
    "SCT_013_030": 3,
    "SCT_013_050": 4,
    "WIRED_CT_1":  1,   # factory-preset wired-CT slot 1
    "WIRED_CT_2":  2,
    "WIRED_CT_3":  3,
}

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

# Per-class CT model validation — root SEED 2026-06-16 gotcha #3:
# valid CT-code sets are NON-CONTIGUOUS. Firmware is the ultimate authority
# (it returns DEV_ERR_PARAM at runtime for out-of-set codes); this schema-side
# validator surfaces the constraint at `esphome config` time so users don't
# have to flash + boot + read logs to discover a typo.
CT_MODEL_ALLOWED_PER_CLASS = {
    "UNSET":      set(),           # no class chosen → no CT model allowed
    "SCT_013":    {1, 2, 3, 4, 6}, # -005, -010, -030, -050, -020 (codes 5,7 reserved)
    "WIRED_CT":   {1, 2, 3},       # factory preset slots
    "BUILTIN_CT": set(),           # device's onboard CT only — no preset selection
}

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


def _validate_ct_model_per_class(config):
    """Reject CT codes outside the firmware's per-class allowed set (root SEED
    2026-06-16 gotcha #3). Firmware authority: the codes are non-contiguous
    inside the 0..7 enum space, so validation can't be expressed as a simple
    range; it must be a set membership check against CT_MODEL_ALLOWED_PER_CLASS.
    Catches the common mistakes:
      * `sensor_class: SCT_013` + `ct_model: SCT_013_100` (code 5 reserved)
      * `sensor_class: BUILTIN_CT` + ANY ct_model (BUILTIN_CT has no preset
        selection — the onboard CT is fixed)
      * `sensor_class: WIRED_CT` + `ct_model: SCT_013_050` (code 4 not in
        WIRED_CT's {1,2,3} preset slots)
    """
    sensor_class = config.get(CONF_SENSOR_CLASS, "UNSET")
    allowed_codes = CT_MODEL_ALLOWED_PER_CLASS.get(sensor_class, set())

    def _check_code(code, location):
        # `code` is the cv.enum KEY string (e.g. "SCT_013_030"); map to the
        # firmware int via CT_MODELS before the int-set membership test.
        if CT_MODELS[code] not in allowed_codes:
            # Build a friendly "what IS allowed" hint from CT_MODELS lookup
            allowed_names = sorted(
                name for name, c in CT_MODELS.items() if c in allowed_codes
            )
            hint = ", ".join(allowed_names) if allowed_names else "(none — this class has no CT model selection)"
            raise cv.Invalid(
                f"CT model code {code} is not valid for sensor_class "
                f"`{sensor_class}` ({location}). The firmware will reject this "
                f"write with DEV_ERR_PARAM. Allowed for `{sensor_class}`: {hint}."
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
                cv.Length(min=1, max=3),
            ),
            cv.Optional(CONF_SENSOR_CLASS, default="SCT_013"): cv.enum(
                RBAMP_SENSOR_CLASSES, upper=True
            ),
            cv.Optional(CONF_BIDIRECTIONAL, default=False): cv.boolean,
            cv.Optional(CONF_NEW_ADDRESS): cv.i2c_address,
            cv.Optional(CONF_BROADCAST_LATCH, default=False): cv.boolean,
            cv.Optional(CONF_TOPOLOGY, default="SINGLE"): cv.enum(
                TOPOLOGIES, upper=True
            ),
            cv.Optional(CONF_FLEET_GC_ENABLE): cv.boolean,
            cv.Optional(CONF_GROUP_ID): cv.All(cv.uint8_t, cv.Range(min=0, max=255)),
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x50)),
    _validate_new_address,
    _validate_ct_model_mutex,
    _validate_ct_model_per_class,
)


def _clamp_update_interval(config):
    """Reject `update_interval` < 1s (docs #5). The latch-rotation cadence
    needs at least one full ~200 ms RT window plus settle time per cycle;
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


# Apply the update_interval clamp on top of the per-class validator chain.
CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _clamp_update_interval)


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
