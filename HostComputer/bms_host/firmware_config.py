from __future__ import annotations

from dataclasses import dataclass


DEFAULT_MIN_CELL_VOLT_MV = 1000
DEFAULT_MAX_CELL_VOLT_MV = 4200


@dataclass(frozen=True)
class CellVoltageThresholds:
    min_mV: int
    max_mV: int


def default_cell_voltage_thresholds() -> CellVoltageThresholds:
    return CellVoltageThresholds(DEFAULT_MIN_CELL_VOLT_MV, DEFAULT_MAX_CELL_VOLT_MV)
