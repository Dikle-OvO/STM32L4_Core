# STM32L431 OTA Firmware Update System

## Flash 分区布局

```
0x08000000 ┌─────────────────┐
           │   Bootloader    │ 16KB  (Pages 0-7)
0x08004000 ├─────────────────┤
           │   Slot 0 (APP)  │ 52KB  (Pages 8-33)
0x08011000 ├─────────────────┤
           │   Slot 1 (OTA)  │ 52KB  (Pages 34-59)
0x0801E000 ├─────────────────┤
           │   Metadata      │  8KB  (Pages 60-63)
0x08020000 └─────────────────┘
```

## 工程结构

```
STM32L4_Core/
├── Bootloader/                 # BL 独立工程 (16KB Flash)
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── STM32L431_BL.ld        # BL linker script (0x08000000, 16KB)
│   ├── Inc/
│   │   ├── bl_core.h          # 跳转/swap/验证
│   │   └── bl_meta.h          # 元数据结构 (BL 本地副本)
│   └── Src/
│       ├── bl_main.c          # BL 入口 (HSI 16MHz)
│       ├── bl_core.c          # 状态机 + swap + jump
│       └── bl_meta.c          # 元数据读写 (BL 本地副本)
├── Components/OTA/             # 共享 OTA 组件
│   ├── flash_port.h / .c      # Layer 1: Flash 硬件抽象
│   ├── bl_meta.h / .c         # Layer 2: 元数据管理 (APP 侧)
│   ├── ota_core.h / .c        # Layer 3: OTA 业务逻辑
│   └── ota_proto.h / .c       # Layer 4: UART 传输协议
├── Core/                       # APP 代码 (CubeMX 生成 + 用户代码)
├── Drivers/                    # HAL 驱动 (BL 和 APP 共享)
├── scripts/
│   ├── build_all.ps1          # 一键全量构建脚本
│   └── merge_hex.py           # BL + APP 合并工具
├── STM32L431XX_FLASH.ld       # APP linker script (0x08004000, 52KB)
└── CMakeLists.txt             # APP 构建配置
```

## 构建

### 前置条件

- `arm-none-eabi-gcc` 工具链 (已加入 PATH)
- CMake ≥ 3.22
- Ninja
- Python 3

### 诊断环境

在新电脑上构建前，先检查环境：

```powershell
.\scripts\diagnose.ps1
```

如果有缺失的工具，按提示安装即可。

### 一键构建

```powershell
.\scripts\build_all.ps1          # 构建 BL + APP + 合并
.\scripts\build_all.ps1 -Clean   # 清理后重新构建
```

### 分步构建

```powershell
# 1. 构建 Bootloader
cd Bootloader
cmake --preset Release
cmake --build build_bl

# 2. 构建 Application
cd ..
cmake --preset Release
cmake --build build/Release

# 3. 合并固件
python scripts/merge_hex.py \
    --bl Bootloader/build_bl/STM32L431_BL.bin \
    --app build/Release/STM32L431CBT6.bin \
    -o build/merged_firmware.bin
```

### 构建产物

| 文件 | 说明 |
|------|------|
| `Bootloader/build_bl/STM32L431_BL.bin` | Bootloader 固件 |
| `build/Release/STM32L431CBT6.bin` | APP 固件 (OTA 升级包原始数据) |
| `build/merged_firmware.bin` | 合并固件 (首次烧录用) |

## 烧录

### 首次烧录 (BL + APP)

使用 STM32CubeProgrammer 或 ST-Link：

```powershell
# 烧录合并固件到 0x08000000
STM32_Programmer_CLI -c port=SWD -w build/merged_firmware.bin 0x08000000 -v -rst
```

### OTA 升级 (仅 APP)

```powershell
# TODO: 使用 Host 侧 OTA 工具通过 UART 发送 APP 固件
python scripts/ota_send.py --port COM3 --firmware build/Release/STM32L431CBT6.bin
```

## OTA 升级流程

```
Host                              MCU (APP)                    MCU (BL)
 │                                  │                            │
 │──CMD_OTA_START(header)──────────>│                            │
 │<─────────────────────ACK OK──────│                            │
 │                                  │ 擦除 Slot1                 │
 │──CMD_OTA_DATA(chunk 0)─────────>│                            │
 │<─────────────────────ACK OK──────│ 写入 Slot1                 │
 │──CMD_OTA_DATA(chunk 1)─────────>│                            │
 │<─────────────────────ACK OK──────│                            │
 │          ...                     │                            │
 │──CMD_OTA_END────────────────────>│                            │
 │<─────────────────────ACK OK──────│ CRC 校验通过               │
 │──CMD_OTA_REBOOT─────────────────>│                            │
 │<─────────────────────ACK OK──────│                            │
 │                                  │ 写 SWAP 标记               │
 │                                  │ NVIC_SystemReset()         │
 │                                  │              ┌─────────────│
 │                                  │              │ 读 metadata  │
 │                                  │              │ Slot1→Slot0  │
 │                                  │              │ 标记 TESTING  │
 │                                  │<─────────────┘ Jump to APP │
 │                                  │                            │
 │                                  │ ota_confirm_app()          │
 │                                  │ TESTING → NORMAL           │
```

## 协议帧格式

```
[0xAA][CMD][SEQ][LEN_L][LEN_H][PAYLOAD...][CRC16_L][CRC16_H]
```

- CRC-16/MODBUS，覆盖 CMD ~ PAYLOAD
- SEQ: 0-255 循环，用于数据包顺序校验
- 最大 payload: 1024 bytes

## 故障排查

### 构建失败

**问题**：`cmake : Build type: Release` 相关错误

**原因**：环境变量或工具链未正确配置

**解决**：
1. 运行诊断脚本检查环境
   ```powershell
   .\scripts\diagnose.ps1
   ```
2. 确保 PATH 中包含：
   - `arm-none-eabi-gcc` 工具链路径
   - CMake 安装路径
   - Ninja 安装路径
3. 如果使用 STM32CubeCLT，需要先运行其环境初始化脚本

**Windows PATH 设置**（Win+R → sysdm.cpl）：
```
C:\GNU-tools-for-STM32\bin
C:\Program Files\CMake\bin
C:\path\to\ninja
```

### 启动死机

**问题**：烧录后系统无响应

**原因**：VTOR（中断向量表）配置不对

**解决**：已在 `Core/Src/system_stm32l4xx.c` 中修复，确保：
```c
#define USER_VECT_TAB_ADDRESS
#define VECT_TAB_OFFSET  0x00004000U   // APP 的偏移
```

重新构建并烧录。

### 调试连接失败

**问题**：IDE 无法 attach 调试

**解决**：
1. 确认 ST-Link 驱动已安装
2. 检查 USB 连接
3. 使用 STM32CubeProgrammer 验证连接是否正常
4. IDE 中选择正确的 `.elf` 文件（`Bootloader/build_bl/STM32L431_BL.elf` 或 `build/Release/STM32L431CBT6.elf`）
