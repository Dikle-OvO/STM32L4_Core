#ifndef __OTA_CORE_H
#define __OTA_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Layer 3: OTA 业务逻辑 (ECU/APP 侧)
 * ============================================================================
 * 职责:
 * - 接收固件数据块，缓冲并写入 Slot 1
 * - 管理 OTA 状态机 (空闲→接收→验证→完成)
 * - 固件完整性校验 (CRC32)
 * - 触发 reboot 前设置 metadata 为 SWAP 状态
 * ========================================================================= */

/* ============================================================================
 * OTA 固件包头 (由 Host 侧打包工具生成, 传输前首先发送)
 * ========================================================================= */
#define OTA_HEADER_MAGIC        0x4F544148U  /* "OTAH" */

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* OTA_HEADER_MAGIC */
    uint32_t fw_version;        /* 固件版本号 */
    uint32_t fw_size;           /* 固件总大小 (bytes, 不含此头) */
    uint32_t fw_crc32;          /* 固件数据 CRC32 (不含此头) */
    uint32_t header_crc32;      /* 此头的 CRC32 (不含本字段) */
} ota_header_t;

/* ============================================================================
 * OTA 状态
 * ========================================================================= */
typedef enum {
    OTA_STATE_IDLE = 0,         /* 空闲, 等待开始 */
    OTA_STATE_HEADER,           /* 等待/解析固件头 */
    OTA_STATE_RECEIVING,        /* 接收固件数据中 */
    OTA_STATE_VERIFYING,        /* 接收完毕, 正在验证 */
    OTA_STATE_DONE,             /* 验证通过, 等待 reboot */
    OTA_STATE_ERROR,            /* 出错 */
} ota_state_t;

/* ============================================================================
 * OTA 错误码
 * ========================================================================= */
#define OTA_OK                  0
#define OTA_ERR_STATE           (-1)    /* 状态不对 */
#define OTA_ERR_HEADER          (-2)    /* 头部校验失败 */
#define OTA_ERR_SIZE            (-3)    /* 固件过大 */
#define OTA_ERR_FLASH           (-4)    /* Flash 操作失败 */
#define OTA_ERR_CRC             (-5)    /* 固件 CRC 校验失败 */
#define OTA_ERR_VERSION         (-6)    /* 版本号不合法 */

/* ============================================================================
 * OTA 上下文 (APP 侧维护)
 * ========================================================================= */
typedef struct {
    ota_state_t state;
    ota_header_t header;            /* 已解析的固件头 */
    uint32_t received_size;         /* 已接收字节数 */
    uint32_t write_offset;          /* Slot1 中下一次写入偏移 */
    uint32_t running_crc;           /* 接收数据的 CRC 累计 */

    /* 写缓冲: 凑够 write_buf_size 字节后刷入 Flash */
    uint8_t write_buf[256];         /* 必须是 FLASH_WRITE_SIZE(8) 的倍数 */
    uint16_t buf_used;              /* 缓冲中已用字节数 */
} ota_ctx_t;

/* ============================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief  初始化 OTA 上下文，进入空闲状态
 * @param  ctx: OTA 上下文
 */
void ota_init(ota_ctx_t *ctx);

/**
 * @brief  开始 OTA 流程, 状态从 IDLE → HEADER
 * @param  ctx: OTA 上下文
 * @return OTA_OK 或 OTA_ERR_STATE
 */
int ota_begin(ota_ctx_t *ctx);

/**
 * @brief  喂入固件头数据
 * @param  ctx: OTA 上下文
 * @param  header: 固件头 (由传输层解包后传入)
 * @return OTA_OK: 头部合法, 已擦除 Slot1, 进入 RECEIVING
 *         负值: 错误
 */
int ota_feed_header(ota_ctx_t *ctx, const ota_header_t *header);

/**
 * @brief  喂入固件数据块 (由传输层分块后逐块调用)
 * @param  ctx: OTA 上下文
 * @param  data: 数据指针
 * @param  len: 数据长度 (任意值, 内部缓冲对齐)
 * @return OTA_OK: 继续接收
 *         负值: 错误 (Flash写入失败等)
 */
int ota_feed_data(ota_ctx_t *ctx, const uint8_t *data, uint32_t len);

/**
 * @brief  通知数据接收完毕, 触发 CRC 校验
 * @param  ctx: OTA 上下文
 * @return OTA_OK: 校验通过, 状态变为 DONE
 *         OTA_ERR_CRC: CRC 不匹配
 *         OTA_ERR_SIZE: 大小不匹配
 */
int ota_finish(ota_ctx_t *ctx);

/**
 * @brief  确认升级: 写入 metadata 设置 SWAP 标记
 * @param  ctx: OTA 上下文
 * @return OTA_OK: metadata 已更新, 可安全 reboot
 *         负值: 错误
 * @note   调用后应由上层执行 NVIC_SystemReset()
 */
int ota_commit(ota_ctx_t *ctx);

/**
 * @brief  中止 OTA, 重置状态
 * @param  ctx: OTA 上下文
 */
void ota_abort(ota_ctx_t *ctx);

/**
 * @brief  获取当前 OTA 状态
 */
ota_state_t ota_get_state(const ota_ctx_t *ctx);

/**
 * @brief  获取接收进度 (百分比 0-100)
 */
uint8_t ota_get_progress(const ota_ctx_t *ctx);

/**
 * @brief  APP 启动后调用: 确认新固件运行正常
 *         将 boot_state 从 TESTING 改为 NORMAL, 防止回滚
 * @return OTA_OK 或负值错误
 */
int ota_confirm_app(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_CORE_H */
