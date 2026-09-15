#!/usr/bin/env python3
"""
Generate and write APP metadata for IAP bootloader.
"""
import sys
import struct
import zlib
import subprocess

APP_METADATA_ADDRESS = 0x080FFF00
APP_METADATA_MAGIC = 0xB00710AD

def crc32_ieee(data):
    """Calculate CRC32 using zlib (IEEE polynomial)."""
    return zlib.crc32(data) & 0xFFFFFFFF

def generate_metadata(bin_path):
    """Generate metadata for APP binary."""
    with open(bin_path, 'rb') as f:
        app_data = f.read()

    # Pad to 4-byte boundary with 0xFF
    while len(app_data) % 4 != 0:
        app_data += b'\xFF'

    image_length = len(app_data)
    image_crc32 = crc32_ieee(app_data)

    # Metadata structure: magic, magic_inv, length, crc32, crc32_inv
    metadata = struct.pack('<IIIII',
                          APP_METADATA_MAGIC,
                          ~APP_METADATA_MAGIC & 0xFFFFFFFF,
                          image_length,
                          image_crc32,
                          ~image_crc32 & 0xFFFFFFFF)

    print(f"APP size: {image_length} bytes")
    print(f"APP CRC32: 0x{image_crc32:08X}")
    print(f"Metadata: {metadata.hex()}")

    return metadata, image_length, image_crc32

def write_metadata_stlink(metadata):
    """Write metadata via ST-Link using STM32_Programmer_CLI."""
    # Write metadata to temporary file
    metadata_file = 'metadata_temp.bin'
    with open(metadata_file, 'wb') as f:
        f.write(metadata)

    # Program via STM32_Programmer_CLI
    cmd = [
        'STM32_Programmer_CLI',
        '-c', 'port=SWD', 'mode=UR', 'reset=HWrst',
        '-w', metadata_file, f'0x{APP_METADATA_ADDRESS:08X}',
        '-v'
    ]

    print(f"\nWriting metadata to 0x{APP_METADATA_ADDRESS:08X}...")
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode == 0:
        print("Metadata written successfully!")
        return True
    else:
        print(f"Error writing metadata: {result.stderr}")
        return False

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <APP.bin>")
        sys.exit(1)

    bin_path = sys.argv[1]
    metadata, length, crc = generate_metadata(bin_path)

    if write_metadata_stlink(metadata):
        print("\n[OK] APP metadata written successfully!")
        print(f"  Image length: {length} bytes")
        print(f"  Image CRC32:  0x{crc:08X}")
    else:
        print("\n[FAIL] Failed to write metadata")
        sys.exit(1)
