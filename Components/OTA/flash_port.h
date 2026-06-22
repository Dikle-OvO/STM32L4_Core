#ifndef __FLASH_PORT_H
#define __FLASH_PORT_H

#include <stdint.h>
#include <stddef.h>
#include "stm32l4xx_hal.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * STM32L431CBT6 Flash Characteristics
 * ============================================================================
 * Total Flash:       128KB (0x08000000 - 0x0801FFFF)
 * Page Size:         2KB (2048 bytes)
 * Write Granularity: 8 bytes (double word)
 * Total Pages:       64
 * Erase Value:       0xFF
 * ========================================================================= */

#define FLASH_BASE_ADDR         0x08000000U
#define FLASH_TOTAL_SIZE        (128U * 1024U)
#define FLASH_PAGE_SIZE_BYTES   2048U
#define FLASH_WRITE_SIZE        8U
#define FLASH_TOTAL_PAGES       64U
#define FLASH_ERASE_VALUE       0xFFU

/* ============================================================================
 * Partition Layout (128KB)
 * ============================================================================
 * | Partition    | Pages  | Start Addr | End Addr   | Size  |
 * |--------------|--------|------------|------------|-------|
 * | Bootloader   | 0-7    | 0x08000000 | 0x08003FFF | 16KB  |
 * | Slot 0 (APP) | 8-33   | 0x08004000 | 0x08010FFF | 52KB  |
 * | Slot 1 (OTA) | 34-59  | 0x08011000 | 0x0801DFFF | 52KB  |
 * | Swap Status  | 60-61  | 0x0801E000 | 0x0801EFFF |  4KB  |
 * | Metadata     | 62-63  | 0x0801F000 | 0x0801FFFF |  4KB  |
 * ========================================================================= */

#define PART_BOOT_OFFSET        0x00000000U
#define PART_BOOT_SIZE          (16U * 1024U)

#define PART_SLOT0_OFFSET       0x00004000U
#define PART_SLOT0_SIZE         (52U * 1024U)

#define PART_SLOT1_OFFSET       0x00011000U
#define PART_SLOT1_SIZE         (52U * 1024U)

#define PART_SWAP_STATUS_OFFSET 0x0001E000U
#define PART_SWAP_STATUS_SIZE   (4U * 1024U)

#define PART_META_OFFSET        0x0001F000U
#define PART_META_SIZE          (4U * 1024U)

/* Error codes */
#define FLASH_OK                0
#define FLASH_ERR_PARAM         (-1)
#define FLASH_ERR_ALIGN         (-2)
#define FLASH_ERR_RANGE         (-3)
#define FLASH_ERR_ERASE         (-4)
#define FLASH_ERR_WRITE         (-5)
#define FLASH_ERR_LOCK          (-6)

/* ============================================================================
 * Flash Device (底层驱动抽象)
 * ========================================================================= */
struct flash_device {
    uint32_t base_addr;         /* Flash 起始物理地址 */
    uint32_t total_size;        /* Flash 总大小 (bytes) */
    uint32_t page_size;         /* 擦除粒度 (bytes) */
    uint32_t write_size;        /* 写入粒度 (bytes) */

    /* 底层操作函数指针 */
    int (*read)(uint32_t addr, uint32_t size, void *dst);
    int (*write)(uint32_t addr, uint32_t size, const void *src);
    int (*erase)(uint32_t addr, uint32_t size);
};

/* ============================================================================
 * Flash Partition (分区抽象)
 * ========================================================================= */
struct flash_partition {
    const struct flash_device *flash;   /* 底层驱动 */
    uint32_t offset;                    /* 分区起始偏移 (相对于 flash base) */
    uint32_t size;                      /* 分区大小 (bytes) */
};

/* ============================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief  从分区读取数据
 * @param  part: 分区指针
 * @param  offset: 分区内偏移
 * @param  size: 读取字节数
 * @param  dst: 目标缓冲区
 * @return FLASH_OK 成功, 负值为错误码
 */
int flash_part_read(const struct flash_partition *part,
                    uint32_t offset, uint32_t size, void *dst);

/**
 * @brief  向分区写入数据 (必须先擦除, 写入需 8 字节对齐)
 * @param  part: 分区指针
 * @param  offset: 分区内偏移 (必须 8 字节对齐)
 * @param  size: 写入字节数 (必须 8 字节对齐)
 * @param  src: 源数据缓冲区
 * @return FLASH_OK 成功, 负值为错误码
 */
int flash_part_write(const struct flash_partition *part,
                     uint32_t offset, uint32_t size, const void *src);

/**
 * @brief  擦除分区中的区域 (必须按 page 对齐)
 * @param  part: 分区指针
 * @param  offset: 分区内偏移 (必须 page 对齐)
 * @param  size: 擦除字节数 (必须 page 对齐)
 * @return FLASH_OK 成功, 负值为错误码
 */
int flash_part_erase(const struct flash_partition *part,
                     uint32_t offset, uint32_t size);

/**
 * @brief  获取预定义分区
 */
const struct flash_partition *flash_get_partition_boot(void);
const struct flash_partition *flash_get_partition_slot0(void);
const struct flash_partition *flash_get_partition_slot1(void);
const struct flash_partition *flash_get_partition_swap_status(void);
const struct flash_partition *flash_get_partition_meta(void);

/**
 * @brief  获取底层 flash 设备
 */
const struct flash_device *flash_get_device(void);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_PORT_H */
