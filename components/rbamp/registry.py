"""Sensor-model registry loader (v1.6).

Source of truth: ``libs/spec/sensor_models.yaml`` at the repo root. Loaded
at codegen-time so ``__init__.py`` never carries a hand-maintained model
list — that was the exact source of the six-model drift fixed in v1.6.0.

Symbols exposed:

* ``CT_MODELS`` — ``{"SCT_013_005": 1, "WIRED_CT_6": 6, ...}``. Keys
  chosen for backwards-compat with pre-v1.6 YAMLs: SCT_013 gets
  ``SCT_013_<3-digit-A>`` derived from descriptor; WIRED_CT gets
  ``WIRED_CT_<code>`` (numeric).
* ``CT_MODEL_ALLOWED_PER_CLASS`` — allowed code set per class.
  ``characterized`` + ``legacy`` accepted (both are on shipping firmware);
  ``reserved`` rejected (no preset row → ERR_PARAM).
* ``CT_MODEL_STATUS`` — ``{(class_name, code): "characterized"|"legacy"|"reserved"}``.
  Consulted for the "legacy — not re-verified" warning in the validator.
* ``CT_MODEL_DESCRIPTORS`` — ``{(class_name, code): "SCT-013-005, 5 A"}``.
  Used by dump_config on the C++ side via cg.add(var.register_ct_model_descriptor_(...))
  if we ever wire it; currently for validator error messages only.

Fallback: if the YAML is unreadable (e.g. component published stand-alone,
no monorepo path), a MINIMAL hardcoded map is used (SCT_013 + WIRED_CT
1..3 legacy) with a build-time warning. This keeps stand-alone consumers
from breaking, while the monorepo path (real deployments) always drives
from the registry.
"""

from __future__ import annotations

import os
from typing import Dict, Set, Tuple

# Registry path — relative to this file:
#   tools/esphome-rbamp/components/rbamp/registry.py
#   ../../../../libs/spec/sensor_models.yaml
_REGISTRY_PATH = os.path.abspath(
    os.path.join(
        os.path.dirname(__file__),
        "..", "..", "..", "..", "libs", "spec", "sensor_models.yaml",
    )
)


def _sct013_amp_from_descriptor(descriptor: str) -> str | None:
    """Extract the 3-digit amp code from "SCT-013-005, 5 A" → "005"."""
    # descriptor form: "SCT-013-<AAA>, <N> A"
    if not descriptor.startswith("SCT-013-"):
        return None
    head = descriptor[len("SCT-013-"):]
    # head starts with digits until first non-digit
    amp = ""
    for ch in head:
        if ch.isdigit():
            amp += ch
        else:
            break
    if not amp:
        return None
    return amp.zfill(3)


def _load_from_yaml() -> Tuple[
    Dict[str, int],
    Dict[str, Set[int]],
    Dict[Tuple[str, int], str],
    Dict[Tuple[str, int], str],
]:
    """Parse the registry YAML, return (models, allowed_per_class, status, descriptors).

    Raises whatever ``yaml.safe_load`` raises on malformed input.
    """
    import yaml  # PyYAML — ESPHome dependency; always available at codegen time

    with open(_REGISTRY_PATH, encoding="utf-8") as fh:
        reg = yaml.safe_load(fh)

    # Class code → name lookup, e.g. {1: "SCT_013", 2: "WIRED_CT"}
    class_by_code: Dict[int, str] = {
        c["code"]: c["name"] for c in reg.get("classes", [])
    }

    models: Dict[str, int] = {}
    allowed: Dict[str, Set[int]] = {name: set() for name in class_by_code.values()}
    status: Dict[Tuple[str, int], str] = {}
    descriptors: Dict[Tuple[str, int], str] = {}

    for row in reg.get("models", []):
        class_code = row["class"]
        class_name = class_by_code.get(class_code)
        if class_name is None:
            continue  # spec drift — unknown class byte, skip silently
        code = row["code"]
        row_status = row.get("status", "reserved")
        descriptor = row.get("descriptor", "")

        status[(class_name, code)] = row_status
        descriptors[(class_name, code)] = descriptor

        # Symbol synthesis: keep pre-v1.6 keys stable, add new ones by rule.
        if class_name == "SCT_013":
            amp = _sct013_amp_from_descriptor(descriptor)
            if amp:
                symbol = f"SCT_013_{amp}"
                # All SCT_013 rows are addressable by symbol (even reserved,
                # so the validator emits the friendlier "reserved" error
                # rather than cv.enum "unknown value").
                models[symbol] = code
        elif class_name == "WIRED_CT":
            # WIRED_CT symbols by code — stable across the 13-slot range.
            symbol = f"WIRED_CT_{code}"
            models[symbol] = code
        # BUILTIN_CT / UNSET: no per-model symbols by design.

        # Allowed set: characterized + legacy are on-firmware; reserved
        # returns ERR_PARAM and must not be a live shipping choice.
        if row_status in ("characterized", "legacy"):
            allowed[class_name].add(code)

    return models, allowed, status, descriptors


