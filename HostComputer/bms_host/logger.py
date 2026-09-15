from __future__ import annotations

import csv
from datetime import datetime
from pathlib import Path
from typing import TextIO

from .protocol import TEMP_GROUPS, TEMP_CHANNELS, Telemetry, cell_sequence_number, slot_index, valid_cell_slots


class BmsCsvLogger:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._file: TextIO | None = None
        self._writer: csv.writer | None = None

    def start(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._file = self.path.open("w", newline="")
        self._writer = csv.writer(self._file)
        self._writer.writerow(self._header())

    def close(self) -> None:
        if self._file is not None:
            self._file.close()
        self._file = None
        self._writer = None

    def write(self, telemetry: Telemetry) -> None:
        if self._writer is None:
            return
        row: list[object] = [
            datetime.now().isoformat(timespec="milliseconds"),
            telemetry.uptime_ms,
            telemetry.ts_state,
            telemetry.target_state,
            telemetry.error_kind,
            telemetry.control_mode,
            telemetry.air_bits,
            int(telemetry.sdc_closed),
            int(telemetry.hv_active),
            telemetry.pack_voltage_mV,
            telemetry.shunt_voltage_bat_mV,
            telemetry.shunt_voltage_veh_mV,
            telemetry.shunt_current_mA,
            telemetry.busbar_temp_dC,
            telemetry.min_cell_mV,
            telemetry.max_cell_mV,
            telemetry.min_temp_C,
            telemetry.max_temp_C,
            telemetry.soc_percent,
        ]
        for group, channel in valid_cell_slots():
            row.append(telemetry.cell_mV[slot_index(group, channel)])
        row.extend(telemetry.temperatures_C)
        self._writer.writerow(row)

    def _header(self) -> list[str]:
        header = [
            "host_time",
            "uptime_ms",
            "ts_state",
            "target_state",
            "error_kind",
            "control_mode",
            "air_bits",
            "sdc_closed",
            "hv_active",
            "pack_voltage_mV",
            "shunt_voltage_bat_mV",
            "shunt_voltage_veh_mV",
            "shunt_current_mA",
            "busbar_temp_dC",
            "min_cell_mV",
            "max_cell_mV",
            "min_temp_C",
            "max_temp_C",
            "soc_percent",
        ]
        header.extend(
            f"cell_G{group:02d}_C{cell_sequence_number(group, channel):03d}_mV"
            for group, channel in valid_cell_slots()
        )
        header.extend(
            f"temp_G{group:02d}_T{channel:02d}_C"
            for group in range(1, TEMP_GROUPS + 1)
            for channel in range(1, TEMP_CHANNELS + 1)
        )
        return header
