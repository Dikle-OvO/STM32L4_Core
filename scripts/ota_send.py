#!/usr/bin/env python3
"""
ota_send.py - OTA 固件升级工具 (上位机/PC端)

本脚本是 STM32L4 系列 MCU 的 OTA (Over-The-Air，这里实际是通过串口)
固件升级工具的上位机端。它实现了完整的升级协议：

  ┌─────────────┐         UART          ┌──────────────┐
  │  PC 上位机  │ ◄══════════════════► │  MCU 目标板  │
  │  (本脚本)   │    自定义协议帧       │  (ota_proto) │
  └─────────────┘                      └──────────────┘

协议分层 (Layer 5):
  应用层   —— 打包固件头、拆分数据块、管理升级流程
  传输层   —— 命令/应答帧 组装与解析、CRC 校验
  数据链路层 —— 串口 UART 通信

用法示例:
  # 发送固件升级
  python scripts/ota_send.py --port COM3 --firmware build/Release/STM32L431CBT6.bin

  # 指定固件版本号
  python scripts/ota_send.py --port COM3 --firmware app.bin --version 0x00020000

  # 查询 MCU 当前状态和版本
  python scripts/ota_send.py --port COM3 --query
"""

import argparse   # 命令行参数解析
import struct     # 二进制数据的打包/解包（CRC32 原始字节 → 结构体）
import sys        # 错误退出 (sys.exit)
import os         # 检查固件文件是否存在
import time       # （保留，未直接使用，可用于延迟等待）
import serial     # pySerial 库 —— 通过串口与 MCU 通信

# =============================================================================
# 协议常量 —— 必须与 MCU 端 ota_proto.h 保持一致
#                如果 MCU 端修改了这些值，需要同步更新
# =============================================================================

# ── 帧结构 ──
PROTO_SOF = 0xAA              # 帧起始标识 (Start Of Frame)
                               # 每个数据帧都以 0xAA 开头，接收方通过它来定位帧边界
                               # 帧格式：[SOF(1)][CMD(1)][SEQ(1)][LEN_L(1)][LEN_H(1)][PAYLOAD(N)][CRC16_L(1)][CRC16_H(1)]
                               # 最小帧长 = 1 + 1 + 1 + 1 + 1 + 0 + 2 = 7 字节
                               # 最大帧长 = 7 + 1024 = 1031 字节

PROTO_MAX_PAYLOAD = 1024      # 单帧最大负载字节数
                               # 限制原因：
                               #   1. MCU 端接收缓冲区有限
                               #   2. 单帧太大 → CRC 校验失败的代价高（需重传整个大帧）
                               #   3. 1024 与 STM32 内部 Flash 写入页大小对齐

# ── 命令码（低 7 位有效）──
CMD_OTA_START       = 0x01    # 开始 OTA 升级 → 携带固件头信息
CMD_OTA_DATA        = 0x02    # 传输固件数据块 → payload 为固件原始字节
CMD_OTA_END         = 0x03    # 结束数据传输 → MCU 进行 CRC32 整体校验
CMD_OTA_ABORT       = 0x04    # 中止 OTA → MCU 回到 IDLE 状态
CMD_OTA_REBOOT      = 0x05    # 请求 MCU 重启 → 切换到新固件
CMD_QUERY_STATUS    = 0x10    # 查询状态 → 返回 OTA 状态机状态 + 进度
CMD_QUERY_VERSION   = 0x11    # 查询版本 → 返回当前固件版本号
CMD_ACK_MASK        = 0x80    # ACK 标志位：MCU 应答时，cmd |= 0x80
                               # 上位机命令：bit7=0 (0x01~0x7F)
                               # MCU  应答：bit7=1 (0x81~0xFF)

# ── ACK 状态码 ──
ACK_OK              = 0x00    # 操作成功
ACK_ERR_CRC         = 0x01    # CRC16 校验失败 → 帧数据在传输中损坏
ACK_ERR_SEQ         = 0x02    # 序列号异常 → 丢失帧或重复帧
ACK_ERR_CMD         = 0x03    # 未知命令 → MCU 不理解该命令码
ACK_ERR_STATE       = 0x04    # 状态错误 → 当前 OTA 状态下不允许此命令
ACK_ERR_FLASH       = 0x05    # Flash 操作错误 → 擦除/写入失败（可能是坏块）
ACK_ERR_VERIFY      = 0x06    # 校验错误 → 固件 CRC32 与头中不一致
ACK_ERR_SIZE        = 0x07    # 大小错误 → 固件超过预留 Slot 空间