def _fallback() -> Tuple[
    Dict[str, int],
    Dict[str, Set[int]],
    Dict[Tuple[str, int], str],
    Dict[Tuple[str, int], str],
]:
    """Last-resort hardcoded map for the stand-alone / no-monorepo case.

    Matches the pre-v1.6 shipping surface (SCT_013 codes 1-6 with 5 reserved,
    WIRED_CT legacy slots 1-3). Anything newer requires the monorepo registry
    — a stand-alone deployment on old firmware won't hit the new codes anyway.
    """
    models = {
        "SCT_013_005": 1,
        "SCT_013_010": 2,
        "SCT_013_020": 6,
        "SCT_013_030": 3,
        "SCT_013_050": 4,
        "WIRED_CT_1":  1,
        "WIRED_CT_2":  2,
        "WIRED_CT_3":  3,
    }
    allowed = {
        "UNSET":      set(),
        "SCT_013":    {1, 2, 3, 4, 6},
        "WIRED_CT":   {1, 2, 3},
        "BUILTIN_CT": set(),
    }
    status = {
        ("SCT_013", 1): "characterized",
        ("SCT_013", 2): "characterized",
        ("SCT_013", 3): "characterized",
        ("SCT_013", 4): "characterized",
        ("SCT_013", 6): "characterized",
        ("WIRED_CT", 1): "legacy",
        ("WIRED_CT", 2): "legacy",
        ("WIRED_CT", 3): "legacy",
    }
    descriptors = {
        ("SCT_013", 1): "SCT-013-005, 5 A",
        ("SCT_013", 2): "SCT-013-010, 10 A",
        ("SCT_013", 3): "SCT-013-030, 30 A",
        ("SCT_013", 4): "SCT-013-050, 50 A",
        ("SCT_013", 6): "SCT-013-020, 20 A",
        ("WIRED_CT", 1): "Toroid 1:1000 (legacy)",
        ("WIRED_CT", 2): "Toroid 1:2000 (legacy)",
        ("WIRED_CT", 3): "Toroid 1:3000 (legacy)",
    }
    return models, allowed, status, descriptors


try:
    CT_MODELS, CT_MODEL_ALLOWED_PER_CLASS, CT_MODEL_STATUS, CT_MODEL_DESCRIPTORS = (
        _load_from_yaml()
    )
    REGISTRY_SOURCE = _REGISTRY_PATH
except Exception as exc:  # noqa: BLE001 — codegen must not hard-fail
    import logging
    logging.getLogger("rbamp").warning(
        "sensor_models.yaml unavailable (%s); falling back to hardcoded shipping "
        "map. New CT models (WIRED_CT_4..13) will be unknown to the schema.",
        exc,
    )
    CT_MODELS, CT_MODEL_ALLOWED_PER_CLASS, CT_MODEL_STATUS, CT_MODEL_DESCRIPTORS = (
        _fallback()
    )
    REGISTRY_SOURCE = "hardcoded fallback"
