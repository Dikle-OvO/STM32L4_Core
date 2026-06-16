#!/usr/bin/env python3
"""
merge_hex.py - Merge Bootloader and Application hex/bin files

Usage:
    python scripts/merge_hex.py --bl build_bl/STM32L431_BL.bin \
                                --app build/Release/STM32L431CBT6.bin \
                                --output build/merged_firmware.bin

The script places BL at offset 0 and APP at the configured slot0 offset.
"""

import argparse
import struct
import sys
import os

# Must match flash_port.h partition layout
FLASH_BASE = 0x08000000
BL_OFFSET = 0x00000000
BL_MAX_SIZE = 16 * 1024       # 16KB
APP_OFFSET = 0x00004000       # Slot 0
APP_MAX_SIZE = 52 * 1024      # 52KB
TOTAL_FLASH = 128 * 1024      # 128KB


def merge_bin(bl_path: str, app_path: str, output_path: str):
    """Merge BL and APP binaries into a single flash image."""

    with open(bl_path, 'rb') as f:
        bl_data = f.read()

    with open(app_path, 'rb') as f:
        app_data = f.read()

    # Validate sizes
    if len(bl_data) > BL_MAX_SIZE:
        print(f"ERROR: Bootloader size ({len(bl_data)} bytes) exceeds limit ({BL_MAX_SIZE} bytes)")
        sys.exit(1)

    if len(app_data) > APP_MAX_SIZE:
        print(f"ERROR: Application size ({len(app_data)} bytes) exceeds limit ({APP_MAX_SIZE} bytes)")
        sys.exit(1)

    # Create merged image (fill with 0xFF = erased flash)
    merged = bytearray(b'\xFF' * TOTAL_FLASH)

    # Place BL at offset 0
    merged[BL_OFFSET:BL_OFFSET + len(bl_data)] = bl_data

    # Place APP at Slot 0 offset
    merged[APP_OFFSET:APP_OFFSET + len(app_data)] = app_data

    # Write output (trimmed to last used byte)
    last_used = APP_OFFSET + len(app_data)
    output_data = bytes(merged[:last_used])

    os.makedirs(os.path.dirname(output_path) if os.path.dirname(output_path) else '.', exist_ok=True)
    with open(output_path, 'wb') as f:
        f.write(output_data)

    print(f"Merged firmware generated: {output_path}")
    print(f"  Bootloader: {len(bl_data):>6} bytes @ 0x{FLASH_BASE + BL_OFFSET:08X}")
    print(f"  Application: {len(app_data):>5} bytes @ 0x{FLASH_BASE + APP_OFFSET:08X}")
    print(f"  Total image: {len(output_data):>5} bytes")


def main():
    parser = argparse.ArgumentParser(description="Merge BL + APP into single firmware binary")
    parser.add_argument('--bl', required=True, help='Path to bootloader .bin')
    parser.add_argument('--app', required=True, help='Path to application .bin')
    parser.add_argument('--output', '-o', default='build/merged_firmware.bin',
                        help='Output merged binary path')
    args = parser.parse_args()

    if not os.path.exists(args.bl):
        print(f"ERROR: Bootloader binary not found: {args.bl}")
        sys.exit(1)
    if not os.path.exists(args.app):
        print(f"ERROR: Application binary not found: {args.app}")
        sys.exit(1)

    merge_bin(args.bl, args.app, args.output)


if __name__ == '__main__':
    main()
