# BMS Host Computer

PyQt6 host computer for the BMS demo firmware. It receives binary telemetry
over a serial port, displays all 144 cell slots with the six invalid slots
marked as `N/A`, shows 48 temperatures, records CSV logs, and sends guarded
state/relay commands.

## Setup

```sh
conda env create -f HostComputer/environment.yml
conda activate bms_pc
```

If the environment already exists:

```sh
conda env update -n bms_pc -f HostComputer/environment.yml
```

## Run

Simulated data:

```sh
python -m bms_host --simulate
```

Real serial port:

```sh
python -m bms_host
```

## macOS App Packaging

Build the macOS `.app` on macOS from the host computer environment:

```sh
cd HostComputer
conda activate bms_pc
python -m pip install pyinstaller
./macos_packaging/build_macos_app.sh
```

The generated app is:

```text
HostComputer/dist/SZTU-BMS.app
```

The host reads `TOTAL_IC`, `MIN_CELL_VOLT`, and `MAX_CELL_VOLT` from BMS
telemetry after the serial link is connected, so the packaged app does not need
`APP/Core/Inc/slave_monitor.h`.

The firmware UART settings are `115200 8N1`.

The APP listens on both USART1 and USART3 and uses the port that first receives
a valid protocol frame. The active port is released after heartbeat timeout.

The BMS does not stream telemetry immediately after boot. Clicking `Connect`
opens the serial port and sends a heartbeat; firmware starts telemetry only
after receiving heartbeats and stops again after heartbeat timeout.

## Protocol

Frames use:

```text
B5 4D | version:u8 | msg_type:u8 | seq:u16 | payload_len:u16 | payload | crc16:u16
```

All integer payload fields are little-endian. The CRC is Modbus CRC16 over
`version` through the end of the payload, excluding the two magic bytes.

Cell slots are displayed by group plus global cell number. `G01-C001` is the
first valid cell and `G12-C138` is the last valid cell. `G02/G04/G06/G08/G10/G12`
channel 12 are displayed as `N/A` and are not included in min/max calculations.

Engineering AIR debug mode requires typing `sztu` in the host UI before
the relay checkboxes are enabled. In that mode the firmware accepts independent
POS/NEG/PRE relay bits, while still rejecting nonzero relay closure when BMS
faults are active or SDC is open. Heartbeat timeout exits the mode.

The `Firmware Upgrade` button is immediately to the right of `Disconnect`.
It selects an APP `.bin`, checks its size, stack pointer, and reset vector, and
then performs the complete upgrade sequence: APP reset request, Bootloader
`HELLO`, `BEGIN`, 256-byte `DATA` packets, `END`, and `RUN`. A lost final END
ACK is retried before RUN, so a successful flash is not reported as failed.
Progress and Bootloader errors are shown in the message area. Do not remove power or disconnect the
serial cable while an upgrade is running.
