# 嵌入式 Bootloader 与 OTA 固件升级 —— 从原理到实践

> 以 STM32L431 为硬件平台，系统讲解 MCU 端 Bootloader / OTA 升级系统的完整设计与实现。
> 结合本工程的具体实现与业界标准做法，去伪存真，提炼精华。

---

## 目录

- [第一部分：基础概念](#第一部分基础概念)
  - [第1章 为什么需要 Bootloader](#第1章-为什么需要-bootloader)
  - [第2章 Flash 存储器物理特性](#第2章-flash-存储器物理特性)
  - [第3章 内存布局与分区策略](#第3章-内存布局与分区策略)
  - [第4章 Cortex-M 启动流程详解](#第4章-cortex-m-启动流程详解)
- [第二部分：Bootloader 核心设计](#第二部分bootloader-核心设计)
  - [第5章 Bootloader 的启动与自检](#第5章-bootloader-的启动与自检)
  - [第6章 镜像验证](#第6章-镜像验证)
  - [第7章 固件跳转技术](#第7章-固件跳转技术)
  - [第8章 升级状态机](#第8章-升级状态机)
  - [第9章 元数据管理](#第9章-元数据管理)
- [第三部分：OTA 升级系统](#第三部分ota-升级系统)
  - [第10章 OTA 升级全流程](#第10章-ota-升级全流程)
  - [第11章 通信协议设计](#第11章-通信协议设计)
  - [第12章 Flash 驱动抽象层](#第12章-flash-驱动抽象层)
  - [第13章 OTA 业务逻辑层](#第13章-ota-业务逻辑层)
- [第四部分：进阶主题](#第四部分进阶主题)
  - [第14章 安全启动与固件签名](#第14章-安全启动与固件签名)
  - [第15章 回滚（Rollback）机制](#第15章-回滚机制)
  - [第16章 掉电保护与看门狗](#第16章-掉电保护与看门狗)
  - [第17章 差分升级（Delta OTA）](#第17章-差分升级delta-ota)
  - [第18章 生产烧录与合并工具](#第18章-生产烧录与合并工具)
- [第五部分：本工程实战分析](#第五部分本工程实战分析)
  - [第19章 工程架构评价](#第19章-工程架构评价)
  - [第20章 改进建议清单](#第20章-改进建议清单)

---

## 第一部分：基础概念

### 第1章 为什么需要 Bootloader

#### 1.1 什么是 Bootloader

Bootloader（引导加载程序）是嵌入式设备上电后最先执行的固件。它负责：

1. **硬件最小初始化** —— 配置时钟、启动关键外设
2. **固件完整性检查** —— 验证应用程序是否有效
3. **固件更新** —— 接收新固件并写入 Flash
4. **启动决策** —— 决定引导到哪个应用程序
5. **故障恢复** —— 当应用程序损坏时提供恢复路径

你可以把 Bootloader 想象成 PC 上的 BIOS/UEFI —— 它负责硬件自检和操作系统加载，但本身不执行业务逻辑。

#### 1.2 为什么 MCU 需要 Bootloader

在没有 Bootloader 的传统开发模式中：

```
开发 → 编译 → 通过调试器（SWD/JTAG）烧录 → 运行
```

这种方式的问题：
- **必须物理连接调试器**：产品出厂后无法升级
- **没有故障保护**：如果应用程序本身崩溃，设备变砖
- **没有远程升级能力**：IoT 设备需要 OTA

引入 Bootloader 后：

```
上电 → Bootloader → [检查升级标志] → 应用程序
                        ↓
                  [需要升级]
                        ↓
              接收新固件 → 验证 → 替换 → 重启
```

#### 1.3 Bootloader 的核心设计目标

| 目标 | 说明 |
|------|------|
| **小体积** | BL 本身越小，留给 APP 的空间越多 |
| **高可靠** | BL 本身绝不能损坏（通常设为写保护） |
| **简单** | BL 只做必要的事，复杂逻辑交给 APP |
| **可恢复** | 任何异常状态都有恢复到可工作状态的路径 |

---

### 第2章 Flash 存储器物理特性

#### 2.1 NOR Flash 基本特性

STM32L4 系列使用嵌入式 NOR Flash，关键物理特性：

```
╔══════════════════════════════════════════════════════════╗
║      STM32L431CBT6 Internal Flash Characteristics       ║
╠══════════════════════════════════════════════════════════╣
║  Total Size:      128 KB  (0x08000000 - 0x0801FFFF)     ║
║  Page Size:       2 KB    (2048 bytes)                   ║
║  Total Pages:     64                                    ║
║  Write Unit:      8 bytes (Double Word)                  ║
║  Erase Value:     0xFF                                  ║
║  Write Mechanism: 1→0 only (bit programming)             ║
║  Endurance:       ~10,000 Program/Erase cycles           ║
╚══════════════════════════════════════════════════════════╝
```

#### 2.2 理解"写前擦除"（Erase-Before-Write）

这是 NOR Flash 最重要的约束：

```
初始状态（已擦除）:  0xFF  0xFF  0xFF  0xFF
                     1111  1111  1111  1111

写入 0x12 后:        0x12  0xFF  0xFF  0xFF
                     0001  0010 ....

再次写入 0x34:       0x10  0xFF  0xFF  0xFF   ← 错误！
                     0001  0000 ....           只有 1→0 的位可以写

必须先擦除回 0xFF:   0xFF  0xFF  0xFF  0xFF   ← 擦除以 Page 为单位
再写入 0x34:         0x34  0xFF  0xFF  0xFF   ← 正确
```

**关键结论**：
- 写操作只能将 bit 从 `1` 变为 `0`
- 要将 bit 从 `0` 变为 `1`，必须擦除整个 Page
- 擦除以 Page（2KB）为单位，写入以 Double Word（8 字节）为单位

#### 2.3 写对齐要求

STM32L4 的 Flash 写入要求 **8 字节对齐**：

```c
// 正确：地址和长度都是 8 的倍数
HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, 0x08004000, *(uint64_t*)data);

// 错误：地址不是 8 字节对齐
HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, 0x08004001, ...);  // ❌
```

这就是为什么 OTA 代码需要**写缓冲**（write buffer）—— 上层传来的数据可能任意对齐，需要在 BL 内部缓冲突 8 字节边界再写入。

---

### 第3章 内存布局与分区策略

#### 3.1 分区设计的约束

分区设计受以下因素制约：
1. **Flash 总容量** —— 决定了各分区大小上限
2. **Page 大小** —— 分区边界必须 Page 对齐（擦除约束）
3. **Bootloader 体积** —— 越小越好，但要预留扩展空间
4. **应用程序体积** —— 包含未来功能增长的空间
5. **固件升级方式** —— 单槽、双槽还是外部存储

#### 3.2 常见分区方案

##### 方案一：单槽（Single Slot）—— 最简单

```
┌─────────────┐ 0x08000000
│  Bootloader │  16KB
├─────────────┤ 0x08004000
│             │
│  APP Slot   │  104KB
│             │
├─────────────┤ 0x0801E000
│  Metadata   │  8KB
└─────────────┘ 0x08020000
```

**特点**：
- 空间利用率最高（APP 占 104KB）
- 升级时 APP 无法运行 —— 必须在 BL 中接收新固件
- BL 体积必须更大（要包含通信协议栈）
- **升级失败 → 变砖**，没有备份

##### 方案二：双槽（Dual Slot / A-B）—— 推荐

```
┌─────────────┐ 0x08000000
│  Bootloader │  16KB
├─────────────┤ 0x08004000
│  Slot 0     │  52KB  ← 当前运行的 APP
│  (Primary)  │
├─────────────┤ 0x08011000
│  Slot 1     │  52KB  ← 新固件下载区
│  (Secondary)│
├─────────────┤ 0x0801E000
│  Metadata   │  8KB
└─────────────┘ 0x08020000
```

这是**本工程采用**的方案，也是业界最常用的方案。

**核心思想**：
- APP 在 Slot 0 中运行时，OTA 固件下载到 Slot 1
- 下载完毕后重启，BL 将 Slot 1 内容复制到 Slot 0
- Slot 0 始终是"运行槽位"，Slot 1 是"下载槽位"

**优点**：
- 升级过程中 APP 可以继续运行（只要不影响通信）
- BL 保持简洁 —— 通信在 APP 中处理
- 可以回滚（如果保留了旧固件）

**缺点**：
- 空间利用率只有 ~40%（两个槽各 52KB）
- Swap 过程中断电极易出问题

##### 方案三：容错 Swap（MCUboot 默认模式）—— 可恢复的升级

MCUboot 默认**仍然是复制式 Swap**（Slot 1 → Slot 0），BL 始终跳转到 Slot 0（固定编译地址运行）。但与方案二的本质区别在于**Swap 过程是可恢复的**：

```
Swap 过程（逐 Sector，带状态记录）：
┌──────────────────────────────────────────────────┐
│ ① 写 status = "swapping, sector=0" 到 Metadata   │
│ ② 将 Slot 0 Sector 0 暂存到 Scratch/镜像尾       │
│ ③ 擦除 Slot 0 Sector 0                           │
│ ④ 从 Slot 1 复制 Sector 0 → Slot 0               │
│ ⑤ 写 status = "swapping, sector=1"                │  ← 掉电可续传
│ ⑥ 重复直到全部完成                                 │
│ N 全部完成 → status = "done"                       │
└──────────────────────────────────────────────────┘

掉电恢复：
  BL 启动 → 读 status → "swapping, sector=5"
  → Slot 1 的数据还在 → 从 Sector 5 继续 Swap
```

**与方案二的关键区别**：
- 不直接覆盖式 Copy——而是逐扇区 + 状态追踪
- 任何时候断电，都能从记录点恢复，Slot 1 数据未损坏
- MCUboot 使用 3 区布局（Slot A + Slot B + Scratch），增加了一个暂存区

**对传统 MCU 固件的关键约束**：

> 固件在编译时通过链接脚本确定了绝对地址（如 `ORIGIN = 0x08004000`）。如果将同一份二进制直接放到 `0x08011000` 执行，所有绝对跳转（`ldr r0, =0x08006A00`）都会指向错误位置 → **HardFault**。因此，不经过复制而直接从 Slot B 启动，需要满足以下条件之一：
>
> 1. **位置无关代码（PIC, Position-Independent Code）**：编译时加 `-fPIC -fpie -msingle-pic-base`，代码使用 PC 相对寻址 + GOT（Global Offset Table）。代价：体积增加 5–15%，性能下降，需额外重定位步骤。
> 2. **硬件 Flash 重映射**：部分高端 MCU 支持通过寄存器将不同物理 Bank 映射到同一逻辑地址（如 `0x08000000`），固件编译地址不变，硬件负责地址转换。STM32L4 **不支持**此功能。
> 3. **双份链接**：为 Slot A 和 Slot B 分别编译两份固件（不同链接脚本），升级时发送对应版本。管理成本高，不实用。

**结论**：对于 STM32L4 这类 Cortex-M4 MCU，最实际的做法是 **MCUboot 风格的容错 Swap**——保持"从 Slot 0 运行"的模型，但在 Swap 流程中增加可恢复性。

##### 方案四：外部 Flash

```
Internal Flash              External SPI Flash
┌──────────┐               ┌──────────────┐
│  BL      │               │  Slot 1      │  大容量（几MB）
├──────────┤               │  (下载区)    │
│  Slot 0  │               │              │
│  (运行)  │               └──────────────┘
├──────────┤
│  Meta    │
└──────────┘
```

适用场景：固件很大（> 内部 Flash 一半），常见于 Linux-capable 的 MPU 或带 WiFi/BT 协议的设备。

#### 3.3 本工程的分区布局

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

**设计分析**：

| 分区 | 大小 | 说明 |
|------|------|------|
| Bootloader | 16KB（8 pages） | 预留足够空间，虽然当前 BL 只有 ~4KB |
| Slot 0 | 52KB（26 pages） | APP 运行区，0x08004000 起始 |
| Slot 1 | 52KB（26 pages） | OTA 下载区，0x08011000 起始 |
| Metadata | 8KB（4 pages） | 只用了前面几个字节，大材小用 |

**值得商榷的点**：
- Metadata 占用 8KB（4 pages），但实际只需 64 字节。可以减少到 1 page（2KB），释放 6KB 给 APP。
- 当前使用**一次性覆盖式 Swap**（Slot1 → Slot0），而非带状态追踪的容错 Swap。掉电恢复和回滚问题详见第15章。

---

### 第4章 Cortex-M 启动流程详解

理解启动流程是理解 Bootloader 的基础。

#### 4.1 从硬件复位到 main()

```
上电 / 复位
    │
    ▼
① CPU 从 0x08000000 读取 SP (MSP 初始值)
    │
    ▼
② CPU 从 0x08000004 读取 PC (Reset_Handler 地址)
    │
    ▼
③ 执行 Reset_Handler:
   ├── 设置堆栈指针 SP = _estack
   ├── 调用 SystemInit()  ─── 配置时钟、设置 VTOR
   ├── 复制 .data 段 (Flash → RAM)
   ├── 清零 .bss 段
   ├── 调用 __libc_init_array() (静态构造函数)
   └── 调用 main()
```

#### 4.2 向量表（Vector Table）

向量表是 Flash 最开头的区域，前两项至关重要：

```c
// startup_stm32l431xx.s 中的向量表开头
g_pfnVectors:
    .word  _estack           // [0] 初始堆栈指针（MSP）
    .word  Reset_Handler      // [1] 复位向量（启动地址）
    .word  NMI_Handler        // [2] 不可屏蔽中断
    .word  HardFault_Handler  // [3] 硬件错误
    ...
```

CPU 上电后，硬件自动做两件事：
1. 从地址 `0x00000000`（映射到 `0x08000000`）读取 4 字节 → 设置 MSP
2. 从地址 `0x00000004`（映射到 `0x08000004`）读取 4 字节 → 跳转执行

#### 4.3 VTOR：让 APP 拥有自己的向量表

这是 Bootloader 设计的**关键知识点**。

**问题**：BL 和 APP 是两个独立编译的程序，各自有自己的向量表。BL 的向量表在 `0x08000000`，APP 的向量表在 `0x08004000`（Slot 0 开头）。CPU 复位后默认从 `0x08000000` 取向量表。

**解决**：Cortex-M 处理器有 VTOR 寄存器（Vector Table Offset Register），可以重新定位向量表：

```c
// 在跳转到 APP 之前
SCB->VTOR = 0x08004000;  // 将向量表指向 APP 的位置
```

**标准做法**：

```
① BL 启动时：
   SCB->VTOR = FLASH_BASE;       // 指向 BL 自己的向量表 (0x08000000)
   处理自己的中断...

② 跳转到 APP 前：
   SCB->VTOR = APP_START_ADDR;   // 指向 APP 的向量表 (0x08004000)

③ APP 的 SystemInit() 中：
   SCB->VTOR = VECT_TAB_BASE_ADDRESS | VECT_TAB_OFFSET;
   确保 APP 的向量表正确
```

**⚠️ 本工程的一个陷阱**：`system_stm32l4xx.c` 中硬编码了 `VECT_TAB_OFFSET = 0x00004000U`，这意味着 `SystemInit()` 会把 VTOR 设置为 `0x08004000`（APP 位置）。Bootloader 的 `bl_main.c` 中在 `HAL_Init()` 之前手动修复了这个问题：

```c
// BL 必须在 HAL_Init 之前矫正 VTOR，因为 SystemInit 会把它设为 0x08004000
SCB->VTOR = FLASH_BASE;
```

这是**不好的设计**——SystemInit 中的 VTOR 偏移应该是编译时配置的，而不是硬编码。标准做法是在链接脚本中定义符号，或者在 BL 的编译选项中覆盖 `VECT_TAB_OFFSET` 宏。

**标准做法**：
```c
// 不要在 system_stm32l4xx.c 中写死偏移！
// 应该通过编译宏区分：
#ifdef BOOTLOADER_BUILD
  #define VECT_TAB_OFFSET  0x00000000U
#else
  #define VECT_TAB_OFFSET  0x00004000U
#endif
```

---

## 第二部分：Bootloader 核心设计

### 第5章 Bootloader 的启动与自检

#### 5.1 最小化硬件初始化

Bootloader 应该做**最少必要的初始化**：

```c
int main(void)
{
    // ① 矫正向量表（如果 SystemInit 改了它）
    SCB->VTOR = FLASH_BASE;

    // ② 最小 HAL 初始化
    HAL_Init();

    // ③ 时钟配置 —— 建议用内部 HSI，不依赖外部晶振
    SystemClock_Config();  // HSI 16MHz

    // ④ 初始化必要的外设（调试串口）
    UART_Init();

    // ⑤ 执行 BL 逻辑
    bl_run();

    // BL 正常不返回（跳转到 APP）
    while (1);
}
```

**关键设计原则**：
- **不初始化 APP 需要的所有外设** —— 留给 APP 自己做
- **使用内部振荡器**（HSI/MSI）—— 不依赖外部晶振（外部晶振可能不焊）
- **不需要的 HAL 驱动不链接** —— 减小 BL 体积

#### 5.2 为什么 BL 应该用 HSI

| 因素 | HSI（内部） | HSE（外部晶振） |
|------|-----------|-------------|
| 启动时间 | ~2μs | ~2ms（等待稳定） |
| 可靠性 | 极高（芯片内置） | 依赖焊接和晶振质量 |
| 精度 | ±1%（足够 UART） | ±20ppm（高精度） |
| 适用场景 | BL、故障恢复 | APP（需要精确时钟） |

#### 5.3 BL 的自检流程

```
上电
 │
 ▼
┌─────────────┐
│ 1. 向量表矫正 │
└──────┬──────┘
       ▼
┌─────────────┐
│ 2. 时钟+UART │
└──────┬──────┘
       ▼
┌─────────────┐    损坏
│ 3. 读 Metadata ├────→ 初始化默认 Metadata
└──────┬──────┘
       ▼
┌─────────────┐
│ 4. 检查状态机│
│   SWAP?      │──→ Swap Slot1→Slot0 → 写 TESTING → 跳转
│   TESTING?   │──→ boot_count++ → 超过阈值? → ROLLBACK
│   ROLLBACK?  │──→ 恢复 NORMAL
│   NORMAL?    │──→ 直接验证跳转
└──────┬──────┘
       ▼
┌─────────────┐    有效
│ 5. 验证 APP  ├────→ 跳转到 APP
└──────┬──────┘
       │ 无效
       ▼
┌─────────────────────┐
│ 6. 停留 BL, 等待恢复 │
└─────────────────────┘
```

---

### 第6章 镜像验证

#### 6.1 至少应该验证什么

BL 在跳转到 APP 之前，必须确认 APP 镜像是有效的。

**基础验证**（本工程实现了）：

```c
int bl_validate_image(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)addr;       // 读取 SP
    uint32_t reset = *(volatile uint32_t *)(addr + 4); // 读取 Reset_Handler

    // ① SP 必须在 RAM 范围内
    if (sp < 0x20000000U || sp > (0x20000000U + 48U * 1024U)) {
        return -1;  // SRAM1: 0x20000000 ~ 0x2000C000
    }

    // ② Reset_Handler 必须在 Flash 范围内
    if (reset < 0x08000000U || reset > (0x08000000U + FLASH_TOTAL_SIZE)) {
        return -1;
    }

    // ③ 复位向量必须是 Thumb 代码（bit0 = 1）
    if ((reset & 1U) == 0) {
        return -1;  // Cortex-M 只能执行 Thumb 指令
    }

    return 0;
}
```

**为什么检查 bit0**：Cortex-M 处理器只支持 Thumb 指令集。地址的最低位为 `1` 表示这是 Thumb 代码地址（实际跳转时硬件会忽略 bit0）。

#### 6.2 标准做法：CRC 或签名验证

**仅检查 SP 和 Reset_Handler 是不够的** —— 一个全是 `0xFF` 的区域可能偶然满足这两个条件。

**标准做法**应该在验证中至少包含**完整镜像的 CRC32 校验**：

```c
// 标准做法：验证前先做 CRC 校验
int bl_validate_image_full(uint32_t addr)
{
    // ① 基本检查（SP、Reset_Handler）
    if (bl_validate_image(addr) != 0) return -1;

    // ② 从 Metadata 读取期望的 CRC 和 Size
    boot_meta_t meta;
    meta_load(&meta);

    // ③ 重新计算 APP 区域的 CRC32
    uint32_t actual_crc = crc32_calc((const void*)addr, meta.app_size);

    // ④ 比对
    if (actual_crc != meta.app_crc32) {
        return -1;  // 固件损坏
    }

    return 0;
}
```

#### 6.3 安全启动中的签名验证

进一步的标准做法是**非对称签名验证**（详见第14章）：

```
镜像 = [固件数据] + [数字签名]
        │              │
        │              └── 由私钥签名
        │
        └── 用公钥验证：ECDSA / RSA
```

---

### 第7章 固件跳转技术

#### 7.1 跳转的完整步骤

从 BL 跳转到 APP 是 Bootloader 最关键的操作，必须按正确顺序执行：

```c
void bl_jump_to_app(uint32_t addr)
{
    typedef void (*app_entry_t)(void);

    // ① 从 APP 向量表读取 SP 和入口地址
    uint32_t app_sp = *(volatile uint32_t *)addr;
    uint32_t app_reset = *(volatile uint32_t *)(addr + 4);

    // ② 关闭所有中断
    __disable_irq();

    // ③ 关闭 SysTick（否则会触发 SysTick_Handler 跳到 APP 的地址）
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    // ④ 清除所有挂起的中断
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;  // 关闭中断使能
        NVIC->ICPR[i] = 0xFFFFFFFF;  // 清除挂起标志
    }

    // ⑤ 将向量表指向 APP
    SCB->VTOR = addr;

    // ⑥ 设置 APP 的堆栈指针（MSP）
    __set_MSP(app_sp);

    // ⑦ 可选：设置 PSP = MSP（如果 APP 用 RTOS）
    __set_PSP(app_sp);

    // ⑧ 重新启用中断（APP 会自行配置自己的中断）
    __enable_irq();

    // ⑨ 跳转到 APP 的 Reset_Handler
    app_entry_t app_entry = (app_entry_t)app_reset;
    app_entry();

    // ⑩ 永远不会到这里
    while (1);
}
```

#### 7.2 常见坑

**坑 1：忘记关闭 SysTick**

SysTick 中断触发时，PC 会跳转到向量表中 SysTick 的地址。如果此时 VTOR 已经改了，SysTick_Handler 是 APP 中的地址；如果 VTOR 还没改，SysTick_Handler 是 BL 中的地址。不论哪种情况，都没准备好正确处理 → HardFault。

**正确做法**：跳转前先关 SysTick。

**坑 2：忘记清除外设中断**

BL 使用的 UART 中断可能在跳转时挂起。APP 初始化外设时如果中断产生，会进入不一致的状态。

**正确做法**：清除所有 NVIC 中断，让 APP 自己重新配置。

**坑 3：BL 和 APP 使用不同的 FPU 设置**

如果 BL 编译时用了 `-mfloat-abi=soft`，而 APP 用了 `-mfloat-abi=hard`，跳转后 FPU 寄存器可能不一致。

**正确做法**：BL 和 APP 编译选项保持一致（FPU 相关）。

**坑 4：没有恢复外设到复位状态**

BL 初始化的 GPIO、UART、时钟等可能影响 APP 的正常运行。

**标准做法**：在跳转前调用 `HAL_DeInit()` 复位所有外设，或者在 BL 中只初始化最小必要外设。

#### 7.3 标准做法：参考 ARM 官方推荐

ARM 官方推荐的复位到应用程序的流程：

```c
// ARM 推荐的标准跳转步骤
__attribute__((noreturn))
void boot_jump(uint32_t vector_table_addr)
{
    uint32_t *vector_table = (uint32_t*)vector_table_addr;

    // 1. 禁用中断
    __disable_irq();

    // 2. 复位所有使用中的外设
    //    HAL_DeInit()  或手动复位

    // 3. 设置 VTOR
    SCB->VTOR = vector_table_addr;

    // 4. 设置堆栈
    __set_MSP(vector_table[0]);

    // 5. 加载复位向量
    uint32_t reset_handler = vector_table[1];

    // 6. 跳转（使用内联汇编确保彻底清理）
    __asm volatile (
        "mov   r0, %[addr]    \n"  // 将入口地址放入 r0
        "bx    r0              \n"  // 跳转
        :: [addr] "r" (reset_handler)
        : "r0"
    );

    __builtin_unreachable();
}
```

---

### 第8章 升级状态机

#### 8.1 状态定义

升级状态机是 BL 最核心的逻辑，它决定 BL 在启动时做什么。

**本工程的四状态模型**：

```
                    ┌──────────────────────────────────┐
                    │                                  │
                    ▼                                  │
              ┌──────────┐    OTA下载完成        ┌─────┴────┐
    上电 ───→ │  NORMAL  │ ─────────────────→  │   SWAP    │
              │ Slot 0   │                      │ Slot1→0  │
              └──────────┘                      └─────┬────┘
                    ▲                                 │
                    │                          Swap 成功
                    │                                 │
                    │                                 ▼
                    │                          ┌──────────┐
                    │          boot_count++     │ TESTING  │
                    │         ┌─────────────── │ 新固件测试│
                    │         │                └─────┬────┘
                    │         │ 超过max_boot          │
                    │         │                       │ APP确认正常
                    │    ┌────┴────┐                  │
                    │    │ROLLBACK │                  │
                    │    └─────────┘                  │
                    │                                 │
                    └─────────────────────────────────┘
                    回到 NORMAL               ota_confirm_app()
```

**状态说明**：

| 状态 | 值 | 含义 | 触发条件 |
|------|-----|------|---------|
| NORMAL | 0x00 | 正常运行 | 上电、升级确认、回滚后 |
| SWAP | 0x01 | 等待复制 Slot1→Slot0 | OTA 下载完成+提交 |
| TESTING | 0x02 | 新固件测试中 | Swap 完成后 |
| ROLLBACK | 0x03 | 回滚旧固件 | TESTING 启动失败超限 |

#### 8.2 状态转移逻辑（BL 端）

```c
switch (meta.boot_state) {

case BOOT_STATE_SWAP:
    // Slot 1 中有新固件，需要复制到 Slot 0
    bl_swap_slot1_to_slot0(meta.ota_size);
    meta.boot_state = BOOT_STATE_TESTING;  // 进入测试
    meta.boot_count = 0;
    break;

case BOOT_STATE_TESTING:
    meta.boot_count++;
    if (meta.boot_count > meta.max_boot_count) {
        // 启动失败次数超限，触发回滚
        meta.boot_state = BOOT_STATE_ROLLBACK;
    }
    break;

case BOOT_STATE_ROLLBACK:
    // 回滚：回到 NORMAL，尝试用旧固件
    meta.boot_state = BOOT_STATE_NORMAL;
    break;

case BOOT_STATE_NORMAL:
default:
    // 正常启动，什么都不用做
    break;
}
```

#### 8.3 TESTING 状态与软件确认

这是**防变砖的关键机制**：

```
┌─────────────────────────────────────────────────────────┐
│  Swap 完成后，BL 不知道新固件能否正常工作               │
│                                                         │
│  TESTING 状态给 APP 机会来证明"我能正常工作"             │
│                                                         │
│  如果 APP 在 max_boot_count 次启动内调用 ota_confirm_app()│
│    → 状态变为 NORMAL → 新固件生效                       │
│                                                         │
│  如果 APP 到期还没确认（每次启动 boot_count++）           │
│    → 超过阈值 → ROLLBACK → 恢复旧固件                   │
└─────────────────────────────────────────────────────────┘
```

**APP 侧的确认代码**：

```c
int ota_confirm_app(void)
{
    boot_meta_t meta;
    meta_load(&meta);

    if (meta.boot_state == BOOT_STATE_TESTING) {
        meta.boot_state = BOOT_STATE_NORMAL;
        meta.boot_count = 0;
        meta_save(&meta);
    }
    return 0;
}
```

**标准做法**：APP 应该在自检完成后调用 `ota_confirm_app()`，而不是在 `main()` 开头就调用：

```c
int main(void) {
    HAL_Init();
    SystemClock_Config();

    // ❌ 不要在这里确认！硬件可能还没初始化完
    // ota_confirm_app();   太早了！

    // ✅ 先做自检
    if (self_test_all_peripherals() == OK) {
        ota_confirm_app();   // 所有硬件正常工作后再确认
    }

    // 正常业务逻辑
    while (1) { ... }
}
```

#### 8.4 ⚠️ 本工程状态机的缺陷

1. **没有真正的回滚**：状态机有 ROLLBACK 状态，但 `bl_swap_slot1_to_slot0()` 已经覆盖了 Slot 0 中的旧固件。回滚意味着回到一个已经不存在的固件。

2. **Swap 过程无保护**：如果 Swap（擦除 + 复制）过程中断电，Slot 0 会被破坏——既不是旧固件也不是新固件 → 变砖。

3. **只有一次升级路径**：SWAP → TESTING → (NORMAL|ROLLBACK)。一旦进入 NORMAL，无法再次触发 SWAP，直到下次 OTA。

**标准做法**（MCUboot 风格）见第15章。

---

### 第9章 元数据管理

#### 9.1 元数据应该存储什么

Metadata（元数据）是存储在固定 Flash 区域中的持久化状态信息。它告诉 BL：

- 当前处于什么状态（NORMAL/SWAP/TESTING/...）
- APP 固件的属性（版本、大小、CRC）
- 待升级固件的属性（版本、大小、CRC）
- 升级尝试的次数

**本工程的结构体**：

```c
typedef struct __attribute__((packed, aligned(8))) {
    uint32_t magic;          // 魔数 0x4F54414D ("OTAM")
    uint32_t version;        // 元数据格式版本
    uint8_t  boot_state;     // 当前状态
    uint8_t  boot_count;     // 启动计数（TESTING 模式）
    uint8_t  max_boot_count; // 最大尝试次数
    uint8_t  reserved0;
    uint32_t app_size;       // APP 固件大小
    uint32_t app_crc32;      // APP 固件 CRC32
    uint32_t app_version;    // APP 版本号
    uint32_t ota_size;       // OTA 固件大小
    uint32_t ota_crc32;      // OTA 固件 CRC32
    uint32_t ota_version;    // OTA 版本号
    uint32_t meta_crc32;     // 元数据 CRC（不含此字段）
    uint8_t  padding[20];    // 填充到 64 字节
} boot_meta_t;
```

#### 9.2 备份与容错

元数据可能会在写入时因断电而损坏。标准做法是**双副本冗余存储**：

```
Metadata Partition (8KB)
┌──────────────────────────────┐
│  Page 0: Primary Metadata    │  ← 位于分区偏移 0
│  (2KB, 实际使用 64 bytes)    │
├──────────────────────────────┤
│  Page 1: Backup Metadata     │  ← 位于分区偏移 2KB
│  (2KB, 实际使用 64 bytes)    │
├──────────────────────────────┤
│  Page 2-3: 保留              │
└──────────────────────────────┘
```

**读取策略**（本工程实现得很好）：

```c
int meta_load(boot_meta_t *meta)
{
    boot_meta_t primary, backup;

    // 读取两份副本
    flash_part_read(meta_part, PRIMARY_OFFSET, sizeof(primary), &primary);
    flash_part_read(meta_part, BACKUP_OFFSET, sizeof(backup), &backup);

    // 校验 CRC + Magic
    bool primary_ok = meta_validate(&primary);
    bool backup_ok  = meta_validate(&backup);

    if (primary_ok) {
        *meta = primary;
        // 如果备份损坏，自动修复
        if (!backup_ok) repair_backup(&primary);
        return 0;
    }
    if (backup_ok) {
        *meta = backup;
        // 如果主份损坏，自动修复
        repair_primary(&backup);
        return 0;
    }
    return -1;  // 两份都坏了
}
```

**写入策略**：

```c
int meta_save(boot_meta_t *meta)
{
    // 先计算 CRC
    meta_compute_crc(meta);

    // 先写主副本
    erase_and_write(PRIMARY_OFFSET, meta);
    // 再写备份副本
    erase_and_write(BACKUP_OFFSET, meta);
}
```

#### 9.3 写入的原子性问题

**⚠️ 当前实现的问题**：`meta_save()` 先擦除再写入。如果在擦除后、写入完成前断电 → 该副本损坏。但因为有双副本，只要不是两个副本同时损坏，就能恢复。

**标准做法**：写入顺序保证至少有一个有效副本：

```
保存元数据的安全顺序：
① 擦除备份区 Page
② 写入备份区          ← 现在备份是新的，主份是旧的
③ 擦除主份区 Page
④ 写入主份区          ← 现在两份都是新的
```

如果在步骤②后断电：备份是新数据，主份是旧数据。BL 应该选择版本更新（或 boot_count 更大）的那一份。

#### 9.4 更好的结构：TLV（Type-Length-Value）

本工程使用固定结构体，升级元数据格式需要增加字段时兼容性差。标准做法是 TLV 格式：

```
[Magic: 4B][Version: 4B][Tag:2B][Length:2B][Value:NB]...[CRC:4B]
                                 └── 可扩展 ──┘
```

这样新版本的 BL 可以解析包含额外字段的元数据（忽略不认识的 Tag）。

---

## 第三部分：OTA 升级系统

### 第10章 OTA 升级全流程

#### 10.1 端到端流程

```
 ┌──────────────────────────── 完整 OTA 流程 ──────────────────────────┐
 │                                                                      │
 │  Host (PC/Server)              MCU (APP)            MCU (BL)         │
 │       │                           │                    │             │
 │  ═══[阶段1: 握手协商]════════    │                    │             │
 │       │── 查询版本 ───────────→  │                    │             │
 │       │←─ 当前版本 v1.0 ────────  │                    │             │
 │       │── 开始 OTA ───────────→  │                    │             │
 │       │   (固件头: 版本+大小+CRC)  │                    │             │
 │       │                           │ ① 验证固件头        │             │
 │       │                           │ ② 擦除 Slot 1      │             │
 │       │←─ ACK OK ───────────────  │                    │             │
 │       │                           │                    │             │
 │  ═══[阶段2: 数据传输]════════    │                    │             │
 │       │── 数据块 0 (1024B) ───→  │                    │             │
 │       │←─ ACK OK (seq=0) ───────  │ ③ 写入 Flash       │             │
 │       │── 数据块 1 (1024B) ───→  │                    │             │
 │       │←─ ACK OK (seq=1) ───────  │                    │             │
 │       │        ...                │                    │             │
 │       │── 数据块 N ────────────→  │                    │             │
 │       │←─ ACK OK ───────────────  │                    │             │
 │       │                           │                    │             │
 │  ═══[阶段3: 校验验证]════════    │                    │             │
 │       │── 传输结束 ────────────→  │                    │             │
 │       │                           │ ④ CRC32 校验       │             │
 │       │←─ ACK OK (CRC pass) ────  │                    │             │
 │       │                           │                    │             │
 │  ═══[阶段4: 提交重启]════════    │                    │             │
 │       │── 确认升级+重启 ───────→  │                    │             │
 │       │                           │ ⑤ 写 metadata SWAP │             │
 │       │                           │ ⑥ NVIC_SystemReset │
 │       │                           │        │            │             │
 │       │                           │        └──────┐     │             │
 │       │                           │               ▼     │             │
 │       │                           │         ┌─────────┐ │             │
 │       │                           │         │  BL     │ │             │
 │       │                           │         │ 读 meta │ │             │
 │       │                           │         │ 发现SWAP│ │             │
 │       │                           │         │ Slot1→0│ │             │
 │       │                           │         │ 跳转APP │ │             │
 │       │                           │         └─────────┘ │             │
 │       │                           │                    │             │
 │       │                           │ ⑦ APP 自检正常      │             │
 │       │                           │    ota_confirm_app() │             │
 │       │                           │    TESTING→NORMAL   │             │
 └─────────────────────────────────────────────────────────────────────┘
```

#### 10.2 固件头结构

固件头包含 BL 验证新固件所需的全部信息：

```c
typedef struct {
    uint32_t magic;        // 0x4F544148 ("OTAH")
    uint32_t fw_version;   // 固件版本号
    uint32_t fw_size;      // 固件总大小 (bytes)
    uint32_t fw_crc32;     // 固件数据的 CRC32
    uint32_t header_crc32; // 头自身的 CRC32（最后字段，覆盖前四个）
} ota_header_t;  // 20 字节
```

**为什么 header 自身也需要 CRC**：主机在串口上发送 20 字节的头部，传输过程中可能损坏。接收方验证 header_crc32 确保头本身正确，再信任头中的 fw_size 和 fw_crc32。

#### 10.3 每阶段的错误处理

```
阶段1 (握手)：头部损坏 / Magic 不匹配 / 固件太大 → 拒绝升级
阶段2 (传输)：帧CRC错误 → 重传
              序列号不连续 → 拒绝（ACK_ERR_SEQ）
              Flash写入失败 → 标记错误状态
阶段3 (校验)：CRC32 不匹配 → 标记错误，需要重新下载
阶段4 (提交)：Metadata 写入失败 → 重试写入
```

---

### 第11章 通信协议设计

#### 11.1 协议分层

```
┌───────────────────────────────────────────────┐
│  应用层 (ota_send.py / ota_core.c)            │
│  职责：切分包、进度管理、固件头组装           │
├───────────────────────────────────────────────┤
│  传输层 (ota_proto.c)                         │
│  职责：帧封装/解析、CRC校验、SEQ管理、ACK     │
├───────────────────────────────────────────────┤
│  链路层 (UART + 中断/DMA)                     │
│  职责：逐字节收发、中断驱动                   │
└───────────────────────────────────────────────┘
```

这套分层是本工程的**亮点之一**——各层职责清晰，耦合度低。

#### 11.2 帧格式详解

```
┌──────┬──────┬──────┬───────┬───────┬──────────┬─────────┬─────────┐
│ SOF  │ CMD  │ SEQ  │ LEN_L │ LEN_H │ PAYLOAD  │ CRC16_L │ CRC16_H │
│ 1 B  │ 1 B  │ 1 B  │  1 B  │  1 B  │ 0~1024 B │   1 B   │   1 B   │
│ 0xAA │      │      │       │       │          │         │         │
└──────┴──────┴──────┴───────┴───────┴──────────┴─────────┴─────────┘
         │                                               │
         └────────── CRC16 覆盖范围 ──────────────────────┘
```

**各字段含义**：

| 字段 | 大小 | 说明 |
|------|------|------|
| SOF | 1 byte | 帧起始标识 `0xAA`，用于帧同步 |
| CMD | 1 byte | 命令码，bit7=0 表示请求，bit7=1 表示应答 |
| SEQ | 1 byte | 序列号，0~255 循环，用于丢帧检测 |
| LEN | 2 bytes | Payload 长度，小端序，最大 1024 |
| PAYLOAD | 0~1024 B | 有效负载 |
| CRC16 | 2 bytes | CRC-16/MODBUS，覆盖 CMD+SEQ+LEN+PAYLOAD |

**为什么 CRC 不覆盖 SOF**：SOF 用作帧同步，本身不参与完整性检查。如果 SOF 出错，接收方找不到帧头，整个帧被丢弃。

#### 11.3 命令码设计：主从模式

```
Host → MCU (CMD, bit7=0)      MCU → Host (ACK, bit7=1)
─────────────────────────      ─────────────────────────
CMD_OTA_START     = 0x01       → ACK = 0x81
CMD_OTA_DATA      = 0x02       → ACK = 0x82
CMD_OTA_END       = 0x03       → ACK = 0x83
CMD_OTA_ABORT     = 0x04       → ACK = 0x84
CMD_OTA_REBOOT    = 0x05       → ACK = 0x85
CMD_QUERY_STATUS  = 0x10       → ACK = 0x90
CMD_QUERY_VERSION = 0x11       → ACK = 0x91
```

**ACK 的 payload[0] 固定为状态码**：

```c
ACK_OK           = 0x00   // 成功
ACK_ERR_CRC      = 0x01   // 帧 CRC 错误
ACK_ERR_SEQ      = 0x02   // 序列号错误
ACK_ERR_CMD      = 0x03   // 未知命令
ACK_ERR_STATE    = 0x04   // 状态机不允许
ACK_ERR_FLASH    = 0x05   // Flash 操作失败
ACK_ERR_VERIFY   = 0x06   // 固件 CRC32 校验失败
ACK_ERR_SIZE     = 0x07   // 固件超限
```

#### 11.4 接收状态机（逐字节解析）

在 UART 中断中逐字节喂入：

```c
int proto_feed_byte(proto_ctx_t *ctx, uint8_t byte)
{
    switch (ctx->rx.state) {
    case RX_STATE_SOF:
        if (byte == PROTO_SOF) {
            ctx->rx.state = RX_STATE_CMD;
            ctx->rx.calc_crc = 0xFFFF;  // CRC 从 SOF 之后的字节开始计算
        }
        break;

    case RX_STATE_CMD:
        ctx->rx.frame.cmd = byte;
        ctx->rx.calc_crc = crc16_update(ctx->rx.calc_crc, byte);
        ctx->rx.state = RX_STATE_SEQ;
        break;

    case RX_STATE_SEQ:
        ctx->rx.frame.seq = byte;
        ctx->rx.calc_crc = crc16_update(ctx->rx.calc_crc, byte);
        ctx->rx.state = RX_STATE_LEN_L;
        break;

    // ... LEN_L, LEN_H, PAYLOAD, CRC_L, CRC_H ...

    case RX_STATE_CRC_H:
        ctx->rx.rx_crc |= ((uint16_t)byte << 8);
        // 验证 CRC
        if (ctx->rx.rx_crc == ctx->rx.calc_crc) {
            ctx->rx.state = RX_STATE_SOF;
            return 1;  // 完整帧就绪
        }
        // CRC 不匹配：静默丢弃
        ctx->rx.state = RX_STATE_SOF;
        break;
    }
    return 0;  // 还需要更多字节
}
```

#### 11.5 CRC 选型

**帧级 CRC**：CRC-16/MODBUS
- 多项式：`0x8005`（反转 `0xA001`）
- 初始值：`0xFFFF`
- 特点：计算快（适合逐字节），检测能力强（能检测连续 16 位以下的错误）

**固件级 CRC**：CRC-32（Ethernet 标准）
- 多项式：`0x04C11DB7`（反转 `0xEDB88320`）
- 初始值：`0xFFFFFFFF`，最终异或 `0xFFFFFFFF`
- 特点：32 位提供了极强的错误检测能力（适合数万字节的固件）

#### 11.6 标准做法：序列号与流量控制

本工程实现了基于 SEQ 的简单流控：

```
Host 发送: CMD_OTA_DATA, seq=0
MCU  应答: ACK, seq=0 → expected_seq 更新为 1
Host 发送: CMD_OTA_DATA, seq=1
MCU  应答: ACK, seq=1 → expected_seq 更新为 2
Host 发送: CMD_OTA_DATA, seq=3   ← 跳过了 2！
MCU  应答: ACK_ERR_SEQ           ← 拒绝，要求重传
```

**更完善的标准做法**：

- **滑动窗口**：允许 N 个未确认的帧在途中（提高吞吐量）
- **选择性重传**：只重传丢失的帧，不中断整体流
- **窗口大小自适应**：根据链路质量动态调整

但对于 MCU 串口 OTA，**停-等协议**（发送一个帧、等待 ACK、再发送下一个）已经足够。UART 速率（115200 bps ≈ 11.5 KB/s）远低于 Flash 写入速率，瓶颈不在协议。

#### 11.7 协议设计的其他标准选择

| 选择 | 本工程 | 标准替代 |
|------|--------|---------|
| 帧头 | `0xAA` | `0x7E`（HDLC 风格）、`0x55`（平衡电平） |
| 帧校验 | CRC-16/MODBUS | CRC-16/CCITT、Fletcher-16 |
| 固件校验 | CRC-32/Ethernet | SHA-256（带签名的场景） |
| 流控 | SEQ + 停-等 | 滑动窗口 |
| 分包大小 | 1024 字节（固定） | 可变（协商阶段确定） |
| 编码方式 | 原始二进制 | COBS（Consistent Overhead Byte Stuffing）、Base64（文本通道） |

---

### 第12章 Flash 驱动抽象层

#### 12.1 抽象的价值

本工程设计了清晰的 Flash 抽象层，这是**标准做法中的精华**：

```c
// ① 底层设备抽象（操作物理 Flash）
struct flash_device {
    uint32_t base_addr;
    uint32_t total_size;
    uint32_t page_size;
    uint32_t write_size;
    int (*read)(uint32_t addr, uint32_t size, void *dst);
    int (*write)(uint32_t addr, uint32_t size, const void *src);
    int (*erase)(uint32_t addr, uint32_t size);
};

// ② 分区抽象（逻辑分区映射到物理 Flash 的某个区域）
struct flash_partition {
    const struct flash_device *flash;
    uint32_t offset;    // 相对于 flash->base_addr
    uint32_t size;
};
```

**好处**：
- **统一接口**：上层代码不需要知道底层是内部 Flash 还是外部 SPI Flash
- **边界保护**：分区访问自动校验地址范围，防止越界写入
- **对齐检查**：自动检查写入对齐（8 字节）和擦除对齐（Page 对齐）
- **可测试**：可以注入 mock 设备进行单元测试

#### 12.2 写缓冲：解决对齐问题

这是**必须理解的关键设计**：

```c
// 问题：上层传来的数据可能是任意长度的
// 例如：主机发送了 100 字节的数据块
// Flash 要求：8 字节对齐写入

// 解决：使用内部缓冲区
uint8_t write_buf[256];   // 256 字节缓冲（必须是 8 的倍数）
uint16_t buf_used;        // 缓冲中已有的字节数

int ota_feed_data(ota_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    // 填充缓冲区
    while (len > 0) {
        uint32_t space = sizeof(write_buf) - buf_used;
        uint32_t chunk = MIN(space, len);
        memcpy(&write_buf[buf_used], data, chunk);
        buf_used += chunk;
        data += chunk;
        len -= chunk;

        // 缓冲区满 → 刷入 Flash
        if (buf_used >= sizeof(write_buf)) {
            flush_buffer(ctx);  // 写入 buffer → Flash
        }
    }
}

int flush_buffer(ota_ctx_t *ctx)
{
    // 补齐到 8 字节对齐（填充 0xFF）
    uint32_t write_len = ALIGN_UP(buf_used, FLASH_WRITE_SIZE);  // = 8
    memset(&write_buf[buf_used], 0xFF, write_len - buf_used);

    flash_part_write(slot1, offset, write_len, write_buf);
    buf_used = 0;
}
```

#### 12.3 STM32 HAL Flash 操作的注意事项

```c
// 标准 Flash 写入序列
HAL_FLASH_Unlock();              // ① 解锁 Flash
__HAL_FLASH_CLEAR_FLAG(ALL);     // ② 清除遗留的错误标志
HAL_FLASH_Program(DOUBLEWORD,    // ③ 以双字为单位编程
    addr, *(uint64_t*)data);
HAL_FLASH_Lock();                // ④ 重新锁定
```

**⚠️ 关键点**：
- `__HAL_FLASH_CLEAR_FLAG` 是必须的——如果上次操作有错误标志位没清除，下次操作会失败
- Flash 编程期间**不能执行 Flash 中的代码**（Flash 处于忙状态）。对于单 Bank 的 STM32L431，这意味着整个 CPU 停顿。但对于 Cortex-M4，中断可以继续在 SRAM 中执行
- 如果 BL 和 APP 共享 Flash Bank，写操作会阻塞一切 Flash 读取

---

### 第13章 OTA 业务逻辑层

#### 13.1 OTA 状态机

```
                                ┌─────────────┐
                         ┌─────→│    IDLE     │←──────────┐
                         │      │   (空闲)    │            │
                         │      └──────┬──────┘            │
                         │             │ ota_begin()       │ abort()
                         │             ▼                   │
                         │      ┌─────────────┐            │
                         │      │   HEADER    │            │
                         │      │ (等待头部)  │            │
                         │      └──────┬──────┘            │
                         │             │ ota_feed_header() │
                         │             │ (验证+擦除Slot1)  │
                         │             ▼                   │
                         │      ┌─────────────┐            │
                         │      │  RECEIVING  │            │
                         │      │  (接收数据) │            │
                         │      └──────┬──────┘            │
                         │             │ ota_finish()      │
                         │             │ (CRC32校验)       │
                         │             ▼                   │
                         │      ┌─────────────┐            │
┌────────────────────────┘      │  VERIFYING  │            │
│  ota_confirm_app()            │  (验证中)   │            │
│  (APP侧确认)                  └──────┬──────┘            │
│                                     │                    │
│                              ┌──────┴──────┐            │
│                              │    DONE     │            │
│                              │  (完成)     │            │
│                              └──────┬──────┘            │
│                                     │ ota_commit()      │
│                                     │ (写metadata+重启) │
│                                     ▼                   │
│                              ┌─────────────┐            │
│                              │   等待重启  │            │
│                              └─────────────┘            │
│                                                        │
│                      ┌─────────────┐                   │
│                      │    ERROR    │───────────────────┘
│                      │   (错误)    │     abort() 可恢复
│                      └─────────────┘
```

#### 13.2 CRC 增量计算

本工程的一个**技术亮点**是 CRC32 的增量计算：

```c
// 传统方式：接收完所有数据后一次性计算 CRC
//   缺点：需要缓存整个固件（52KB！），MCU RAM 不够
//   crc = crc32_calc(all_data, total_size);  ❌

// 增量方式：每收到一个数据块就更新 CRC 状态
//   优点：只需 O(1) 内存
uint32_t crc_state = 0x00000000;  // CRC 初始值

void ota_feed_data(ota_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    // 增量更新 CRC
    ctx->running_crc = crc32_update(ctx->running_crc, data, len);

    // 同时缓冲并写入 Flash
    ...
}
```

**⚠️ 当前实现的小问题**：`ota_init()` 中 `running_crc = 0xFFFFFFFFU ^ 0xFFFFFFFFU`（结果为 `0`），而 `ota_begin()` 中设置为 `0x00000000U`。最终结果是一致的，但初始化表达式过于晦涩——直接写 `0` 更清晰。

**标准做法**：CRT 的增量计算在 `crc32_update` 中内部处理初始值的 XOR 转换：

```c
uint32_t crc32_update(uint32_t crc_state, const void *data, uint32_t len)
{
    // crc_state 是已 finalize 的值（已 XOR 0xFFFFFFFF）
    // 需要 undo XOR，继续计算，再 re-XOR
    uint32_t crc = crc_state ^ 0xFFFFFFFFU;
    for (...) { ... }
    return crc ^ 0xFFFFFFFFU;
}

// 初始状态：一个空数据的 CRC32（即 0x00000000）
#define CRC32_INITIAL  0x00000000U

// 最终校验：不需要额外处理
if (ctx->running_crc != header.fw_crc32) { ... }
```

---

## 第四部分：进阶主题

### 第14章 安全启动与固件签名

**⚠️ 本工程目前没有任何安全机制。** 这是最重要的缺失功能之一。

#### 14.1 威胁模型

没有签名验证的 Bootloader 面临以下威胁：

| 威胁 | 场景 | 后果 |
|------|------|------|
| 恶意固件注入 | 攻击者通过 UART 发送恶意固件 | 设备被控制 |
| 固件篡改 | 固件文件在传输/存储中被修改 | 设备变砖 |
| 版本回退 | 安装旧版本（有已知漏洞）的固件 | 安全退化 |

#### 14.2 数字签名基础

```
                    签名过程                          验证过程
              ┌─────────────────┐            ┌─────────────────┐
              │  原始固件       │            │  收到的固件     │
              │  firmware.bin   │            │  firmware.bin   │
              └───────┬─────────┘            │  + signature    │
                      │                      └───────┬─────────┘
                      ▼                              │
              ┌───────────────┐                      ▼
              │ SHA-256 哈希  │              ┌───────────────┐
              │ Hash = H(FW)  │              │ SHA-256 哈希  │
              └───────┬───────┘              │ Hash' = H(FW) │
                      │                      └───────┬───────┘
                      ▼                              │
              ┌───────────────┐                      ▼
              │ ECDSA 签名    │              ┌───────────────┐
              │ Sig = Sign(H) │              │ ECDSA 验证    │
              │ (使用私钥)    │              │ Verify(Hash', │
              └───────────────┘              │   Sig, PubKey)│
                                             └───────┬───────┘
                                                     │
                                              ┌──────┴──────┐
                                        通过  │  ✓ 继续启动 │
                                              │  ✗ 拒绝固件 │
                                              └─────────────┘
```

#### 14.3 ECDSA vs RSA

| 特性 | ECDSA (P-256) | RSA-2048 |
|------|--------------|----------|
| 签名长度 | 64 字节 | 256 字节 |
| 验证速度 | 快 (~100ms on M4) | 慢 (~500ms on M4) |
| 密钥大小 | 32 字节（私钥） | 256 字节（私钥） |
| 安全性 | 128-bit | 112-bit |
| 推荐度 | ⭐⭐⭐ | ⭐ |

对于嵌入式系统，**ECDSA secp256r1 (P-256)** 是最佳选择。

#### 14.4 固件镜像格式（标准做法）

```
┌──────────────────────────────────────┐
│           Firmware Image             │
├──────────────────────────────────────┤
│  Header:                             │
│    magic       = 0xCAFEBABE         │  4 bytes
│    image_size  = 51200              │  4 bytes
│    version     = 1.0.0              │  4 bytes
│    signature_size = 64              │  4 bytes
│    pubkey_hash = SHA256(pubkey)     │  32 bytes (可选)
│  ──────────────────────────────────  │
│  Payload:                            │
│    firmware.bin (raw binary)        │  N bytes
│  ──────────────────────────────────  │
│  Signature:                          │
│    ECDSA signature over             │  64 bytes
│    (header + payload)               │
└──────────────────────────────────────┘
```

#### 14.5 在 BL 中集成签名验证

```c
// 标准实现结构
int bl_validate_image_secure(uint32_t addr)
{
    image_header_t *hdr = (image_header_t*)addr;

    // ① 检查 header magic
    if (hdr->magic != IMAGE_MAGIC) return -1;

    // ② 计算 SHA-256
    uint8_t hash[32];
    sha256_calc((uint8_t*)addr, hdr->image_size, hash);

    // ③ ECDSA 验证签名
    uint8_t *sig = (uint8_t*)(addr + hdr->image_size);
    int result = ecdsa_verify(hash, sig, hdr->signature_size, public_key);

    return (result == 0) ? 0 : -1;
}
```

**可用的轻量级密码库**：
- **micro-ecc** —— 最小化的 ECDSA 实现，适合 MCU
- **tinycrypt** —— Intel 维护的轻量级密码库
- **mbedTLS** —— ARM 维护，功能全面但体积较大

---

### 第15章 回滚（Rollback）机制

#### 15.1 本工程的回滚问题

**当前方案无法真正回滚**：

```
正常流程：
  Slot 0 (旧固件 v1.0)  Slot 1 (新固件 v1.1)
  ① 下载 v1.1 到 Slot 1
  ② Swap: 擦除 Slot 0 → 复制 v1.1 到 Slot 0  ← 旧固件 v1.0 已消失！
  ③ 如果 v1.1 不能正常工作 → 尝试回滚 → 回滚到哪？

实际情况：
  BL 尝试跳转到 Slot 0 → 如果 v1.1 损坏，跳转失败
  → BL 回到等待状态 → 设备变砖（除非通过 UART 重新下载）
```

代码中 `BOOT_STATE_ROLLBACK` 分支的处理只是改回 `NORMAL`，并没有实际恢复旧固件。

#### 15.2 标准做法：指针交换为何不能直接用 + MCUboot 实际方案

**为什么"改指针直接跳转"在传统 MCU 上行不通？**

传统嵌入式固件是**位置相关（Position-Dependent）**的——链接脚本决定了所有绝对地址：

```
APP 链接脚本：
  FLASH (rx) : ORIGIN = 0x08004000, LENGTH = 52K

编译后：
  函数 main()            → 绝对地址 0x08004100
  函数 HAL_UART_Transmit → 绝对地址 0x08005800
  常量 "Firmware v1.0"   → 绝对地址 0x08006A00
  中断向量表             → 绝对地址 0x08004000

如果把这个固件放到 0x08011000 执行：
  CPU 跳到 0x08011100 期待找到 main()
  → 但那里是 0xFF（空白 Flash）
  → HardFault 💀
```

Cortex-M 的 `LDR Rd, =label` 伪指令会生成绝对地址的常量池加载——槽位变了，所有这类寻址全部失效。

**MCUboot 实际怎么做？**

MCUboot **默认模式下仍然做物理 Swap**（不依赖 PIC），但 Swap 过程可恢复：

```
MCUboot 三个物理分区：
┌─────────────┐ 0x08000000
│  Bootloader │  16KB
├─────────────┤
│  Slot 0     │  52KB  ← BL 始终跳转到这里（固定的链接地址）
├─────────────┤
│  Slot 1     │  52KB  ← 下载区
├─────────────┤
│  Scratch    │  8KB   ← 交换暂存区（关键！）
├─────────────┤
│  Metadata   │  8KB
└─────────────┘

Swap 流程（逐 Sector，掉电可恢复）：
  ┌──────────────────────────────────────────────────┐
  │ ① 写状态 status = { 源:Slot1, 目标:Slot0,       │
  │                     sector:0, 阶段:copying }     │
  │ ② 将 Slot0 Sector0 借 Scratch 暂存              │
  │ ③ 擦除 Slot0 Sector0                             │
  │ ④ 将 Slot1 Sector0 复制到 Slot0 Sector0          │
  │ ⑤ 更新 status.sector = 1                         │  ← 掉电续传点
  │ ⑥ 重复 ②~⑤ 直到全部完成                          │
  │ ⑦ status = "idle"                                │
  └──────────────────────────────────────────────────┘

掉电恢复：
  上电 → BL 读 status → "正在 swap sector=5"
  → Slot 1 Sector5 数据还在 → 从 Sector5 继续
  → 不会丢失数据，不会变砖
```

**为什么这比本工程当前的"全擦后全写"更安全？**

| 对比 | 本工程（当前实现） | MCUboot 标准做法 |
|------|-------------------|-----------------|
| Swap 方式 | 一次性擦除全部 Slot0 → 逐块复制 | 逐 Sector 擦除 + 状态追踪 |
| 掉电后 | Slot0 被破坏，Slot1 完整但无法恢复 | 从断点继续，Slot1 数据完好 |
| 回滚能力 | 旧固件已消失，无法回滚 | 旧固件在最后 sector 被覆盖前可恢复 |
| 额外存储 | 无 | 需要 Scratch 区 |

**结论**：在 STM32L4 这类不支持硬件重映射的 MCU 上，**容错 Swap（带状态追踪的逐扇区交换）是唯一可行的双槽 + 可回滚方案**。MCUboot 的 "Swap using scratch" 和 "Overwrite only" 两种模式都是这个思路的不同实现。

#### 15.3 本工程可落地的改进方案

基于当前分区（无 Scratch 区）的最小改动方案——补强 Swap 的掉电保护：

```c
// 在 Bootloader 的 Metadata 结构中增加 swap 进度字段
typedef struct __attribute__((packed, aligned(8))) {
    // ... 原有字段 ...
    uint8_t  swap_state;     // 0=idle, 1=swapping
    uint32_t swap_offset;    // 已复制的字节偏移，掉电恢复起点
    uint32_t swap_size;      // 总共需要复制的大小
    // ...
} boot_meta_t;

// 改进后的 Swap 流程
int bl_swap_slot1_to_slot0(uint32_t size)
{
    boot_meta_t meta;
    meta_load(&meta);

    // 检查是否有未完成的 Swap（上次掉电中断）
    uint32_t start_offset = 0;
    if (meta.swap_state == 1) {
        // 恢复上次未完成的 Swap
        start_offset = meta.swap_offset;
        util_uart_printf("[BL] Resuming swap from offset %u\r\n",
                         (unsigned)start_offset);
    }

    meta.swap_state = 1;
    meta.swap_size = size;

    uint32_t offset = start_offset;
    while (offset < size) {
        // 每完成一个 Page 的复制就更新进度
        uint32_t chunk = /* ... */;

        // 擦除和复制操作
        flash_part_erase(slot0, offset, chunk);
        flash_part_write(slot0, offset, chunk, buf);

        offset += chunk;

        // 记录进度（掉电后可从此处恢复）
        meta.swap_offset = offset;
        meta_save(&meta);  // 注意：这会再次擦除+写入 Metadata
    }

    // Swap 全部完成
    meta.swap_state = 0;
    meta.swap_offset = 0;
    meta_save(&meta);
    return 0;
}
```

**⚠️ 注意**：频繁调用 `meta_save()` 会导致 Metadata 区域快速磨损（每 256B 就擦写一次是 52K/256 = 200 次擦除）。更实际的粒度是**每 Page（2KB）记录一次**——总共 26 次写入，可控且能保证每 Page 粒度恢复。

**如果愿意增加 Scratch 分区**（从当前 Metadata 8KB 中分出 2KB），可以直接实现 MCUboot 的标准 Swap 算法，彻底消除掉电风险。

#### 15.4 版本防回退

即使有回滚机制，某些场景下需要**禁止回退到旧版本**（安全原因，旧版本可能存在已知漏洞）：

**方案一：硬件计数器**

STM32L4 有 OTP（一次性可编程）区域，可以写入单调递增的版本计数器：

```c
// 在 OTA commit 时写入
uint32_t min_version = read_otp_min_version();
if (new_version < min_version) {
    return ERR_VERSION_DOWNGRADE;  // 拒绝安装更旧的版本
}
// 如果新版本更高，烧录新的 OTP 位
if (new_version > min_version) {
    program_otp_min_version(new_version);
}
```

**方案二：eFuse / Anti-rollback Counter**

对于有安全要求的场景，使用硬件安全模块中的计数器。

---

### 第16章 掉电保护与看门狗

#### 16.1 掉电风险分析

OTA 过程中任何时刻都可能断电。需要考虑最坏情况：

```
阶段          操作                       掉电后果
────────────────────────────────────────────────────
下载中        写入 Slot 1               Slot 1 数据不完整
              更新 CRC                  → BL 发现 CRC 错误
                                       → 不影响 Slot 0，相对安全

Swap 擦除     擦除 Slot 0 的一部分     Slot 0 被破坏！
              → 旧固件部分丢失          → 变砖！（最危险）

Swap 复制     写入 Slot 0 的一部分     Slot 0 不完整
                                       → 旧固件已丢，新固件不完整
                                       → 变砖！

Metadata 写入  擦除 Metadata Page      Metadata 损坏
              写入 Metadata            → 有双副本，可以恢复
                                       → 相对安全
```

**关键发现**：Swap 复制过程是最危险的！

#### 16.2 改进方案对比：减少掉电风险窗口

```
方案对比：

本工程当前方案（一次性刷写）：
  操作：擦除全部 52KB Slot0 → 逐块写入 52KB 新固件
  时间：~1-2 秒（Flash 操作慢）
  风险窗口：全在这 2 秒内 → 任何时刻掉电都变砖
  恢复能力：无（如果没有从 UART 恢复的机制）

容错 Swap（带进度追踪）：
  操作：逐页(2KB)擦除+写入 + 每页保存进度
  时间：~1-2 秒（实际 Flash 操作时间相同）
  风险窗口：每次进度保存之间的 ~20ms（单页操作时间）
  恢复能力：掉电后从上次保存的进度继续

MCUboot 标准 Swap（带 Scratch）：
  操作：逐 Sector 三向交换 + 状态追踪
  时间：~1-2 秒
  风险窗口：每 Sector 操作间 ~50ms
  恢复能力：完整可恢复，任何时刻掉电都安全
```

**核心思想**：没有必要追求"不 Swap"（这需要 PIC 或硬件重映射，不适用于 STM32L4），而是让 Swap 过程**由不可逆变可逆**。

#### 16.3 看门狗集成

长时间 Flash 操作期间，必须不断**喂狗**防止看门狗复位：

```c
// Swap 过程中喂狗
int bl_swap_slot1_to_slot0(uint32_t size)
{
    while (offset < size) {
        // 每个 chunk 写入后喂狗
        flash_part_write(slot0, offset, chunk_size, buf);

        // 喂狗
        HAL_IWDG_Refresh(&hiwdg);  // 或 IWDG->KR = 0xAAAA

        offset += chunk_size;
    }
}
```

**⚠️ 本工程没有集成看门狗** —— 如果 Swap 过程死循环（例如 Flash 操作卡住），系统永远不会恢复。

**标准做法**：
1. BL 启动时启用看门狗（超时 ~5-10 秒）
2. 长时间操作中定期喂狗
3. 跳转前禁用或重置看门狗
4. APP 启动后重新配置看门狗

#### 16.4 最低可恢复状态

设计的一个核心原则是：**即使在任何操作中途断电，上电后必须能回到可工作的状态**。

```
状态转移    最低要求
────────────────────────────────────────────────
NORMAL      Slot 0 完整 + Metadata 有效
  │
  ↓ 开始 OTA 下载
  │
RECEIVING   Slot 0 完整         ← 旧固件还在
  │          Slot 1 部分写入    ← 不影响
  │
  ↓ OTA 完成
  │
DONE        Slot 0 完整         ← 还没 Swap
  │          Slot 1 完整         ← 新固件已验证
  │          Metadata 有效       ← 标记 SWAP
  │
  ↓ Swap
  │
SWAPPING    ⚠️ Slot 0 可能损坏  ← 这是危险状态！
  │          ⚠️ Slot 1 完整      ← 备份还在
  │
```

**关键设计**：如果 Swap 中断，应该能**从中断点继续 Swap**（Slot 1 数据完好），而不是依赖 Slot 0 的完整性。

**含 Scratch 区的标准 Swap 流程**：

```
① 新固件下载到 Slot 1，验证通过
② BL 逐扇区执行三向交换：Slot0[sector] → Scratch → Slot1[sector] → Slot0[sector]，每步记录状态
③ 掉电恢复：读状态 → 从上次完成的 sector 继续
④ 全部完成 → 跳转到 Slot 0（固定链接地址）
```

见第15章的具体实现细节。

---

### 第17章 差分升级（Delta OTA）

#### 17.1 什么是差分升级

传统 OTA 传输整个固件（如 52KB），差分升级只传输**新旧固件的差异**。

```
全量 OTA:        传输 52,000 字节
差分 OTA:        传输 200 ~ 5,000 字节  (取决于改动量)

压缩比：         ~10:1 到 ~100:1
```

**适用场景**：
- 窄带 IoT（NB-IoT、LoRa）—— 带宽极其有限
- 大固件（数百 KB 到 MB 级别）
- 频繁小更新

#### 17.2 基本原理

```
旧固件 (v1.0)        新固件 (v1.1)      差分 (patch)
┌──────────┐        ┌──────────┐        ┌──────────────┐
│ func_a() │ 相同 → │ func_a() │        │ COPY 100, 50 │
│ ...50B   │        │ ...50B   │        │ (从旧固件     │
├──────────┤        ├──────────┤        │  偏移100复制  │
│ func_b() │ 修改 → │ func_b() │        │  50字节)      │
│ ...60B   │        │ ...70B   │        ├──────────────┤
├──────────┤        ├──────────┤        │ ADD 10B "..." │
│ (无)     │ 新增 → │ func_c() │        │ (新增10字节)  │
└──────────┘        └──────────┘        └──────────────┘
```

**MCU 端执行**：
1. 收到 patch 数据
2. 从 Slot 0 读取旧固件
3. 按 patch 指令（COPY / ADD / ...）重建新固件
4. 写入 Slot 1

#### 17.3 业界工具

| 工具 | 提供者 | 特点 |
|------|--------|------|
| **Joey (Jojodiff)** | jojodiff.org | 专为嵌入式的差分算法，极低 RAM |
| **Hdiffpatch** | GitHub | 大文件差分，高压缩比 |
| **bsdiff/bspatch** | Colin Percival | 经典差分工具，需较多 RAM |
| **MCUboot + SUIT** | Zephyr/IETF | 标准化固件更新格式 |

小内存 MCU（如 STM32L4 48KB RAM）推荐 Joey，它可以在几 KB RAM 内完成 patch。

---

### 第18章 生产烧录与合并工具

#### 18.1 合并固件

首次烧录需要将 BL 和 APP 合并成一个二进制文件：

```python
# merge_hex.py 的核心逻辑
def merge_bin(bl_path, app_path, output_path):
    # 创建 128KB 的空白镜像（填充 0xFF = 已擦除 Flash）
    merged = bytearray(b'\xFF' * 128 * 1024)

    # BL 放在 0x08000000
    merged[0x00000 : 0x00000 + len(bl_data)] = bl_data

    # APP 放在 0x08004000 (Slot 0)
    merged[0x04000 : 0x04000 + len(app_data)] = app_data

    # 写入文件
    with open(output_path, 'wb') as f:
        f.write(merged[:0x04000 + len(app_data)])
```

#### 18.2 烧录命令

```bash
# 使用 STM32CubeProgrammer CLI
STM32_Programmer_CLI \
    -c port=SWD \
    -w build/merged_firmware.bin 0x08000000 \
    -v \           # 验证写入
    -rst           # 烧录后复位
```

#### 18.3 量产考虑

**安全烧录**：
- 首次烧录时插入设备唯一的密钥对（用于后续安全 OTA 签名验证）
- OTP 区域写入设备序列号、安全配置

**写入保护**：
```c
// 量产时设置 BL 区域写保护（防止被意外擦除）
HAL_FLASH_OB_Unlock();

FLASH_OBProgramInitTypeDef ob = {0};
ob.OptionType = OPTIONBYTE_WRP;
ob.WRPArea = OB_WRPAREA_BANK1_AREAA;  // Pages 0-7 (BL)
ob.WRPEndOffset = 0x07;
HAL_FLASHEx_OBProgram(&ob);

HAL_FLASH_OB_Launch();  // 复位后生效
HAL_FLASH_OB_Lock();
```

---

## 第五部分：本工程实战分析

### 第19章 工程架构评价

#### 19.1 架构优点（精华）

| 优点 | 具体体现 | 评价 |
|------|---------|------|
| **分层清晰** | Layer 1-4：Flash抽象 → Metadata → OTA逻辑 → 传输协议 | ⭐⭐⭐ 教科书级别的分层 |
| **独立构建** | BL 和 APP 有各自的 CMakeLists.txt 和链接脚本 | ⭐⭐⭐ 标准工程实践 |
| **双副本 Metadata** | Primary + Backup 自动修复 | ⭐⭐⭐ 鲁棒性好 |
| **状态机驱动** | NORMAL/SWAP/TESTING/ROLLBACK 四态模型 | ⭐⭐ 方向正确，但实现有缺陷 |
| **Flash 抽象** | flash_device + flash_partition + 参数校验 | ⭐⭐⭐ 可移植、可测试 |
| **协议设计** | SOF/CMD/SEQ/CRC 帧格式 + 状态机解析 | ⭐⭐⭐ 工业级协议基础 |
| **Python 上位机** | 注释详尽的 OTA 客户端工具 | ⭐⭐ 实用，但缺异常恢复 |
| **CRC 分层** | CRC-16 帧校验 + CRC-32 固件校验 + CRC-32 Metadata 校验 | ⭐⭐⭐ 多层防护 |
| **写缓冲设计** | 256B 缓冲解决 Flash 对齐问题 | ⭐⭐ 正确的工程方法 |
| **增量 CRC** | 边接收边计算 CRC32，无需缓存整个固件 | ⭐⭐⭐ 内存高效 |

#### 19.2 代码质量问题（糟粕）

| 问题 | 位置 | 严重性 | 说明 |
|------|------|--------|------|
| **代码重复** | `bl_meta.c/h` 在 `Bootloader/Src/` 和 `Components/OTA/` 各一份 | 中 | 应共享一份源文件 |
| **无签名验证** | 整个系统 | 🔴 高 | 任何串口都能刷任意固件 |
| **假回滚** | `bl_core.c:ROLLBACK` | 🔴 高 | 有状态，无实际回滚能力 |
| **硬编码 VTOR** | `system_stm32l4xx.c:141` | 中 | BL 被迫手动矫正 |
| **无看门狗** | 全局 | 中 | Swap 卡死无法恢复 |
| **无版本防退** | `ota_core.c` | 中 | 可降级到有漏洞的旧版本 |
| **Swap 无保护** | `bl_core.c:50` | 🔴 高 | 擦除+复制过程中断电→变砖 |
| **全局 OTA 上下文** | `ota_proto.c:35` `extern g_ota_ctx` | 低 | 不便于多实例 |
| **初始化 CRC 魔法数** | `ota_core.c:89` | 低 | `0xFFFFFFFFU ^ 0xFFFFFFFFU` 不如直接写 `0` |
| **缺少超时处理** | BL 端 | 低 | BL 等待升级时无限阻塞 |
| **进度计算闪烁** | `bl_core.c:85-89` | 低 | 10% 增量的方式可能跳过某些百分比 |

#### 19.3 与业界标准对比

| 维度 | 本工程 | 标准做法（MCUboot） | 差距 |
|------|--------|-------------------|------|
| 分区方案 | 双槽 + 一次性复制式 Swap | 双槽 + 容错 Swap（逐扇区，状态追踪） | 关键差距 |
| 安全启动 | 无 | ECDSA 签名验证 | 缺失 |
| 镜像格式 | 自定义头 + 原始二进制 | TLV 格式 + Manifest | 缺失 |
| 升级方式 | 全量 + 串口 | 全量/差分 + 多种传输 | 可扩展 |
| 加密 | 无 | AES-ECB/CTR | 缺失 |
| 回滚 | 假回滚（旧固件已被覆盖） | 物理交换完成前可恢复 | 关键差距 |
| 掉电保护 | 部分（Metadata 双副本） | 完整（Swap 过程可断点续传） | 关键差距 |
| 看门狗 | 无 | 内置 | 缺失 |
| 版本防退 | 无 | 安全计数器 | 缺失 |
| 构建系统 | CMake 独立构建 | CMake + Kconfig | 相当 |
| 文档 | README（不错） | 完整文档 + spec | 可用 |

---

### 第20章 改进建议清单

按优先级排序的改进建议：

#### 🔴 高优先级（影响安全性/可靠性）

1. **将 Swap 改为可恢复的逐扇区交换**
   - Metadata 中增加 `swap_state`/`swap_offset` 字段，记录交换进度
   - 每次只处理一个 Page (2KB)，完成后立即更新进度
   - 掉电后从上次记录的进度恢复
   - 可将 Metadata 的 8KB 中分出 2KB 作 Scratch（实现 MCUboot 标准交换算法）
   - **不改变"从 Slot 0 运行"的模型，无需 PIC 编译**

2. **增加固件签名验证**
   - 集成 micro-ecc 库
   - 在 OTA 头和 BL 验证中增加 ECDSA 签名验证
   - 至少实现 ECDSA-P256

3. **修复回滚机制**
   - 当前方案下：可在 OTA Commit 前将 Slot 0 备份到预留的 Scratch 区
   - 本质改进：实施第 1 条的容错 Swap 后，Swap 未完成时 Slot 1 数据完好 → 自然可恢复

#### 🟡 中优先级（改善健壮性）

4. **集成看门狗**
   - BL 启动时启用 IWDG (~10s)
   - Swap 循环中定期喂狗
   - 跳转前重置看门狗

5. **消除代码重复**
   - 将 `bl_meta.c/h` 统一放在 `Components/OTA/`
   - BL 的 CMakeLists.txt 引用共享位置

6. **区分 BL 和 APP 的 VTOR 配置**
   - 使用编译宏 `-DBOOTLOADER_BUILD` 区分
   - 移除 `bl_main.c` 中的手动 VTOR 矫正 hack

7. **BL 增加超时机制**
   - 如果 APP 无效，BL 等待升级超时后自动重新尝试验证

#### 🟢 低优先级（提升工程品质）

8. **Metadata 区域缩小为 1 Page (2KB)**
   - 释放 6KB 给 APP 使用

9. **镜像格式标准化**
   - 参考 MCUboot 的 image format
   - 使用 TLV 格式存放版本、签名等信息

10. **增加差分升级支持**
    - 集成 Joey (jojodiff) 库
    - Python 端生成 patch，MCU 端应用 patch

11. **CRC 初始化表达清晰化**
    - 定义 `#define CRC32_INITIAL_VALUE 0x00000000U` 替代魔法数

12. **Python 上位机改进**
    - ACK 中验证 SEQ 与发送的 SEQ 一致
    - 增加波特率自动检测
    - 中断恢复（记录已发送的块，下次从断点继续）

---

## 附录

### A. 术语表

| 术语 | 英文 | 说明 |
|------|------|------|
| BL | Bootloader | 引导加载程序 |
| OTA | Over-The-Air | 空中升级（广义：远程升级） |
| VTOR | Vector Table Offset Register | 向量表偏移寄存器 |
| MSP | Main Stack Pointer | 主堆栈指针 |
| CRC | Cyclic Redundancy Check | 循环冗余校验 |
| SOF | Start of Frame | 帧起始标识 |
| SEQ | Sequence Number | 序列号 |
| Metadata | —— | 持久化的启动状态信息 |
| Slot | —— | 固件存储的逻辑分区 |
| SWD | Serial Wire Debug | ARM 调试接口 |
| HSI | High-Speed Internal | 内部高速振荡器 |
| HSE | High-Speed External | 外部高速晶振 |

### B. 关键文件索引

| 文件 | 功能 |
|------|------|
| `Bootloader/Src/bl_main.c` | BL 入口、最小初始化 |
| `Bootloader/Src/bl_core.c` | BL 核心：验证/跳转/状态机/Swap |
| `Bootloader/Src/bl_meta.c` | 元数据读写（BL 侧副本） |
| `Bootloader/STM32L431_BL.ld` | BL 链接脚本（16KB @ 0x08000000） |
| `Components/OTA/flash_port.c` | Flash 硬件抽象+分区管理 |
| `Components/OTA/bl_meta.c` | 元数据读写（APP 侧副本） |
| `Components/OTA/ota_core.c` | OTA 状态机+固件接收+CRC |
| `Components/OTA/ota_proto.c` | 传输协议：帧解析+命令分发 |
| `Core/Src/main.c` | APP 入口 |
| `Core/Src/system_stm32l4xx.c` | CMSIS 系统初始化（含 VTOR） |
| `STM32L431XX_FLASH.ld` | APP 链接脚本（52KB @ 0x08004000） |
| `scripts/ota_send.py` | Python 上位机 OTA 工具 |
| `scripts/merge_hex.py` | BL+APP 合并工具 |

### C. 参考资源

- **MCUboot**：https://github.com/mcu-tools/mcuboot —— Zephyr RTOS 的标准 Bootloader
- **STM32L431 Reference Manual (RM0394)**：Flash 编程章节
- **ARM Cortex-M4 Technical Reference Manual**：VTOR、中断管理
- **IETF SUIT**：https://datatracker.ietf.org/wg/suit/about/ —— 标准化固件更新格式
- **micro-ecc**：https://github.com/kmackay/micro-ecc —— 嵌入式 ECDSA 库
- **CRC 目录**：https://reveng.sourceforge.io/crc-catalogue/ —— 各种 CRC 参数验证

---

> **编写说明**：本文档基于 `STM32L4_Core` 工程（`OTA` 分支，commit `250e793`）的代码分析编写。
> 文档中标注为"标准做法"的内容是业界普遍采用的最佳实践，标注为"⚠️"的内容是本工程需要改进的地方。
> 建议将本工程作为学习 Bootloader/OTA 概念的起点，在生产环境中补全安全签名的完整链路。
