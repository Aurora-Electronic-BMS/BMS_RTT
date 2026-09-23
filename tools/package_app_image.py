#!/usr/bin/env python3
"""Package the APP image for both Bootloader upgrade and direct ST-Link flashing.

Outputs:
  - <name>.bin           APP-only binary for the host Firmware Upgrade flow.
  - <name>.metadata.bin  Bootloader validity metadata at 0x080FFF00.
  - <name>_stlink.hex    Intel HEX containing APP @ 0x08010000 and metadata @ 0x080FFF00.
"""

from __future__ import annotations

import argparse
import binascii
import shutil
import struct
import subprocess
import sys
from pathlib import Path


UINT32_MASK = 0xFFFFFFFF

APP_START_ADDRESS_DEFAULT = 0x08010000
APP_MAX_IMAGE_SIZE_DEFAULT = 0x000B0000
APP_METADATA_ADDRESS_DEFAULT = 0x080FFF00
APP_METADATA_MAGIC_DEFAULT = 0xB00710AD

MAIN_SRAM_START = 0x20000000
MAIN_SRAM_END = 0x20020000
CCM_SRAM_START = 0x10000000
CCM_SRAM_END = 0x10010000


def parse_int(value: str) -> int:
    return int(value, 0)


def u32_not(value: int) -> int:
    return (~value) & UINT32_MASK


def is_stack_valid(stack: int) -> bool:
    in_main_sram = MAIN_SRAM_START < stack <= MAIN_SRAM_END
    in_ccm_sram = CCM_SRAM_START < stack <= CCM_SRAM_END
    return (stack & 0x7) == 0 and (in_main_sram or in_ccm_sram)


def is_reset_vector_valid(reset: int, app_start: int, image_length: int) -> bool:
    reset_address = reset & ~0x1
    return (
        (reset & 0x1) == 0x1
        and app_start <= reset_address < (app_start + image_length)
    )


def build_metadata(
    image: bytes,
    *,
    app_start: int,
    max_image_size: int,
    metadata_address: int,
    magic: int,
) -> tuple[bytes, int, int]:
    image_length = len(image)
    metadata_limited_size = metadata_address - app_start
    max_image_size = min(max_image_size, metadata_limited_size)

    if image_length < 8:
        raise ValueError(f"APP image is too small: {image_length} bytes")
    if image_length > max_image_size:
        raise ValueError(
            f"APP image is {image_length} bytes, exceeds reserved APP area "
            f"{max_image_size} bytes"
        )

    stack, reset = struct.unpack_from("<II", image, 0)
    if not is_stack_valid(stack):
        raise ValueError(f"invalid initial stack pointer: 0x{stack:08X}")
    if not is_reset_vector_valid(reset, app_start, image_length):
        raise ValueError(
            f"invalid reset vector: 0x{reset:08X}; expected Thumb address inside "
            f"0x{app_start:08X}..0x{app_start + image_length - 1:08X}"
        )

    image_crc32 = binascii.crc32(image) & UINT32_MASK
    metadata = struct.pack(
        "<IIIII",
        magic,
        u32_not(magic),
        image_length,
        image_crc32,
        u32_not(image_crc32),
    )
    return metadata, image_length, image_crc32


def intel_hex_record(address: int, record_type: int, data: bytes = b"") -> str:
    if not 0 <= address <= 0xFFFF:
        raise ValueError(f"Intel HEX record address out of range: 0x{address:X}")
    if len(data) > 0xFF:
        raise ValueError(f"Intel HEX record data too long: {len(data)} bytes")

    record = bytes(
        [len(data), (address >> 8) & 0xFF, address & 0xFF, record_type & 0xFF]
    ) + data
    checksum = (-sum(record)) & 0xFF
    return ":" + record.hex().upper() + f"{checksum:02X}"


