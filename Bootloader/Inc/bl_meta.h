#ifndef __BL_META_H
#define __BL_META_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * OTA Metadata Structure
 * ============================================================================
 * 存储在 Metadata 分区 (4KB = 2 pages)，两份冗余 (primary + backup)
 * 每份占一个 page (2KB)，实际使用前 64 字节
 * ========================================================================= */

#define META_MAGIC              0x4F54414DU  /* "OTAM" */
#define META_VERSION            2U

/* Boot 状态 */
#define BOOT_STATE_NORMAL   0x00U     /* 正常启动 Slot 0 */
#define BOOT_STATE_SWAP     0x01U     /* 需要交换 Slot 0 ↔ Slot 1 */
#define BOOT_STATE_SWAPPING 0x02U     /* 交换进行中 (掉电可恢复) */
#define BOOT_STATE_TESTING  0x03U     /* 新固件测试中 (等待 APP 确认) */
#define BOOT_STATE_ROLLBACK 0x04U     /* 需要回滚: 再次交换回旧固件 */

/* 元数据结构 (64 bytes, 8-byte aligned) */
typedef struct __attribute__((packed, aligned(8))) {
    uint32_t magic;                 /* 魔数 META_MAGIC */
    uint32_t version;               /* 元数据格式版本 */
    uint8_t  boot_state;            /* 当前启动状态 */
    uint8_t  boot_count;            /* 测试启动计数 (TESTING 模式下递增) */
    uint8_t  max_boot_count;        /* 最大测试次数, 超过则回滚 */
    uint8_t  reserved0;
    uint32_t app_size;              /* APP 固件大小 (bytes) */
    uint32_t app_crc32;             /* APP 固件 CRC32 */
    uint32_t app_version;           /* APP 版本号 */
    uint32_t ota_size;              /* OTA 固件大小 (Slot1 中) */
    uint32_t ota_crc32;             /* OTA 固件 CRC32 */
    uint32_t ota_version;           /* OTA 版本号 */
    uint32_t meta_crc32;            /* 元数据本身的 CRC (不含此字段) */
    uint8_t  padding[20];           /* 填充到 64 字节 */
} boot_meta_t;

_Static_assert(sizeof(boot_meta_t) == 64, "boot_meta_t must be 64 bytes");

/* ============================================================================
 * Swap Status (4KB = 2 pages, append-only log for power-safe page copy)
 * ============================================================================
 * Layout:
 *   [0]     swap_header_t   (8 bytes) — written when swap begins
 *   [1..N]  swap_marker_t   (8 bytes each) — one per page copied
 *
 * 掉电恢复: BL 读取已完成的 marker 数量, 从断点继续拷贝
 * ========================================================================= */

#define SWAP_MAGIC              0x53575021U  /* "SWP!" */

/*
 * 每页交换 3 步 (RAM 做中转):
 *   step 0: 读 Slot0[page] → RAM, 擦 Slot0[page], 写 Slot1[page] → Slot0[page]
 *   step 1: 擦 Slot1[page], 写 RAM → Slot1[page]
 *   step 2: 该页交换完成
 *
 * swap_marker_t.step 记录已完成的最后一步:
 *   0xFF (erased)  = 未开始
 *   STEP_SLOT0_DONE = step 0 完成: Slot0 已写入新数据, 旧数据在 RAM
 *   STEP_SLOT1_DONE = step 1 完成: Slot1 已写入旧数据
 *   STEP_PAGE_DONE  = step 2 完成: 该页交换彻底完成
 */
#define SWAP_STEP_SLOT0_DONE    0x01U
#define SWAP_STEP_SLOT1_DONE    0x02U
#define SWAP_STEP_PAGE_DONE     0x03U

typedef struct __attribute__((packed, aligned(8))) {
    uint32_t magic;             /* SWAP_MAGIC */
    uint16_t total_pages;       /* 需要交换的总页数 */
    uint16_t reserved;
} swap_header_t;

typedef struct __attribute__((packed, aligned(8))) {
    uint16_t page_index;        /* 页号 */
    uint8_t  step;              /* 完成步骤 */
    uint8_t  reserved[5];
} swap_marker_t;

_Static_assert(sizeof(swap_header_t) == 8, "swap_header_t must be 8 bytes");
_Static_assert(sizeof(swap_marker_t) == 8, "swap_marker_t must be 8 bytes");

/* ============================================================================
 * Metadata API
 * ========================================================================= */

int meta_load(boot_meta_t *meta);
int meta_save(boot_meta_t *meta);
void meta_init_default(boot_meta_t *meta);

/* ============================================================================
 * Swap Status API (Bootloader only)
 * ========================================================================= */

/**
 * @brief  初始化 swap status 区域 (擦除 + 写入 header)
 * @param  total_pages: 需要拷贝的总页数
 * @return 0 成功, 负值错误
 */
int swap_status_init(uint16_t total_pages);

/**
 * @brief  读取 swap 进度
 * @param  total_pages: 输出总页数
 * @param  done_pages: 输出已完全交换完成的页数
 * @param  last_step: 输出第一个未完成页的最后 step (0xFF=未开始)
 * @return 0 成功, -1 无有效 swap header
 */
int swap_status_read_progress(uint16_t *total_pages, uint16_t *done_pages,
                              uint8_t *last_step);

/**
 * @brief  标记一页的某个步骤完成
 * @param  page_index: 页号
 * @param  step: 完成的步骤 (SWAP_STEP_xxx)
 * @return 0 成功, 负值错误
 */
int swap_status_mark_step(uint16_t page_index, uint8_t step);

#ifdef __cplusplus
}
#endif

#endif /* __BL_META_H */
