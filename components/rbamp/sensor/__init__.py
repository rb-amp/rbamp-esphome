"""ESPHome sensor platform for rbAmp.

Declares sensors bound to a parent `rbamp:` component instance. Supports two
topology-field groups (mutually exclusive):

  - SINGLE-phase (current firmware): `voltage`, `current[/_1/_2]`,
    `power[/_1/_2]`, `energy[/_1/_2]`, etc.
  - PHASED (future U2I2 / U3I3 SKU): `voltage_a/_b/_c`, `current_a/_b/_c`,
    `power_a/_b/_c`, `energy_a/_b/_c`, `power_total`.

Mixing the two groups in one block raises a validation error.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_APPARENT_POWER,
    CONF_CURRENT,
    CONF_ENERGY,
    CONF_FREQUENCY,
    CONF_POWER,
    CONF_POWER_FACTOR,
    CONF_REACTIVE_POWER,
    CONF_VOLTAGE,
    DEVICE_CLASS_APPARENT_POWER,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_DEGREES,
    UNIT_EMPTY,
    UNIT_HERTZ,
    UNIT_VOLT,
    UNIT_VOLT_AMPS,
    UNIT_VOLT_AMPS_REACTIVE,
    UNIT_VOLT_AMPS_REACTIVE_HOURS,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)

from .. import RbAmpComponent, rbamp_ns  # noqa: F401

DEPENDENCIES = ["rbamp"]

CONF_RBAMP_ID = "rbamp_id"

# Single-phase sub-channel keys (used by UI2/UI3 + v1.5 UI5/UI7 senior SKUs).
# Slots 1..2 use the flat register block (0x86..0xD8) — same as ch0. Slots 3..6
# are v1.5 (senior SKUs) and read through the channel-access window
# (REG_CHANNEL_SELECT 0x32 + REG_CHANNEL_DATA 0x3C) — two I²C transactions per
# value (select + read), factor into your update_interval budget.
CONF_CURRENT_1 = "current_1"
CONF_CURRENT_2 = "current_2"
CONF_CURRENT_3 = "current_3"
CONF_CURRENT_4 = "current_4"
CONF_CURRENT_5 = "current_5"
CONF_CURRENT_6 = "current_6"
CONF_POWER_1 = "power_1"
CONF_POWER_2 = "power_2"
CONF_POWER_3 = "power_3"
CONF_POWER_4 = "power_4"
CONF_POWER_5 = "power_5"
CONF_POWER_6 = "power_6"
CONF_ENERGY_1 = "energy_1"
CONF_ENERGY_2 = "energy_2"
CONF_ENERGY_3 = "energy_3"
CONF_ENERGY_4 = "energy_4"
CONF_ENERGY_5 = "energy_5"
CONF_ENERGY_6 = "energy_6"
CONF_ENERGY_EXPORTED = "energy_exported"
CONF_ENERGY_EXPORTED_1 = "energy_exported_1"
CONF_ENERGY_EXPORTED_2 = "energy_exported_2"
CONF_POWER_FACTOR_1 = "power_factor_1"
CONF_POWER_FACTOR_2 = "power_factor_2"
CONF_POWER_FACTOR_3 = "power_factor_3"
CONF_POWER_FACTOR_4 = "power_factor_4"
CONF_POWER_FACTOR_5 = "power_factor_5"
CONF_POWER_FACTOR_6 = "power_factor_6"
CONF_REACTIVE_POWER_1 = "reactive_power_1"
CONF_REACTIVE_POWER_2 = "reactive_power_2"
CONF_REACTIVE_POWER_3 = "reactive_power_3"
CONF_REACTIVE_POWER_4 = "reactive_power_4"
CONF_REACTIVE_POWER_5 = "reactive_power_5"
CONF_REACTIVE_POWER_6 = "reactive_power_6"

# Phased keys (reserved for U2I2 / U3I3 future SKU)
CONF_VOLTAGE_A = "voltage_a"
CONF_VOLTAGE_B = "voltage_b"
CONF_VOLTAGE_C = "voltage_c"
CONF_CURRENT_A = "current_a"
CONF_CURRENT_B = "current_b"
CONF_CURRENT_C = "current_c"
CONF_POWER_A = "power_a"
CONF_POWER_B = "power_b"
CONF_POWER_C = "power_c"
CONF_POWER_TOTAL = "power_total"
CONF_ENERGY_A = "energy_a"
CONF_ENERGY_B = "energy_b"
CONF_ENERGY_C = "energy_c"
CONF_ENERGY_EXPORTED_A = "energy_exported_a"
CONF_ENERGY_EXPORTED_B = "energy_exported_b"
CONF_ENERGY_EXPORTED_C = "energy_exported_c"
CONF_POWER_FACTOR_A = "power_factor_a"
CONF_POWER_FACTOR_B = "power_factor_b"
CONF_POWER_FACTOR_C = "power_factor_c"
CONF_REACTIVE_POWER_A = "reactive_power_a"
CONF_REACTIVE_POWER_B = "reactive_power_b"
CONF_REACTIVE_POWER_C = "reactive_power_c"

# v1.8 — grand-total / reactive energy, fed by on-device energy counters
# (16-bit-map devices advertising them; on other devices the keys validate
# but never publish, with a boot-time warning). Grand totals cover ALL device
# channels. `energy_net` = import − export, so it can decrease (STATE_CLASS_TOTAL).
CONF_ENERGY_TOTAL = "energy_total"
CONF_ENERGY_EXPORTED_TOTAL = "energy_exported_total"
CONF_ENERGY_NET = "energy_net"
CONF_REACTIVE_ENERGY = "reactive_energy"
CONF_REACTIVE_ENERGY_EXPORTED = "reactive_energy_exported"
# v1.8 — three-phase voltage geometry (phase A is the 0° reference, so only
# B/C angles are exposed). `phase_sequence` publishes 1 forward · 2 reverse ·
# 3 abnormal, and "unknown" (NAN) while it cannot be determined.
CONF_VOLTAGE_ANGLE_B = "voltage_angle_b"
CONF_VOLTAGE_ANGLE_C = "voltage_angle_c"
CONF_PHASE_SEQUENCE = "phase_sequence"

VOLTAGE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
)
CURRENT_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
)
POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
ENERGY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
PF_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_EMPTY,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_POWER_FACTOR,
    state_class=STATE_CLASS_MEASUREMENT,
)
APPARENT_POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT_AMPS,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_APPARENT_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
REACTIVE_POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT_AMPS_REACTIVE,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_REACTIVE_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
ENERGY_NET_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL,
)
REACTIVE_ENERGY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT_AMPS_REACTIVE_HOURS,
    accuracy_decimals=3,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)
VOLTAGE_ANGLE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_DEGREES,
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:angle-acute",
)
PHASE_SEQUENCE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_EMPTY,
    accuracy_decimals=0,
    icon="mdi:sine-wave",
)
FREQUENCY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_HERTZ,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_FREQUENCY,
    state_class=STATE_CLASS_MEASUREMENT,
)

SINGLE_FIELDS = {
    CONF_VOLTAGE: VOLTAGE_SCHEMA,
    CONF_CURRENT: CURRENT_SCHEMA,
    CONF_CURRENT_1: CURRENT_SCHEMA,
    CONF_CURRENT_2: CURRENT_SCHEMA,
    CONF_CURRENT_3: CURRENT_SCHEMA,
    CONF_CURRENT_4: CURRENT_SCHEMA,
    CONF_CURRENT_5: CURRENT_SCHEMA,
    CONF_CURRENT_6: CURRENT_SCHEMA,
    CONF_POWER: POWER_SCHEMA,
    CONF_POWER_1: POWER_SCHEMA,
    CONF_POWER_2: POWER_SCHEMA,
    CONF_POWER_3: POWER_SCHEMA,
    CONF_POWER_4: POWER_SCHEMA,
    CONF_POWER_5: POWER_SCHEMA,
    CONF_POWER_6: POWER_SCHEMA,
    CONF_ENERGY: ENERGY_SCHEMA,
    CONF_ENERGY_1: ENERGY_SCHEMA,
    CONF_ENERGY_2: ENERGY_SCHEMA,
    CONF_ENERGY_3: ENERGY_SCHEMA,
    CONF_ENERGY_4: ENERGY_SCHEMA,
    CONF_ENERGY_5: ENERGY_SCHEMA,
    CONF_ENERGY_6: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_1: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_2: ENERGY_SCHEMA,
    CONF_POWER_FACTOR: PF_SCHEMA,
    CONF_POWER_FACTOR_1: PF_SCHEMA,
    CONF_POWER_FACTOR_2: PF_SCHEMA,
    CONF_POWER_FACTOR_3: PF_SCHEMA,
    CONF_POWER_FACTOR_4: PF_SCHEMA,
    CONF_POWER_FACTOR_5: PF_SCHEMA,
    CONF_POWER_FACTOR_6: PF_SCHEMA,
    CONF_REACTIVE_POWER: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_1: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_2: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_3: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_4: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_5: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_6: REACTIVE_POWER_SCHEMA,
}

PHASED_FIELDS = {
    CONF_VOLTAGE_A: VOLTAGE_SCHEMA,
    CONF_VOLTAGE_B: VOLTAGE_SCHEMA,
    CONF_VOLTAGE_C: VOLTAGE_SCHEMA,
    CONF_CURRENT_A: CURRENT_SCHEMA,
    CONF_CURRENT_B: CURRENT_SCHEMA,
    CONF_CURRENT_C: CURRENT_SCHEMA,
    CONF_POWER_A: POWER_SCHEMA,
    CONF_POWER_B: POWER_SCHEMA,
    CONF_POWER_C: POWER_SCHEMA,
    CONF_POWER_TOTAL: POWER_SCHEMA,
    CONF_ENERGY_A: ENERGY_SCHEMA,
    CONF_ENERGY_B: ENERGY_SCHEMA,
    CONF_ENERGY_C: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_A: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_B: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_C: ENERGY_SCHEMA,
    CONF_POWER_FACTOR_A: PF_SCHEMA,
    CONF_POWER_FACTOR_B: PF_SCHEMA,
    CONF_POWER_FACTOR_C: PF_SCHEMA,
    CONF_REACTIVE_POWER_A: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_B: REACTIVE_POWER_SCHEMA,
    CONF_REACTIVE_POWER_C: REACTIVE_POWER_SCHEMA,
    # v1.8 — voltage phase angles vs phase A (three-phase SKUs)
    CONF_VOLTAGE_ANGLE_B: VOLTAGE_ANGLE_SCHEMA,
    CONF_VOLTAGE_ANGLE_C: VOLTAGE_ANGLE_SCHEMA,
}

# Shared (topology-independent) sensor fields
SHARED_FIELDS = {
    CONF_FREQUENCY: FREQUENCY_SCHEMA,
    CONF_APPARENT_POWER: APPARENT_POWER_SCHEMA,
    # v1.8 — on-device energy counters (grand totals over all device channels)
    CONF_ENERGY_TOTAL: ENERGY_SCHEMA,
    CONF_ENERGY_EXPORTED_TOTAL: ENERGY_SCHEMA,
    CONF_ENERGY_NET: ENERGY_NET_SCHEMA,
    CONF_REACTIVE_ENERGY: REACTIVE_ENERGY_SCHEMA,
    CONF_REACTIVE_ENERGY_EXPORTED: REACTIVE_ENERGY_SCHEMA,
    CONF_PHASE_SEQUENCE: PHASE_SEQUENCE_SCHEMA,
}


# Single-phase per-slot dependencies: a power/energy/pf/Q field at slot N requires
# the matching current at slot N. Power/energy/pf/Q for the primary slot ALSO
# require a voltage (V×I makes no sense without V).
_SINGLE_SLOT_DEPS = [
    # (field, required_companions)
    (CONF_POWER, [CONF_CURRENT, CONF_VOLTAGE]),
    (CONF_POWER_1, [CONF_CURRENT_1, CONF_VOLTAGE]),
    (CONF_POWER_2, [CONF_CURRENT_2, CONF_VOLTAGE]),
    (CONF_POWER_3, [CONF_CURRENT_3, CONF_VOLTAGE]),
    (CONF_POWER_4, [CONF_CURRENT_4, CONF_VOLTAGE]),
    (CONF_POWER_5, [CONF_CURRENT_5, CONF_VOLTAGE]),
    (CONF_POWER_6, [CONF_CURRENT_6, CONF_VOLTAGE]),
    (CONF_ENERGY, [CONF_CURRENT, CONF_VOLTAGE]),
    (CONF_ENERGY_1, [CONF_CURRENT_1, CONF_VOLTAGE]),
    (CONF_ENERGY_2, [CONF_CURRENT_2, CONF_VOLTAGE]),
    (CONF_ENERGY_3, [CONF_CURRENT_3, CONF_VOLTAGE]),
    (CONF_ENERGY_4, [CONF_CURRENT_4, CONF_VOLTAGE]),
    (CONF_ENERGY_5, [CONF_CURRENT_5, CONF_VOLTAGE]),
    (CONF_ENERGY_6, [CONF_CURRENT_6, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR, [CONF_CURRENT, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_1, [CONF_CURRENT_1, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_2, [CONF_CURRENT_2, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_3, [CONF_CURRENT_3, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_4, [CONF_CURRENT_4, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_5, [CONF_CURRENT_5, CONF_VOLTAGE]),
    (CONF_POWER_FACTOR_6, [CONF_CURRENT_6, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER, [CONF_CURRENT, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER_1, [CONF_CURRENT_1, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER_2, [CONF_CURRENT_2, CONF_VOLTAGE]),
    # Reactive power (Q) is not exposed via the channel window on ch3+ (public
    # fields cover I_RMS/I_PEAK/P_REAL/PF only; Q would need private field).
    # Slots 3..6 accept CONF_REACTIVE_POWER_* schema-wise but publish zero/NaN.
    (CONF_REACTIVE_POWER_3, [CONF_CURRENT_3, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER_4, [CONF_CURRENT_4, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER_5, [CONF_CURRENT_5, CONF_VOLTAGE]),
    (CONF_REACTIVE_POWER_6, [CONF_CURRENT_6, CONF_VOLTAGE]),
    (CONF_APPARENT_POWER, [CONF_CURRENT, CONF_VOLTAGE]),
]


def _validate_topology_consistency(config):
    has_single = any(k in config for k in SINGLE_FIELDS)
    has_phased = any(k in config for k in PHASED_FIELDS)
    if has_single and has_phased:
        raise cv.Invalid(
            "Cannot mix single-phase fields (voltage, current, current_1, ...) "
            "with phased fields (voltage_a/b/c, current_a/b/c, ...). "
            "Pick one group based on your rbAmp SKU."
        )
    # Per-slot dependencies (single-phase). Phased dependencies are deferred —
    # firmware support for split/three-phase variants is reserved for future SKUs.
    for field, required in _SINGLE_SLOT_DEPS:
        if field in config:
            for dep in required:
                if dep not in config:
                    raise cv.Invalid(
                        f"`{field}` requires `{dep}` to also be declared — "
                        f"the underlying chip cannot compute it without that input."
                    )
    return config


_schema_dict = {cv.Required(CONF_RBAMP_ID): cv.use_id(RbAmpComponent)}
for _k, _v in {**SINGLE_FIELDS, **PHASED_FIELDS, **SHARED_FIELDS}.items():
    _schema_dict[cv.Optional(_k)] = _v

CONFIG_SCHEMA = cv.All(cv.Schema(_schema_dict), _validate_topology_consistency)

# Map YAML key -> C++ setter name on RbAmpComponent
_SETTER_MAP = {
    # Shared
    CONF_FREQUENCY: "set_frequency_sensor",
    CONF_APPARENT_POWER: "set_apparent_power_sensor",
    # Single-phase
    CONF_VOLTAGE: "set_voltage_sensor",
    CONF_CURRENT: "set_current_sensor",
    CONF_CURRENT_1: "set_current_1_sensor",
    CONF_CURRENT_2: "set_current_2_sensor",
    CONF_CURRENT_3: "set_current_3_sensor",
    CONF_CURRENT_4: "set_current_4_sensor",
    CONF_CURRENT_5: "set_current_5_sensor",
    CONF_CURRENT_6: "set_current_6_sensor",
    CONF_POWER: "set_power_sensor",
    CONF_POWER_1: "set_power_1_sensor",
    CONF_POWER_2: "set_power_2_sensor",
    CONF_POWER_3: "set_power_3_sensor",
    CONF_POWER_4: "set_power_4_sensor",
    CONF_POWER_5: "set_power_5_sensor",
    CONF_POWER_6: "set_power_6_sensor",
    CONF_ENERGY: "set_energy_sensor",
    CONF_ENERGY_1: "set_energy_1_sensor",
    CONF_ENERGY_2: "set_energy_2_sensor",
    CONF_ENERGY_3: "set_energy_3_sensor",
    CONF_ENERGY_4: "set_energy_4_sensor",
    CONF_ENERGY_5: "set_energy_5_sensor",
    CONF_ENERGY_6: "set_energy_6_sensor",
    CONF_ENERGY_EXPORTED: "set_energy_exported_sensor",
    CONF_ENERGY_EXPORTED_1: "set_energy_exported_1_sensor",
    CONF_ENERGY_EXPORTED_2: "set_energy_exported_2_sensor",
    CONF_POWER_FACTOR: "set_power_factor_sensor",
    CONF_POWER_FACTOR_1: "set_power_factor_1_sensor",
    CONF_POWER_FACTOR_2: "set_power_factor_2_sensor",
    CONF_POWER_FACTOR_3: "set_power_factor_3_sensor",
    CONF_POWER_FACTOR_4: "set_power_factor_4_sensor",
    CONF_POWER_FACTOR_5: "set_power_factor_5_sensor",
    CONF_POWER_FACTOR_6: "set_power_factor_6_sensor",
    CONF_REACTIVE_POWER: "set_reactive_power_sensor",
    CONF_REACTIVE_POWER_1: "set_reactive_power_1_sensor",
    CONF_REACTIVE_POWER_2: "set_reactive_power_2_sensor",
    CONF_REACTIVE_POWER_3: "set_reactive_power_3_sensor",
    CONF_REACTIVE_POWER_4: "set_reactive_power_4_sensor",
    CONF_REACTIVE_POWER_5: "set_reactive_power_5_sensor",
    CONF_REACTIVE_POWER_6: "set_reactive_power_6_sensor",
    # Phased
    CONF_VOLTAGE_A: "set_voltage_a_sensor",
    CONF_VOLTAGE_B: "set_voltage_b_sensor",
    CONF_VOLTAGE_C: "set_voltage_c_sensor",
    CONF_CURRENT_A: "set_current_a_sensor",
    CONF_CURRENT_B: "set_current_b_sensor",
    CONF_CURRENT_C: "set_current_c_sensor",
    CONF_POWER_A: "set_power_a_sensor",
    CONF_POWER_B: "set_power_b_sensor",
    CONF_POWER_C: "set_power_c_sensor",
    CONF_POWER_TOTAL: "set_power_total_sensor",
    CONF_ENERGY_A: "set_energy_a_sensor",
    CONF_ENERGY_B: "set_energy_b_sensor",
    CONF_ENERGY_C: "set_energy_c_sensor",
    CONF_ENERGY_EXPORTED_A: "set_energy_exported_a_sensor",
    CONF_ENERGY_EXPORTED_B: "set_energy_exported_b_sensor",
    CONF_ENERGY_EXPORTED_C: "set_energy_exported_c_sensor",
    CONF_POWER_FACTOR_A: "set_power_factor_a_sensor",
    CONF_POWER_FACTOR_B: "set_power_factor_b_sensor",
    CONF_POWER_FACTOR_C: "set_power_factor_c_sensor",
    CONF_REACTIVE_POWER_A: "set_reactive_power_a_sensor",
    CONF_REACTIVE_POWER_B: "set_reactive_power_b_sensor",
    CONF_REACTIVE_POWER_C: "set_reactive_power_c_sensor",
    # v1.8
    CONF_ENERGY_TOTAL: "set_energy_total_sensor",
    CONF_ENERGY_EXPORTED_TOTAL: "set_energy_exported_total_sensor",
    CONF_ENERGY_NET: "set_energy_net_sensor",
    CONF_REACTIVE_ENERGY: "set_reactive_energy_sensor",
    CONF_REACTIVE_ENERGY_EXPORTED: "set_reactive_energy_exported_sensor",
    CONF_VOLTAGE_ANGLE_B: "set_voltage_angle_b_sensor",
    CONF_VOLTAGE_ANGLE_C: "set_voltage_angle_c_sensor",
    CONF_PHASE_SEQUENCE: "set_phase_sequence_sensor",
}


async def to_code(config):
    parent = await cg.get_variable(config[CONF_RBAMP_ID])
    for key, setter in _SETTER_MAP.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(parent, setter)(sens))