# 状态码 → 可读名称映射表（用于日志输出）
ACK_NAMES = {
    ACK_OK:                  "OK",
    ACK_ERR_CRC:             "CRC Error",      # 帧级 CRC16 校验失败
    ACK_ERR_SEQ:             "Sequence Error",  # 序列号不连续
    ACK_ERR_CMD:             "Unknown Command", # 命令码未定义
    ACK_ERR_STATE:           "State Error",     # 状态机不允许此操作
    ACK_ERR_FLASH:           "Flash Error",     # Flash 硬件故障
    ACK_ERR_VERIFY:          "Verify Error",    # 固件整体 CRC32 校验失败
    ACK_ERR_SIZE:            "Size Error",      # 固件太大放不下
}

# ── OTA 固件头 ──
OTA_HEADER_MAGIC = 0x4F544148   # 魔数 "OTAH" 的 ASCII 表示（小端序）
                                 # 'O'=0x4F, 'T'=0x54, 'A'=0x41, 'H'=0x48
                                 # 作用：让 MCU 区分原始数据帧和固件头帧
                                 # 如果魔数不匹配，MCU 拒绝进入升级流程

OTA_HEADER_FMT    = "<IIIII"    # 固件头结构体格式（小端序）
                                 #   I[0] = magic     - 魔数 0x4F544148
                                 #   I[1] = version   - 固件版本号 (e.g. 0x00010000 = v1.0.0)
                                 #   I[2] = size      - 固件大小（字节）
                                 #   I[3] = fw_crc    - 固件数据的 CRC32 校验值
                                 #   I[4] = hdr_crc   - 前 4 个字段的 CRC32 校验值

OTA_HEADER_SIZE    = struct.calcsize(OTA_HEADER_FMT)  # = 20 字节 (5 × 4)

# ── MCU OTA 状态机状态码 ──
OTA_STATE_NAMES = {
    0: "IDLE",        # 空闲 —— 等待升级命令
    1: "HEADER",      # 已收到固件头 —— 等待数据块
    2: "RECEIVING",   # 正在接收数据 —— 逐个写入 Flash
    3: "VERIFYING",   # 正在校验 —— 验证 CRC32
    4: "DONE",        # 完成 —— 数据写入成功，等待 Reboot 命令
    5: "ERROR",       # 错误 —— 升级过程中出现异常
}

# ── 数据传输参数 ──
DEFAULT_CHUNK_SIZE = 1024         # 默认每块大小 (bytes)
                                   # = PROTO_MAX_PAYLOAD，一帧塞满一个 chunk
                                   # 改小 → 帧数变多，协议开销变大
                                   # 改大 → 帧数变少，但单帧出错重传代价大
DEFAULT_BAUD       = 921600       # 默认波特率
                                   # STM32L4 USART1 fractional divider 可精确匹配
                                   # 921600 bps ≈ 112 KB/s，OTA 52KB 固件约 0.5s 传输
DEFAULT_TIMEOUT    = 3.0          # 默认应答超时 (秒)
MAX_RETRIES        = 3            # 最大重试次数
                                   # 每帧最多发送 3 次：首次 + 2 次重试