def write_ihex(segments: list[tuple[int, bytes]], output: Path) -> None:
    lines: list[str] = []
    current_upper: int | None = None

    for base_address, data in sorted(segments, key=lambda item: item[0]):
        offset = 0
        while offset < len(data):
            absolute = base_address + offset
            upper = absolute >> 16
            lower = absolute & 0xFFFF
            if upper != current_upper:
                lines.append(intel_hex_record(0, 0x04, struct.pack(">H", upper)))
                current_upper = upper

            chunk_len = min(16, len(data) - offset, 0x10000 - lower)
            chunk = data[offset : offset + chunk_len]
            lines.append(intel_hex_record(lower, 0x00, chunk))
            offset += chunk_len

    lines.append(intel_hex_record(0, 0x01))
    output.write_text("\n".join(lines) + "\n", encoding="ascii")


def run_objcopy(objcopy: str, elf: Path, binary: Path) -> None:
    try:
        subprocess.run(
            [objcopy, "-O", "binary", str(elf), str(binary)],
            check=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError(
            f"objcopy not found: {objcopy}. Pass --objcopy with the full "
            "arm-none-eabi-objcopy path or add it to PATH."
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(f"objcopy failed with exit code {exc.returncode}") from exc


def copy_binary_if_needed(source: Path, destination: Path) -> None:
    if source.resolve() == destination.resolve():
        return
    shutil.copyfile(source, destination)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Generate APP .bin, Bootloader metadata, and ST-Link HEX."
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--elf", type=Path, help="APP ELF image")
    source.add_argument("--bin", type=Path, help="APP-only binary image")
    parser.add_argument(
        "--max-image-size",
        type=parse_int,
        default=APP_MAX_IMAGE_SIZE_DEFAULT,
        help="Maximum APP binary size, default: 0xB0000 (Sector 4..9)",
    )
    parser.add_argument(
        "--objcopy",
        default="arm-none-eabi-objcopy",
        help="objcopy executable used when --elf is provided",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=None,
        help="output directory, default: input file directory",
    )
    parser.add_argument(
        "--name",
        default=None,
        help="output basename, default: input stem",
    )
    parser.add_argument(
        "--app-start",
        type=parse_int,
        default=APP_START_ADDRESS_DEFAULT,
        help="APP flash start address, default: 0x08010000",
    )
    parser.add_argument(
        "--metadata-address",
        type=parse_int,
        default=APP_METADATA_ADDRESS_DEFAULT,
        help="Bootloader metadata address, default: 0x080FFF00",
    )
    parser.add_argument(
        "--magic",
        type=parse_int,
        default=APP_METADATA_MAGIC_DEFAULT,
        help="Bootloader metadata magic, default: 0xB00710AD",
    )

    args = parser.parse_args(argv)
    input_path = args.elf if args.elf is not None else args.bin
    assert input_path is not None

    out_dir = args.out_dir if args.out_dir is not None else input_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    output_name = args.name if args.name is not None else input_path.stem

    app_binary = out_dir / f"{output_name}.bin"
    metadata_binary = out_dir / f"{output_name}.metadata.bin"
    stlink_hex = out_dir / f"{output_name}_stlink.hex"

    try:
        if args.elf is not None:
            run_objcopy(args.objcopy, args.elf, app_binary)
        else:
            copy_binary_if_needed(args.bin, app_binary)

        image = app_binary.read_bytes()
        metadata, image_length, image_crc32 = build_metadata(
            image,
            app_start=args.app_start,
            max_image_size=args.max_image_size,
            metadata_address=args.metadata_address,
            magic=args.magic,
        )
        metadata_binary.write_bytes(metadata)
        write_ihex(
            [
                (args.app_start, image),
                (args.metadata_address, metadata),
            ],
            stlink_hex,
        )

        print(f"Generated APP binary:      {app_binary}")
        print(f"Generated metadata binary: {metadata_binary}")
        print(f"Generated ST-Link HEX:     {stlink_hex}")
        print(f"image_length={image_length}, image_crc32=0x{image_crc32:08X}")
    except Exception as exc:
        print(f"package_app_image.py: error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
