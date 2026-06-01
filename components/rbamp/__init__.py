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
CT_MODELS = {
    "SCT_013_005": 1,
    "SCT_013_010": 2,
    "SCT_013_030": 3,
    "SCT_013_050": 4,
    "SCT_013_100": 5,
}

# Sensor classes — v1.2+ firmware. Pinned via `sensor_class:` YAML key (writes
# REG_SENSOR_CLASS + CMD_SAVE_GAINS). On v1.2+ firmware this is a PRECONDITION
# for `ct_model:` / `ct_models:` writes — calling either while the class is
# still UNSET yields a runtime warning and skips the write. On v1.0 / v1.1
# firmware the key is metadata-only (same status as `ct_model:`). WIRED_CT
# and BUILTIN_CT are reserved for future SKU variants.
RBAMP_SENSOR_CLASSES = {
    "UNSET":      0,
    "SCT_013":    1,
    "WIRED_CT":   2,
    "BUILTIN_CT": 3,
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
        }
    )
    .extend(cv.polling_component_schema("60s"))
    .extend(i2c.i2c_device_schema(0x50)),
    _validate_new_address,
    _validate_ct_model_mutex,
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