# =============================================================================
# CRC-16/MODBUS —— 帧校验
# =============================================================================
def crc16_modbus(data: bytes) -> int:
    """
    计算 CRC-16/MODBUS 校验值。

    CRC-16 用于每帧的完整性校验，覆盖范围：
      [CMD][SEQ][LEN_L][LEN_H][PAYLOAD...]
      即跳过帧头 SOF 字节。

    算法特点：
      - 多项式: 0x8005 (反转 = 0xA001)
      - 初始值: 0xFFFF
      - 位序:   低位先出 (LSB first)
      - 最终异或: 无

    此函数必须与 MCU 端保持完全一致，
    否则 MCU 校验通过的帧、上位机校验失败，反之亦然。

    参数:
        data: 待计算 CRC 的原始字节串
    返回:
        16-bit CRC 校验值（0x0000 ~ 0xFFFF）
    """
    crc = 0xFFFF                             # 初始化为全 1
    for byte in data:                        # 逐字节处理
        crc ^= byte                          # 将当前字节异或到 CRC 低 8 位
        for _ in range(8):                   # 逐位处理（每个字节 8 位）
            if crc & 1:                      # 如果最低位为 1
                crc = (crc >> 1) ^ 0xA001    #   右移一位后异或多项式反转值
            else:                            # 如果最低位为 0
                crc >>= 1                    #   仅右移一位
    return crc                               # 返回 16-bit 结果


# =============================================================================
# CRC-32 (Ethernet 标准) —— 固件完整性校验
# =============================================================================
def crc32_calc(data: bytes) -> int:
    """
    计算 CRC-32 校验值（以太网标准 / PKZIP 兼容）。

    CRC-32 用于整个固件的完整性校验，不参与每帧校验。
    校验值被打包在 OTA 固件头中，MCU 在接收完成后重新计算并比对。

    算法特点：
      - 多项式: 0x04C11DB7 (反转 = 0xEDB88320)
      - 初始值: 0xFFFFFFFF
      - 位序:   低位先出 (LSB first)
      - 最终异或: 0xFFFFFFFF (反射输出)

    参数:
        data: 整个固件的字节数据
    返回:
        32-bit CRC 校验值
    """
    crc = 0xFFFFFFFF                         # 初始化为全 1
    for byte in data:
        crc ^= byte                          # 异或当前字节到低 8 位
        for _ in range(8):                   # 逐位处理
            if crc & 1:                      # 最低位为 1
                crc = (crc >> 1) ^ 0xEDB88320  # 右移 + 多项式反转
            else:
                crc >>= 1                    # 仅右移
    return crc ^ 0xFFFFFFFF                  # 最终异或（反射）


# =============================================================================
# 协议帧 构建 / 解析
# =============================================================================
def build_frame(cmd: int, seq: int, payload: bytes = b"") -> bytes:
    """
    构建一个协议帧（上位机 → MCU）。

    帧格式（总长度 = 5 + N + 2 字节）：
      ┌─────┬─────┬─────┬───────┬───────┬──────────┬─────────┬─────────┐
      │ SOF │ CMD │ SEQ │ LEN_L │ LEN_H │ PAYLOAD  │ CRC16_L │ CRC16_H │
      │ 1 B │ 1 B │ 1 B │  1 B  │  1 B  │   N B    │   1 B   │   1 B   │
      └─────┴─────┴─────┴───────┴───────┴──────────┴─────────┴─────────┘
       SOF=0xAA │         CRC16 覆盖范围        │
                └───────────────────────────────┘

    参数:
        cmd:     命令码（如 CMD_OTA_DATA = 0x02）
        seq:     序列号（0~255 自增，用于丢帧检测）
        payload: 帧负载字节（最大 1024 字节）

    返回:
        完整的帧字节串，可直接 write() 到串口
    """
    length = len(payload)                          # 负载长度 N

    # 组装帧头（不含 SOF 之后的 CRC 覆盖部分）
    header = bytes([
        PROTO_SOF,               # [0] 帧起始标识
        cmd,                      # [1] 命令码
        seq,                      # [2] 序列号
        length & 0xFF,            # [3] 负载长度低字节
        (length >> 8) & 0xFF,     # [4] 负载长度高字节
    ])

    # CRC16 覆盖范围: CMD + SEQ + LEN_L + LEN_H + PAYLOAD
    # 不包含 SOF，因为 SOF 可能被用作帧同步，本身不参与数据完整性检查
    crc_data = header[1:] + payload
    crc = crc16_modbus(crc_data)

    # 组装完整帧: header + payload + CRC16(2 bytes, little-endian)
    return header + payload + struct.pack("<H", crc)


