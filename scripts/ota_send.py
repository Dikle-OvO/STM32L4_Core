#!/usr/bin/env python3
"""
ota_send.py - OTA Firmware Update Tool (Host Side)

Layer 5: 打包固件头 + 通过 UART 发送到 MCU

Usage:
    python scripts/ota_send.py --port COM3 --firmware build/Release/STM32L431CBT6.bin
    python scripts/ota_send.py --port COM3 --firmware app.bin --version 0x00020000
    python scripts/ota_send.py --port COM3 --query       # 查询当前状态和版本
"""

import argparse
import struct
import sys
import os
import time
import serial

# =============================================================================
# Protocol constants (must match ota_proto.h)
# =============================================================================
PROTO_SOF = 0xAA
PROTO_MAX_PAYLOAD = 1024
CMD_OTA_START   = 0x01
CMD_OTA_DATA    = 0x02
CMD_OTA_END     = 0x03
CMD_OTA_ABORT   = 0x04
CMD_OTA_REBOOT  = 0x05
CMD_QUERY_STATUS  = 0x10
CMD_QUERY_VERSION = 0x11
CMD_ACK_MASK = 0x80

ACK_OK          = 0x00
ACK_ERR_CRC     = 0x01
ACK_ERR_SEQ     = 0x02
ACK_ERR_CMD     = 0x03
ACK_ERR_STATE   = 0x04
ACK_ERR_FLASH   = 0x05
ACK_ERR_VERIFY  = 0x06
ACK_ERR_SIZE    = 0x07

ACK_NAMES = {
    ACK_OK: "OK",
    ACK_ERR_CRC: "CRC Error",
    ACK_ERR_SEQ: "Sequence Error",
    ACK_ERR_CMD: "Unknown Command",
    ACK_ERR_STATE: "State Error",
    ACK_ERR_FLASH: "Flash Error",
    ACK_ERR_VERIFY: "Verify Error",
    ACK_ERR_SIZE: "Size Error",
}

OTA_HEADER_MAGIC = 0x4F544148  # "OTAH"
OTA_HEADER_FMT = "<IIIII"      # magic, version, size, fw_crc, hdr_crc
OTA_HEADER_SIZE = struct.calcsize(OTA_HEADER_FMT)

# OTA states
OTA_STATE_NAMES = {
    0: "IDLE",
    1: "HEADER",
    2: "RECEIVING",
    3: "VERIFYING",
    4: "DONE",
    5: "ERROR",
}

# Default chunk size for data transfer
DEFAULT_CHUNK_SIZE = 1024
DEFAULT_BAUD = 115200
DEFAULT_TIMEOUT = 3.0
MAX_RETRIES = 3


# =============================================================================
# CRC-16/MODBUS
# =============================================================================
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


