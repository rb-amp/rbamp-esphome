"""
AUTO-GENERATED from libs/spec/sensor_models.yaml — DO NOT EDIT

Sensor-model registry: what a CT_MODEL code MEANS (name + descriptor).
Which codes a given module ACCEPTS is runtime truth — write the code and
check for ERR_PARAM; an unknown code leaves the channel's model unchanged.

Regenerate:
    python tools/lib_codegen/codegen_models.py
"""


SENSOR_MODELS_CRC32 = 0xDC7E3CEE
SENSOR_MODELS_VERSION = 1

# ---- Sensor classes (REG_SENSOR_CLASS 0x25) ----
SENSOR_CLASS_UNSET        = 0
SENSOR_CLASS_SCT_013      = 1
SENSOR_CLASS_WIRED_CT     = 2
SENSOR_CLASS_BUILTIN_CT   = 3

# ---- Model codes; names strip the RBAMP_CT_ prefix ----
CT_SCT013_005         = 1  # class 1
CT_SCT013_010         = 2  # class 1
CT_SCT013_030         = 3  # class 1
CT_SCT013_050         = 4  # class 1
CT_SCT013_100         = 5  # class 1
CT_SCT013_020         = 6  # class 1
CT_SCT013_060         = 7  # class 1
CT_TOROID_1000        = 1  # class 2
CT_TOROID_2000        = 2  # class 2
CT_TOROID_3000        = 3  # class 2
CT_CT2000_92R         = 4  # class 2
CT_CLAMP2000_92R      = 5  # class 2
CT_CLAMP2000_1A       = 6  # class 2
CT_CLAMP2000_5A       = 7  # class 2
CT_CLAMP2000_10A      = 8  # class 2
CT_CLAMP2000_20A      = 9  # class 2
CT_CLAMP2000_30A      = 10  # class 2
CT_CLAMP2000_50A      = 11  # class 2
CT_CLAMP2000_80A      = 12  # class 2
CT_CLAMP2000_100A     = 13  # class 2

# (class, code) -> (name, descriptor, status)
SENSOR_MODELS = {
    (1, 1): ("SCT013_005", "SCT-013-005, 5 A", "characterized"),
    (1, 2): ("SCT013_010", "SCT-013-010, 10 A", "characterized"),
    (1, 3): ("SCT013_030", "SCT-013-030, 30 A", "characterized"),
    (1, 4): ("SCT013_050", "SCT-013-050, 50 A", "characterized"),
    (1, 5): ("SCT013_100", "SCT-013-100, 100 A", "characterized"),
    (1, 6): ("SCT013_020", "SCT-013-020, 20 A", "characterized"),
    (1, 7): ("SCT013_060", "SCT-013-060, 60 A", "characterized"),
    (2, 1): ("TOROID_1000", "Toroid 1:1000, 196 R burden", "legacy"),
    (2, 2): ("TOROID_2000", "Toroid 1:2000, 370 R burden", "legacy"),
    (2, 3): ("TOROID_3000", "Toroid 1:3000, 572 R burden", "legacy"),
    (2, 4): ("CT2000_92R", "CT 2000:1, 92 R burden", "legacy"),
    (2, 5): ("CLAMP2000_92R", "Clamp 2000:1, 92 R burden", "legacy"),
    (2, 6): ("CLAMP2000_1A", "Clamp 2000:1, 1 A", "characterized"),
    (2, 7): ("CLAMP2000_5A", "Clamp 2000:1, 5 A", "characterized"),
    (2, 8): ("CLAMP2000_10A", "Clamp 2000:1, 10 A", "characterized"),
    (2, 9): ("CLAMP2000_20A", "Clamp 2000:1, 20 A", "characterized"),
    (2, 10): ("CLAMP2000_30A", "Clamp 2000:1, 30 A", "characterized"),
    (2, 11): ("CLAMP2000_50A", "Clamp 2000:1, 50 A", "reserved"),
    (2, 12): ("CLAMP2000_80A", "Clamp 2000:1, 80 A", "characterized"),
    (2, 13): ("CLAMP2000_100A", "Clamp 2000:1, 100 A", "reserved"),
}


# (class, code) -> (nominal_a, fs_a, peak_a): rated RMS, RMS at clip onset
# for a sine, instantaneous clip ceiling (a distorted load clips at
# peak_a / crest factor). Models without known limits are absent.
SENSOR_LIMITS = {
    (1, 1): (5.0, 5.8, 8.2),
    (1, 2): (10.0, 11.6, 16.4),
    (1, 3): (30.0, 34.8, 49.2),
    (1, 4): (50.0, 58.0, 82.0),
    (1, 5): (100.0, 116.0, 164.0),
    (1, 6): (20.0, 23.2, 32.8),
    (1, 7): (60.0, 69.6, 98.4),
    (2, 6): (1.0, 1.27, 1.8),
    (2, 7): (4.6, 5.75, 8.18),
    (2, 8): (10.18, 12.73, 18.0),
    (2, 9): (20.4, 25.5, 36.0),
    (2, 10): (29.95, 37.4, 52.9),
    (2, 11): (50.91, 63.6, 90.0),
    (2, 12): (76.8, 95.9, 135.0),
    (2, 13): (101.82, 127.3, 180.0),
}


def limits(sensor_class: int, code: int):
    """(nominal_a, fs_a, peak_a) or None if the limits are not known."""
    return SENSOR_LIMITS.get((sensor_class, code))


def lookup(sensor_class: int, code: int):
    """(name, descriptor, status) or None. A registry hit does NOT mean the
    connected module accepts the code - that is runtime truth (ERR_PARAM)."""
    return SENSOR_MODELS.get((sensor_class, code))
