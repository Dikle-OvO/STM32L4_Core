#ifndef __BL_META_H
#define __BL_META_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * OTA Metadata Structure
 * ============================================================================
 * 存储在 Metadata 分区，两份冗余 (primary + backup)
 * 每份占一个 page (2KB)，实际使用前 64 字节
 * ========================================================================= */

#define META_MAGIC              0x4F54414DU  /* "OTAM" */
#define META_VERSION            1U

/* Boot 状态 */
#define BOOT_STATE_NORMAL   0x00U     /* 正常启动 Slot 0 */
#define BOOT_STATE_SWAP     0x01U     /* 需要从 Slot 1 拷贝到 Slot 0 */
#define BOOT_STATE_TESTING  0x02U     /* 新固件测试中 (等待 APP 确认) */
#define BOOT_STATE_ROLLBACK 0x03U     /* 回滚到旧固件 */

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
 * API
 * ========================================================================= */

/**
 * @brief  从 Metadata 分区读取有效元数据 (自动校验 CRC, 主/备冗余)
 * @param  meta: 输出元数据
 * @return 0 成功, -1 两份均损坏
 */
int meta_load(boot_meta_t *meta);

/**
 * @brief  写入元数据到 Metadata 分区 (同时写主/备, 自动计算 CRC)
 * @param  meta: 待写入元数据
 * @return 0 成功, 负值错误
 */
int meta_save(boot_meta_t *meta);

/**
 * @brief  初始化默认元数据 (首次启动或全损坏时调用)
 * @param  meta: 输出元数据
 */
void meta_init_default(boot_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif /* __BL_META_H */
