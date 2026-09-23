from __future__ import annotations

import math
import time

from .protocol import (
    AIR_NEG,
    AIR_POS,
    CELL_CHANNELS,
    CELL_GROUPS,
    BmsConfiguration,
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
        self.configuration = BmsConfiguration(
            total_ic=12,
            discharge_duration_ms=5000,
            no_error_time_ms=1000,
            air_close_wait_ms=10,
            precharge_95_wait_time_ms=5000,
            pre_hold_time_ms=200,
            max_state_hold_time_ms=500,
            drive_mode_timeout_ms=2000,
            max_charge_volt_dV=5800,
            max_charge_curr_dA=30,
            shunt_timeout_ms=500,
            shunt_overcurrent_discharge_mA=200000,
            shunt_overcurrent_charge_mA=15000,
            shunt_overcurrent_regen_mA=25000,
            shunt_overtemp_dC=1000,
            max_cell_volt_mV=4200,
            min_cell_volt_mV=3000,
            max_cell_temp_C=50,
            min_cell_temp_C=0,
            pec_current_threshold_mA=1000,
            shunt_timeout_confirm_ms=500,
            shunt_overcurrent_confirm_ms=500,
            shunt_overtemp_confirm_ms=1000,
            shunt_voltage_mismatch_confirm_ms=1000,
            slave_overvolt_confirm_ms=1000,
            slave_undervolt_confirm_ms=1000,
            slave_overtemp_confirm_ms=1000,
            slave_comm_confirm_ms=1000,
            can1_baudrate=500000,
            can2_baudrate=500000,
        )

    def save_configuration(self, configuration: BmsConfiguration) -> None:
        self.configuration = configuration

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
            cell_valid=valid,
            cell_mV=tuple(cells),
            temperatures_C=tuple(temps),
            soc_percent=round(80 + 8 * math.sin(self._phase * 0.15), 2),
        )