def parse_ack(data: bytes):
    """
    解析 MCU 发回的 ACK 帧。

    MCU 的 ACK 帧结构与命令帧相同，但：
      - CMD 字段的 bit7 = 1（CMD | 0x80）
      - payload[0] 固定为状态码（ACK_OK / ACK_ERR_xxx）
      - payload[1:] 为附加数据（状态查询时带 OTA state 和进度）

    解析流程:
      1. 检查最小帧长 (≥ 7 字节)
      2. 搜索 SOF 字节 (0xAA) —— 处理可能的垃圾数据
      3. 提取 CMD, SEQ, payload_len
      4. 验证实际帧长 ≥ 预期帧长
      5. CRC16 校验

    参数:
        data: 从串口读取到的原始字节串（可能包含噪声或遗留数据）

    返回:
        成功: (cmd, seq, status, extra)
              - cmd:    ACK 命令码 (原始 | 0x80)
              - seq:    序列号（应与发送帧一致）
              - status: 状态码（payload[0]）
              - extra:  附加数据（payload[1:]）
        失败: None
    """
    # 最小帧长度: SOF+CMD+SEQ+LEN(2)+STATUS(1)+CRC(2) = 7 字节
    if len(data) < 7:
        return None

    # 搜索 SOF 标志 —— 跳过串口缓冲区中可能存在的残留数据
    sof_idx = data.find(bytes([PROTO_SOF]))
    if sof_idx < 0:
        return None                                     # 没有找到帧起始标志
    data = data[sof_idx:]                               # 从 SOF 开始截取

    if len(data) < 5:                                   # 连基本帧头都不够
        return None

    # 解析帧头字段
    cmd = data[1]                                       # 命令码 (bit7=1 表示 ACK)
    seq = data[2]                                       # 序列号 —— 用于确认是哪一帧的应答
    payload_len = data[3] | (data[4] << 8)              # 负载长度 (小端序 → int)

    # 计算完整帧的预期长度: header(5) + payload(N) + CRC(2)
    expected_len = 5 + payload_len + 2
    if len(data) < expected_len:                        # 实际收到的不够预期长度
        return None

    # 提取 payload 和 CRC
    payload = data[5 : 5 + payload_len]
    rx_crc = struct.unpack("<H", data[5 + payload_len : 5 + payload_len + 2])[0]

    # CRC16 校验 —— 覆盖范围与发送端一致
    calc_crc = crc16_modbus(data[1 : 5 + payload_len])  # 跳过 SOF
    if rx_crc != calc_crc:
        return None                                      # CRC 校验失败 → 数据损坏

    # 提取状态码和附加数据
    status = payload[0] if payload_len > 0 else 0xFF    # 第一个字节 = 状态码
    extra  = payload[1:] if payload_len > 1 else b""    # 剩余字节 = 附加数据

    return cmd, seq, status, extra


