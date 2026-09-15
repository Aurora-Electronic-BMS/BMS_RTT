from __future__ import annotations

import math
import time

from .protocol import (
    AIR_NEG,
    AIR_POS,
    CELL_CHANNELS,
    CELL_GROUPS,
    ControlMode,
    TEMP_SLOTS,
    TS_ERROR_NONE,
    Telemetry,
    default_cell_valid_flags,
    is_valid_cell_slot,
)


class TelemetrySimulator:
    def __init__(self) -> None:
        self._start = time.monotonic()
        self._phase = 0.0

    def sample(self) -> Telemetry:
        elapsed = time.monotonic() - self._start
        self._phase += 0.17
        valid = default_cell_valid_flags()
        cells: list[int] = []
        valid_values: list[int] = []
        for group in range(1, CELL_GROUPS + 1):
            for channel in range(1, CELL_CHANNELS + 1):
                value = int(3700 + 40 * math.sin(self._phase + group * 0.23 + channel * 0.09))
                if not is_valid_cell_slot(group, channel):
                    value = 0
                cells.append(value)
                if is_valid_cell_slot(group, channel):
                    valid_values.append(value)

        temps = [
            int(27 + 5 * math.sin(self._phase * 0.45 + index * 0.18))
            for index in range(TEMP_SLOTS)
        ]

        return Telemetry(
            uptime_ms=int(elapsed * 1000),
            ts_state=1,
            target_state=1,
            error_kind=TS_ERROR_NONE,
            control_mode=ControlMode.PASSIVE,
            air_bits=AIR_POS | AIR_NEG,
            sdc_closed=True,
            hv_active=True,
            pack_voltage_mV=sum(valid_values),
            shunt_voltage_bat_mV=sum(valid_values),
            shunt_voltage_veh_mV=sum(valid_values) - 120,
            shunt_current_mA=12500,
            busbar_temp_dC=310,
            min_cell_mV=min(valid_values),
            max_cell_mV=max(valid_values),
            min_temp_C=min(temps),
            max_temp_C=max(temps),
            soc_percent=int(80 + 8 * math.sin(self._phase * 0.15)),
            cell_valid=valid,
            cell_mV=tuple(cells),
            temperatures_C=tuple(temps),
        )
