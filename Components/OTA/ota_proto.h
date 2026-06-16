#ifndef __OTA_PROTO_H
#define __OTA_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Layer 4: OTA 传输协议 (UART)
 * ============================================================================
 * 帧格式:
 *   [SOF][CMD][SEQ][LEN_L][LEN_H][PAYLOAD...][CRC16_L][CRC16_H]
 *
 *   SOF:     0xAA (帧起始)
 *   CMD:     命令码 (1 byte)
 *   SEQ:     序列号 (1 byte, 0-255 循环)
 *   LEN:     payload 长度 (2 bytes, little-endian, 0-1024)
 *   PAYLOAD: 数据 (0-1024 bytes)
 *   CRC16:   CRC-16/MODBUS (覆盖 CMD+SEQ+LEN+PAYLOAD)
 *
 * 流控: 主从模式, Host 发命令, MCU 回应答
 * ========================================================================= */

#define PROTO_SOF               0xAAU
#define PROTO_MAX_PAYLOAD       1024U
#define PROTO_HEADER_SIZE       5U      /* SOF + CMD + SEQ + LEN(2) */
#define PROTO_CRC_SIZE          2U
#define PROTO_MAX_FRAME_SIZE    (PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + PROTO_CRC_SIZE)

/* ============================================================================
 * 命令码定义
 * ========================================================================= */

/* Host → MCU (请求) */
#define CMD_OTA_START           0x01U   /* 开始 OTA, payload = ota_header_t */
#define CMD_OTA_DATA            0x02U   /* 数据包, payload = 固件数据块 */
#define CMD_OTA_END             0x03U   /* 传输结束, payload = 空 */
#define CMD_OTA_ABORT           0x04U   /* 中止 OTA */
#define CMD_OTA_REBOOT          0x05U   /* 确认升级并重启 */
#define CMD_QUERY_STATUS        0x10U   /* 查询 OTA 状态 */
#define CMD_QUERY_VERSION       0x11U   /* 查询当前固件版本 */

/* MCU → Host (应答, CMD | 0x80) */
#define CMD_ACK_MASK            0x80U
#define CMD_ACK(cmd)            ((cmd) | CMD_ACK_MASK)

/* ============================================================================
 * 应答状态码
 * ========================================================================= */
#define ACK_OK                  0x00U
#define ACK_ERR_CRC             0x01U   /* 帧 CRC 错误 */
#define ACK_ERR_SEQ             0x02U   /* 序列号错误 */
#define ACK_ERR_CMD             0x03U   /* 未知命令 */
#define ACK_ERR_STATE           0x04U   /* OTA 状态不对 */
#define ACK_ERR_FLASH           0x05U   /* Flash 操作失败 */
#define ACK_ERR_VERIFY          0x06U   /* 校验失败 */
#define ACK_ERR_SIZE            0x07U   /* 大小错误 */

/* ============================================================================
 * 帧结构 (解析后)
 * ========================================================================= */
typedef struct {
    uint8_t cmd;
    uint8_t seq;
    uint16_t payload_len;
    uint8_t payload[PROTO_MAX_PAYLOAD];
} proto_frame_t;

/* ============================================================================
 * 接收状态机
 * ========================================================================= */
typedef enum {
    RX_STATE_SOF = 0,
    RX_STATE_CMD,
    RX_STATE_SEQ,
    RX_STATE_LEN_L,
    RX_STATE_LEN_H,
    RX_STATE_PAYLOAD,
    RX_STATE_CRC_L,
    RX_STATE_CRC_H,
} rx_state_t;

typedef struct {
    rx_state_t state;
    proto_frame_t frame;
    uint16_t payload_idx;
    uint16_t rx_crc;            /* 帧中携带的 CRC */
    uint16_t calc_crc;          /* 计算得到的 CRC */
} proto_rx_ctx_t;

/* ============================================================================
 * 协议层上下文
 * ========================================================================= */
typedef struct {
    proto_rx_ctx_t rx;
    uint8_t expected_seq;       /* 下一个期望的 SEQ */
    uint8_t tx_buf[PROTO_MAX_FRAME_SIZE];

    /* UART 发送回调 (由用户注册) */
    void (*send)(const uint8_t *data, uint16_t len);
} proto_ctx_t;

/* ============================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief  初始化协议层
 * @param  ctx: 协议上下文
 * @param  send_fn: UART 发送函数 (阻塞发送即可)
 */
void proto_init(proto_ctx_t *ctx, void (*send_fn)(const uint8_t *data, uint16_t len));

/**
 * @brief  逐字节喂入接收数据 (在 UART 中断或 DMA 回调中调用)
 * @param  ctx: 协议上下文
 * @param  byte: 接收到的字节
 * @return 1: 完整帧已就绪, 调用 proto_process() 处理
 *         0: 继续接收
 */
int proto_feed_byte(proto_ctx_t *ctx, uint8_t byte);

/**
 * @brief  处理已接收的完整帧 (调度到 OTA 业务层)
 * @param  ctx: 协议上下文
 * @note   此函数内部会自动发送应答帧
 */
void proto_process(proto_ctx_t *ctx);

/**
 * @brief  发送应答帧
 * @param  ctx: 协议上下文
 * @param  cmd: 原始命令码 (会自动加 ACK_MASK)
 * @param  seq: 序列号
 * @param  status: 应答状态码
 * @param  payload: 额外应答数据 (可为 NULL)
 * @param  payload_len: 应答数据长度
 */
void proto_send_ack(proto_ctx_t *ctx, uint8_t cmd, uint8_t seq,
                    uint8_t status, const uint8_t *payload, uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_PROTO_H */