# =============================================================================
# OTA 客户端类 —— 封装完整的升级协议
# =============================================================================
class OTAClient:
    """
    OTA 客户端类。

    负责：
      - 打开和管理串口连接
      - 序列号管理（每帧递增，用于丢帧/重复帧检测）
      - 命令发送与应答接收（含自动重试）
      - OTA 升级四阶段流程控制
      - 状态查询与版本查询

    使用示例:
        client = OTAClient("COM6", baud=921600)
        client.send_firmware(firmware_bytes, version=0x00010000)
        client.close()
    """

    def __init__(self, port: str, baud: int = DEFAULT_BAUD, timeout: float = DEFAULT_TIMEOUT):
        """
        初始化 OTA 客户端，打开串口。

        参数:
            port:    串口名称（Windows: "COM3", Linux: "/dev/ttyUSB0"）
            baud:    波特率（默认 921600）
            timeout: 读取超时（秒）。timeout 后 read() 返回 b""，
                     脚本以此判断 MCU 无应答

        异常:
            serial.SerialException: 串口不存在或已被其他程序占用
        """
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.seq = 0                                     # 帧序列号计数器

        # 清空串口缓冲区 → 防止上一次通信的遗留数据干扰本次协议解析
        self.ser.reset_input_buffer()                    # 清空接收缓冲区
        self.ser.reset_output_buffer()                   # 清空发送缓冲区

    def close(self):
        """
        关闭串口连接。

        应在升级完成后或异常退出时调用。
        如果串口已经关闭（例如在 finally 块中重复调用），不会报错。
        """
        if self.ser and self.ser.is_open:
            self.ser.close()

    def _next_seq(self) -> int:
        """
        生成下一个帧序列号 (0~255)。

        序列号的作用：
          - MCU 通过序列号检测丢帧（seq 不连续 → ACK_ERR_SEQ）
          - MCU 通过序列号检测重复帧（seq 重复 → 可能是重传）

        每调用一次自增 1，溢出后自动回绕到 0。

        返回:
            当前序列号 (0~255)
        """
        s = self.seq & 0xFF         # 取低 8 位，模拟 uint8_t 溢出
        self.seq += 1               # 为下一帧递增
        return s

    def send_cmd(self, cmd: int, payload: bytes = b"", seq: int = None) -> tuple:
        """
        发送命令并等待 MCU 应答（含自动重试）。

        这是所有命令的底层入口，QUERY_STATUS/OTA_START/OTA_DATA 等
        都通过此方法来发送。

        流程:
          1. 构建协议帧 (build_frame)
          2. for 重试计数:
             a. 清空接收缓冲区 → 丢弃可能的旧数据
             b. 发送帧到串口
             c. 读取应答 → 超时? 重试 → 最多 MAX_RETRIES 次
             d. 解析应答帧 → 无效? 重试
             e. 验证 ACK 命令码 → 不匹配? 报错
             f. 返回 (status, extra)

        参数:
            cmd:     命令码
            payload: 负载数据
            seq:     指定序列号（None 则自动生成）

        返回:
            (status, extra): 状态码和附加数据

        异常:
            TimeoutError: 多次重试后仍无应答 → MCU 可能宕机/串口断开
            ValueError:    解析失败或 ACK 命令码异常
        """
        if seq is None:
            seq = self._next_seq()                        # 自动分配序列号

        frame = build_frame(cmd, seq, payload)            # 构建完整的协议帧

        for attempt in range(MAX_RETRIES):
            # 清空接收缓冲区 —— 丢弃上次失败可能留下的残余数据
            self.ser.reset_input_buffer()

            # 发送帧到串口
            self.ser.write(frame)
            self.ser.flush()                              # 确保数据已从 OS 缓冲区发出

            # 读取应答 —— 最多读取一个完整帧的量
            # 5 字节帧头 + 1024 字节负载 + 2 字节 CRC
            response = self.ser.read(5 + PROTO_MAX_PAYLOAD + 2)

            if not response:                              # 读取超时（缓冲区为空）
                if attempt < MAX_RETRIES - 1:
                    print(f"  Timeout, retry {attempt + 1}/{MAX_RETRIES}...")
                    continue
                raise TimeoutError(f"No response after {MAX_RETRIES} attempts")

            result = parse_ack(response)                  # 解析应答帧
            if result is None:                            # 解析失败（CRC/格式错误）
                if attempt < MAX_RETRIES - 1:
                    print(f"  Invalid response, retry {attempt + 1}/{MAX_RETRIES}...")
                    continue
                raise ValueError("Invalid ACK frame after retries")

            ack_cmd, ack_seq, status, extra = result

            # 验证 ACK 命令码: MCU 应答时会将 bit7 置 1
            # 例如 CMD_OTA_DATA(0x02) → ACK(0x82)
            if ack_cmd != (cmd | CMD_ACK_MASK):
                raise ValueError(f"Unexpected ACK cmd: 0x{ack_cmd:02X}")

            return status, extra                          # 成功获得有效应答

        raise RuntimeError("Send failed")                 # 理论上不会到达这里

    # ──────────────────────────────────────────────
    # 查询命令
    # ──────────────────────────────────────────────

    def query_status(self):
        """
        查询 MCU 当前的 OTA 状态和升级进度。

        发送 CMD_QUERY_STATUS，解析返回的：
          - ota_state: MCU 状态机当前状态 (IDLE/RECEIVING/DONE/...)
          - progress:  升级进度百分比 (0~100)

        输出示例:
          OTA State: RECEIVING
          Progress:  45%
        """
        status, extra = self.send_cmd(CMD_QUERY_STATUS)
        if status != ACK_OK:
            print(f"Query failed: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return
        if len(extra) >= 2:
            ota_state = extra[0]                          # 状态码
            progress  = extra[1]                          # 进度 0~100
            print(f"OTA State: {OTA_STATE_NAMES.get(ota_state, f'Unknown({ota_state})')}")
            print(f"Progress:  {progress}%")
        else:
            print("OK (no detail)")

    def query_version(self):
        """
        查询 MCU 当前固件版本号。

        发送 CMD_QUERY_VERSION，解析返回的 32-bit 版本号:
          bits [31:24] = 保留
          bits [23:16] = 主版本号 (Major)
          bits [15:8]  = 次版本号 (Minor)
          bits [7:0]   = 补丁版本号 (Patch)

        输出示例:
          Firmware version: v1.4.2 (0x00010402)
        """
        status, extra = self.send_cmd(CMD_QUERY_VERSION)
        if status != ACK_OK:
            print(f"Query failed: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return
        if len(extra) >= 4:
            ver = struct.unpack("<I", extra[:4])[0]       # 解析 uint32_t（小端序）
            major = (ver >> 16) & 0xFF                    # 主版本
            minor = (ver >> 8)  & 0xFF                    # 次版本
            patch =  ver        & 0xFF                    # 补丁版本
            print(f"Firmware version: v{major}.{minor}.{patch} (0x{ver:08X})")
        else:
            print("OK (no version data)")

    # ──────────────────────────────────────────────
    # OTA 升级核心流程 —— 四个阶段
    # ──────────────────────────────────────────────

    def send_firmware(self, fw_data: bytes, fw_version: int, chunk_size: int = DEFAULT_CHUNK_SIZE):
        """
        执行完整的 OTA 固件升级流程。

        ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐
        │ Step 1  │ → │ Step 2  │ → │ Step 3  │ → │ Step 4  │
        │ 发送头  │    │ 送数据块│    │ 结束传输│    │提交重启 │
        └─────────┘    └─────────┘    └─────────┘    └─────────┘
         擦除 Slot1      逐个写入        CRC32 校验     切换到新固件

        参数:
            fw_data:    固件的完整二进制数据 (bytes)
            fw_version: 固件版本号 (e.g. 0x00010000 = v1.0.0)
            chunk_size: 每块大小（默认 1024 字节）

        返回:
            True:  升级成功
            False: 升级失败（具体错误信息会打印到控制台）
        """
        fw_size = len(fw_data)
        fw_crc  = crc32_calc(fw_data)                    # 计算整个固件的 CRC32

        print(f"Firmware: {fw_size} bytes, CRC32=0x{fw_crc:08X}, version=0x{fw_version:08X}")

        # ═══════════════════════════════════════════════════════════
        # 阶段 1/4: 发送 OTA 固件头
        # ═══════════════════════════════════════════════════════════
        #
        # 固件头结构 (20 字节):
        #   ┌────────────┬──────────────┐
        #   │ magic      │ 魔数 "OTAH"   │ ← 验证这是一个有效的 OTA 头
        #   │ version    │ 固件版本号     │ ← 防止版本回退
        #   │ size       │ 固件大小       │ ← MCU 检查是否超出预留空间
        #   │ fw_crc     │ 固件 CRC32     │ ← 传输完成后校验
        #   │ hdr_crc    │ 头部 CRC32     │ ← 防止头本身损坏
        #   └────────────┴──────────────┘
        #
        # MCU 收到后:
        #   1. 验证 magic == "OTAH"
        #   2. 验证 hdr_crc → 头本身完整
        #   3. 检查 size ≤ Slot1 容量
        #   4. 擦除 Slot1 所在 Flash 区域
        #   5. 状态机 → HEADER
        print("\n[1/4] Sending OTA header...")

        # 打包前 4 个字段（不含 hdr_crc），用于计算 hdr_crc
        hdr_fields = (OTA_HEADER_MAGIC, fw_version, fw_size, fw_crc)
        hdr_partial = struct.pack("<IIII", *hdr_fields)    # 16 字节：magic+version+size+fw_crc
        hdr_crc = crc32_calc(hdr_partial)                  # 计算头的 CRC32

        # 打包完整头: 前 4 字段 + hdr_crc = 20 字节
        header_payload = struct.pack(OTA_HEADER_FMT, *hdr_fields, hdr_crc)

        status, _ = self.send_cmd(CMD_OTA_START, header_payload)
        if status != ACK_OK:
            print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return False
        print("  OK - Slot1 erased, ready to receive")

        # ═══════════════════════════════════════════════════════════
        # 阶段 2/4: 发送固件数据块
        # ═══════════════════════════════════════════════════════════
        #
        # 将固件按 chunk_size 切分为多个数据块，逐块发送。
        #
        # 每块流程:
        #   上位机                            MCU
        #     │── CMD_OTA_DATA + payload ──→ │
        #     │                               │ 1. CRC16 校验帧
        #     │                               │ 2. 检查 seq 连续性
        #     │                               │ 3. 写入 Flash
        #     │                               │ 4. 状态机 → RECEIVING
        #     │←──── ACK (status) ────────── │
        #
        # chunk_size 的选择:
        #   - 1024 (默认): 与 Flash 页大小对齐，效率最优
        #   - 512:        适合不稳定串口链路
        #   - 256:        最小推荐值，协议开销变大
        print(f"\n[2/4] Sending data ({fw_size} bytes in {chunk_size}-byte chunks)...")
        offset = 0                                          # 当前已发送字节数
        total_chunks = (fw_size + chunk_size - 1) // chunk_size  # 总块数（向上取整）

        while offset < fw_size:
            # 切取当前块（最后一块可能小于 chunk_size）
            chunk = fw_data[offset : offset + chunk_size]
            chunk_num = offset // chunk_size + 1             # 当前块编号 (1-based)

            # 发送数据块 → MCU 写入 Flash
            status, _ = self.send_cmd(CMD_OTA_DATA, chunk)
            if status != ACK_OK:
                print(f"\n  FAILED at chunk {chunk_num}: {ACK_NAMES.get(status, f'0x{status:02X}')}")
                return False

            offset += len(chunk)
            pct = (offset * 100) // fw_size                  # 百分比

            # 绘制进度条
            bar_len = 40                                     # 进度条宽度（字符数）
            filled = bar_len * offset // fw_size
            bar = "█" * filled + "░" * (bar_len - filled)
            print(f"\r  [{bar}] {pct:3d}% ({chunk_num}/{total_chunks})",
                  end="", flush=True)                        # \r 回行首 → 原地刷新

        print()  # 换行，结束进度条行

        # ═══════════════════════════════════════════════════════════
        # 阶段 3/4: 结束数据传输
        # ═══════════════════════════════════════════════════════════
        #
        # MCU 收到 CMD_OTA_END 后:
        #   1. 停止接收新数据
        #   2. 重新计算已写入 Flash 数据的 CRC32
        #   3. 与固件头中的 fw_crc 比对
        #   4. 一致 → ACK_OK, 状态机 → DONE
        #      不一致 → ACK_ERR_VERIFY, 状态机 → ERROR
        print("\n[3/4] Finishing transfer...")
        status, _ = self.send_cmd(CMD_OTA_END)
        if status != ACK_OK:
            print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
            return False
        print("  OK - CRC verified")

        # ═══════════════════════════════════════════════════════════
        # 阶段 4/4: 提交重启
        # ═══════════════════════════════════════════════════════════
        #
        # MCU 收到 CMD_OTA_REBOOT 后:
        #   1. 将 Slot1 标记为有效（Magic + Flag 写入 Flash）
        #   2. 设置启动标志 → 下次上电从 Slot1 启动
        #   3. 执行软件复位 (NVIC_SystemReset)
        #
        # 可能触发 TimeoutError:
        #   MCU 在发送 ACK 之前就执行了复位，导致上位机收不到应答。
        #   这是正常现象 → 捕获异常，视为成功。
        print("\n[4/4] Committing and rebooting...")
        try:
            status, _ = self.send_cmd(CMD_OTA_REBOOT)
            if status != ACK_OK:
                print(f"  FAILED: {ACK_NAMES.get(status, f'0x{status:02X}')}")
                return False
        except TimeoutError:
            # 预期行为: MCU 可能在发送 ACK 前就复位了
            pass
        print("  OK - MCU is rebooting")

        print("\n✓ OTA upgrade complete!")
        return True


# =============================================================================
# 命令行入口
# =============================================================================
def main():
    """
    脚本入口函数。

    支持以下操作模式:
      --query:        查询模式 → 查询 MCU 状态和版本
      --abort:        中止模式 → 中止正在进行的 OTA
      --firmware:     升级模式 → 发送固件文件进行 OTA 升级
      (无操作参数):   打印帮助信息
    """
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

    # ── 必需参数 ──
    parser.add_argument("--port", "-p",
                        required=True,
                        help="Serial port (e.g. COM3, /dev/ttyUSB0)")

    # ── 串口参数 ──
    parser.add_argument("--baud", "-b",
                        type=int, default=DEFAULT_BAUD,
                        help=f"Baud rate (default: {DEFAULT_BAUD})")

    parser.add_argument("--timeout", "-t",
                        type=float, default=DEFAULT_TIMEOUT,
                        help=f"Response timeout in seconds (default: {DEFAULT_TIMEOUT})")

    # ── 升级参数 ──
    parser.add_argument("--firmware", "-f",
                        help="Path to firmware .bin file")

    parser.add_argument("--version", "-v",
                        default="0x00010000",
                        help="Firmware version as hex (default: 0x00010000 = v1.0.0)")

    parser.add_argument("--chunk", "-c",
                        type=int, default=DEFAULT_CHUNK_SIZE,
                        help=f"Chunk size in bytes (default: {DEFAULT_CHUNK_SIZE})")

    # ── 操作模式 ──
    parser.add_argument("--query", "-q",
                        action="store_true",
                        help="Query MCU status and version")

    parser.add_argument("--abort",
                        action="store_true",
                        help="Abort ongoing OTA")

    args = parser.parse_args()

    # ── 打开串口 ──
    print(f"Opening {args.port} @ {args.baud} baud...")
    try:
        client = OTAClient(args.port, args.baud, args.timeout)
    except serial.SerialException as e:
        # 常见原因: 串口不存在、被占用、权限不足
        print(f"ERROR: Cannot open {args.port}: {e}")
        sys.exit(1)

    try:
        # ── 模式 1: 查询状态 ──
        if args.query:
            client.query_version()
            client.query_status()

        # ── 模式 2: 中止 OTA ──
        elif args.abort:
            print("Aborting OTA...")
            status, _ = client.send_cmd(CMD_OTA_ABORT)
            print(f"Result: {ACK_NAMES.get(status, f'0x{status:02X}')}")

        # ── 模式 3: OTA 升级 ──
        elif args.firmware:
            # 检查固件文件是否存在
            if not os.path.exists(args.firmware):
                print(f"ERROR: Firmware file not found: {args.firmware}")
                sys.exit(1)

            # 读取固件文件到内存
            with open(args.firmware, "rb") as f:
                fw_data = f.read()

            # 检查文件是否为空
            if len(fw_data) == 0:
                print("ERROR: Firmware file is empty")
                sys.exit(1)

            # 解析版本号（支持 0x 前缀的十六进制）
            fw_version = int(args.version, 0)

            # 执行 OTA 升级
            success = client.send_firmware(fw_data, fw_version, args.chunk)
            if not success:
                sys.exit(1)

        # ── 未指定操作 ──
        else:
            parser.print_help()

    except (TimeoutError, ValueError) as e:
        # 通信异常: 无应答 或 协议解析失败
        print(f"\nERROR: {e}")
        sys.exit(1)

    except KeyboardInterrupt:
        # 用户按 Ctrl+C → 优雅中止
        print("\n\nAborted by user, sending abort command...")
        try:
            client.send_cmd(CMD_OTA_ABORT)
        except Exception:
            pass                                     # 忽略中止过程中的异常
        sys.exit(1)

    finally:
        # 无论如何都要关闭串口，防止资源泄漏
        client.close()


# =============================================================================
# 脚本入口
# =============================================================================
if __name__ == "__main__":
    main()