# =============================================================================
# CRC-32 (standard Ethernet, matches MCU side)
# =============================================================================
def crc32_calc(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
    return crc ^ 0xFFFFFFFF


# =============================================================================
# Frame builder / parser
# =============================================================================
def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """Build a protocol frame: [SOF][CMD][SEQ][LEN_L][LEN_H][PAYLOAD][CRC16_L][CRC16_H]"""
    length = len(payload)
    header = bytes([PROTO_SOF, cmd, seq, length & 0xFF, (length >> 8) & 0xFF])
    # CRC covers CMD+SEQ+LEN+PAYLOAD (skip SOF)
    crc_data = header[1:] + payload
    crc = crc16_modbus(crc_data)
    return header + payload + struct.pack("<H", crc)


def parse_ack(data: bytes):
    """Parse an ACK frame, return (cmd, seq, status, payload) or None on error."""
    if len(data) < 7:  # minimum: SOF+CMD+SEQ+LEN(2)+status(1)+CRC(2)
        return None

    # Find SOF
    sof_idx = data.find(bytes([PROTO_SOF]))
    if sof_idx < 0:
        return None
    data = data[sof_idx:]

    if len(data) < 5:
        return None

    cmd = data[1]
    seq = data[2]
    payload_len = data[3] | (data[4] << 8)

    expected_len = 5 + payload_len + 2
    if len(data) < expected_len:
        return None

    payload = data[5:5 + payload_len]
    rx_crc = struct.unpack("<H", data[5 + payload_len:5 + payload_len + 2])[0]

    # Verify CRC
    calc_crc = crc16_modbus(data[1:5 + payload_len])
    if rx_crc != calc_crc:
        return None

    status = payload[0] if payload_len > 0 else 0xFF
    extra = payload[1:] if payload_len > 1 else b""

    return cmd, seq, status, extra


# =============================================================================
# OTA Client
# =============================================================================
class OTAClient:
    def __init__(self, port: str, baud: int = DEFAULT_BAUD, timeout: float = DEFAULT_TIMEOUT):
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.seq = 0
        # Flush any stale data
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()

    def _next_seq(self) -> int:
        s = self.seq & 0xFF
        self.seq += 1
        return s

    def send_cmd(self, cmd: int, payload: bytes = b"", seq: int = None) -> tuple:
        """Send command and wait for ACK. Returns (status, extra_payload)."""
        if seq is None:
            seq = self._next_seq()

        frame = build_frame(cmd, seq, payload)

        for attempt in range(MAX_RETRIES):
            self.ser.reset_input_buffer()
            self.ser.write(frame)
            self.ser.flush()

            # Read response (max frame size)
            response = self.ser.read(5 + PROTO_MAX_PAYLOAD + 2)
            if not response:
                if attempt < MAX_RETRIES - 1:
                    print(f"  Timeout, retry {attempt + 1}/{MAX_RETRIES}...")
                    continue
                raise TimeoutError(f"No response after {MAX_RETRIES} attempts")

            result = parse_ack(response)
            if result is None:
                if attempt < MAX_RETRIES - 1:
                    print(f"  Invalid response, retry {attempt + 1}/{MAX_RETRIES}...")
                    continue
                raise ValueError("Invalid ACK frame after retries")

            ack_cmd, ack_seq, status, extra = result

            if ack_cmd != (cmd | CMD_ACK_MASK):
                raise ValueError(f"Unexpected ACK cmd: 0x{ack_cmd:02X}")

            return status, extra

        raise RuntimeError("Send failed")

    def query_status(self):
        """Query MCU OTA status."""
        status, extra = self.send_cmd(CMD_QUERY_STATUS)
        if status != ACK_OK:
            print(f"Query failed: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return
        if len(extra) >= 2:
            ota_state = extra[0]
            progress = extra[1]
            print(f"OTA State: {OTA_STATE_NAMES.get(ota_state, f'Unknown({ota_state})')}")
            print(f"Progress:  {progress}%")
        else:
            print("OK (no detail)")

    def query_version(self):
        """Query MCU firmware version."""
        status, extra = self.send_cmd(CMD_QUERY_VERSION)
        if status != ACK_OK:
            print(f"Query failed: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return
        if len(extra) >= 4:
            ver = struct.unpack("<I", extra[:4])[0]
            major = (ver >> 16) & 0xFF
            minor = (ver >> 8) & 0xFF
            patch = ver & 0xFF
            print(f"Firmware version: v{major}.{minor}.{patch} (0x{ver:08X})")
        else:
            print("OK (no version data)")

    def send_firmware(self, fw_data: bytes, fw_version: int, chunk_size: int = DEFAULT_CHUNK_SIZE):
        """Execute full OTA upgrade sequence."""
        fw_size = len(fw_data)
        fw_crc = crc32_calc(fw_data)

        print(f"Firmware: {fw_size} bytes, CRC32=0x{fw_crc:08X}, version=0x{fw_version:08X}")

        # --- Step 1: Build and send OTA header ---
        print("\n[1/4] Sending OTA header...")
        hdr_fields = (OTA_HEADER_MAGIC, fw_version, fw_size, fw_crc)
        hdr_partial = struct.pack("<IIII", *hdr_fields)
        hdr_crc = crc32_calc(hdr_partial)
        header_payload = struct.pack(OTA_HEADER_FMT, *hdr_fields, hdr_crc)

        status, _ = self.send_cmd(CMD_OTA_START, header_payload)
        if status != ACK_OK:
            print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return False
        print("  OK - Slot1 erased, ready to receive")

        # --- Step 2: Send data chunks ---
        print(f"\n[2/4] Sending data ({fw_size} bytes in {chunk_size}-byte chunks)...")
        offset = 0
        total_chunks = (fw_size + chunk_size - 1) // chunk_size

        while offset < fw_size:
            chunk = fw_data[offset:offset + chunk_size]
            chunk_num = offset // chunk_size + 1

            status, _ = self.send_cmd(CMD_OTA_DATA, chunk)
            if status != ACK_OK:
                print(f"\n  FAILED at chunk {chunk_num}: {ACK_NAMES.get(status, f'0x{status:02X}')}")
                return False

            offset += len(chunk)
            pct = (offset * 100) // fw_size
            bar_len = 40
            filled = bar_len * offset // fw_size
            bar = "█" * filled + "░" * (bar_len - filled)
            print(f"\r  [{bar}] {pct:3d}% ({chunk_num}/{total_chunks})", end="", flush=True)

        print()  # newline after progress bar

        # --- Step 3: End transfer ---
        print("\n[3/4] Finishing transfer...")
        status, _ = self.send_cmd(CMD_OTA_END)
        if status != ACK_OK:
            print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return False
        print("  OK - CRC verified")

        # --- Step 4: Commit and reboot ---
        print("\n[4/4] Committing and rebooting...")
        try:
            status, _ = self.send_cmd(CMD_OTA_REBOOT)
            if status != ACK_OK:
                print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
                return False
        except TimeoutError:
            # Expected: MCU reboots before sending ACK sometimes
            pass
        print("  OK - MCU is rebooting")

        print("\n✓ OTA upgrade complete!")
        return True


# =============================================================================
# Main
# =============================================================================
def main():
    parser = argparse.ArgumentParser(
        description="STM32L431 OTA Firmware Update Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s --port COM3 --firmware build/Release/STM32L431CBT6.bin
  %(prog)s --port COM3 --firmware app.bin --version 0x00020000
  %(prog)s --port COM3 --query
  %(prog)s --port COM3 --abort
        """,
    )
    parser.add_argument("--port", "-p", required=True, help="Serial port (e.g. COM3, /dev/ttyUSB0)")
    parser.add_argument("--baud", "-b", type=int, default=DEFAULT_BAUD, help=f"Baud rate (default: {DEFAULT_BAUD})")
    parser.add_argument("--firmware", "-f", help="Path to firmware .bin file")
    parser.add_argument("--version", "-v", default="0x00010000",
                        help="Firmware version as hex (default: 0x00010000 = v1.0.0)")
    parser.add_argument("--chunk", "-c", type=int, default=DEFAULT_CHUNK_SIZE,
                        help=f"Chunk size in bytes (default: {DEFAULT_CHUNK_SIZE})")
    parser.add_argument("--query", "-q", action="store_true", help="Query MCU status and version")
    parser.add_argument("--abort", action="store_true", help="Abort ongoing OTA")
    parser.add_argument("--timeout", "-t", type=float, default=DEFAULT_TIMEOUT,
                        help=f"Response timeout in seconds (default: {DEFAULT_TIMEOUT})")

    args = parser.parse_args()

    print(f"Opening {args.port} @ {args.baud} baud...")
    try:
        client = OTAClient(args.port, args.baud, args.timeout)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {args.port}: {e}")
        sys.exit(1)

    try:
        if args.query:
            client.query_version()
            client.query_status()

        elif args.abort:
            print("Aborting OTA...")
            status, _ = client.send_cmd(CMD_OTA_ABORT)
            print(f"Result: {ACK_NAMES.get(status, f'0x{status:02X}')}")

        elif args.firmware:
            if not os.path.exists(args.firmware):
                print(f"ERROR: Firmware file not found: {args.firmware}")
                sys.exit(1)

            with open(args.firmware, "rb") as f:
                fw_data = f.read()

            if len(fw_data) == 0:
                print("ERROR: Firmware file is empty")
                sys.exit(1)

            fw_version = int(args.version, 0)
            success = client.send_firmware(fw_data, fw_version, args.chunk)
            if not success:
                sys.exit(1)
        else:
            parser.print_help()
    except (TimeoutError, ValueError) as e:
        print(f"\nERROR: {e}")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n\nAborted by user, sending abort command...")
        try:
            client.send_cmd(CMD_OTA_ABORT)
        except Exception:
            pass
        sys.exit(1)
    finally:
        client.close()


if __name__ == "__main__":
    main()
