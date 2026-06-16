#include "ota_proto.h"
#include "ota_core.h"
#include "stm32l4xx_hal.h"
#include <string.h>

/* ============================================================================
 * CRC-16/MODBUS
 * ========================================================================= */

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= (uint16_t)byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 1U) {
            crc = (crc >> 1) ^ 0xA001U;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

static uint16_t crc16_calc(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFU;
    for (uint16_t i = 0; i < len; i++) {
        crc = crc16_update(crc, data[i]);
    }
    return crc;
}

/* ============================================================================
 * OTA context (extern, defined in user code)
 * ========================================================================= */
extern ota_ctx_t g_ota_ctx;

/* ============================================================================
 * Protocol Init
 * ========================================================================= */

void proto_init(proto_ctx_t *ctx, void (*send_fn)(const uint8_t *data, uint16_t len))
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->rx.state = RX_STATE_SOF;
    ctx->expected_seq = 0;
    ctx->send = send_fn;
}

/* ============================================================================
 * Byte-by-byte Frame Parser
 * ========================================================================= */

int proto_feed_byte(proto_ctx_t *ctx, uint8_t byte)
{
    proto_rx_ctx_t *rx = &ctx->rx;

    switch (rx->state) {
    case RX_STATE_SOF:
        if (byte == PROTO_SOF) {
            rx->state = RX_STATE_CMD;
            rx->calc_crc = 0xFFFFU;
        }
        break;

    case RX_STATE_CMD:
        rx->frame.cmd = byte;
        rx->calc_crc = crc16_update(rx->calc_crc, byte);
        rx->state = RX_STATE_SEQ;
        break;

    case RX_STATE_SEQ:
        rx->frame.seq = byte;
        rx->calc_crc = crc16_update(rx->calc_crc, byte);
        rx->state = RX_STATE_LEN_L;
        break;

    case RX_STATE_LEN_L:
        rx->frame.payload_len = byte;
        rx->calc_crc = crc16_update(rx->calc_crc, byte);
        rx->state = RX_STATE_LEN_H;
        break;

    case RX_STATE_LEN_H:
        rx->frame.payload_len |= ((uint16_t)byte << 8);
        rx->calc_crc = crc16_update(rx->calc_crc, byte);
        if (rx->frame.payload_len > PROTO_MAX_PAYLOAD) {
            /* Invalid length, reset */
            rx->state = RX_STATE_SOF;
        } else if (rx->frame.payload_len == 0) {
            rx->state = RX_STATE_CRC_L;
        } else {
            rx->payload_idx = 0;
            rx->state = RX_STATE_PAYLOAD;
        }
        break;

    case RX_STATE_PAYLOAD:
        rx->frame.payload[rx->payload_idx++] = byte;
        rx->calc_crc = crc16_update(rx->calc_crc, byte);
        if (rx->payload_idx >= rx->frame.payload_len) {
            rx->state = RX_STATE_CRC_L;
        }
        break;

    case RX_STATE_CRC_L:
        rx->rx_crc = byte;
        rx->state = RX_STATE_CRC_H;
        break;

    case RX_STATE_CRC_H:
        rx->rx_crc |= ((uint16_t)byte << 8);
        rx->state = RX_STATE_SOF;
        /* Frame complete - check CRC */
        if (rx->rx_crc == rx->calc_crc) {
            return 1;  /* Valid frame ready */
        }
        /* CRC mismatch, discard silently */
        break;

    default:
        rx->state = RX_STATE_SOF;
        break;
    }

    return 0;
}

/* ============================================================================
 * Send ACK Frame
 * ========================================================================= */

void proto_send_ack(proto_ctx_t *ctx, uint8_t cmd, uint8_t seq,
                    uint8_t status, const uint8_t *payload, uint16_t payload_len)
{
    if (ctx->send == NULL) return;

    uint16_t total_payload = 1 + payload_len;  /* status byte + extra payload */
    uint16_t frame_len = PROTO_HEADER_SIZE + total_payload + PROTO_CRC_SIZE;
    uint8_t *buf = ctx->tx_buf;
    uint16_t idx = 0;

    buf[idx++] = PROTO_SOF;
    buf[idx++] = CMD_ACK(cmd);
    buf[idx++] = seq;
    buf[idx++] = (uint8_t)(total_payload & 0xFF);
    buf[idx++] = (uint8_t)(total_payload >> 8);
    buf[idx++] = status;

    if (payload != NULL && payload_len > 0) {
        memcpy(&buf[idx], payload, payload_len);
        idx += payload_len;
    }

    /* CRC covers CMD + SEQ + LEN + PAYLOAD (skip SOF) */
    uint16_t crc = crc16_calc(&buf[1], idx - 1);
    buf[idx++] = (uint8_t)(crc & 0xFF);
    buf[idx++] = (uint8_t)(crc >> 8);

    ctx->send(buf, frame_len);
}

/* ============================================================================
 * Frame Processing (dispatch to OTA layer)
 * ========================================================================= */

void proto_process(proto_ctx_t *ctx)
{
    proto_frame_t *f = &ctx->rx.frame;
    int rc;

    switch (f->cmd) {
    case CMD_OTA_START:
        /* Payload should be ota_header_t */
        if (f->payload_len != sizeof(ota_header_t)) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_SIZE, NULL, 0);
            break;
        }
        ota_init(&g_ota_ctx);
        rc = ota_begin(&g_ota_ctx);
        if (rc != OTA_OK) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_STATE, NULL, 0);
            break;
        }
        rc = ota_feed_header(&g_ota_ctx, (const ota_header_t *)f->payload);
        if (rc != OTA_OK) {
            uint8_t err = (rc == OTA_ERR_HEADER) ? ACK_ERR_VERIFY :
                          (rc == OTA_ERR_SIZE)   ? ACK_ERR_SIZE :
                                                   ACK_ERR_FLASH;
            proto_send_ack(ctx, f->cmd, f->seq, err, NULL, 0);
            break;
        }
        ctx->expected_seq = f->seq + 1;
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, NULL, 0);
        break;

    case CMD_OTA_DATA:
        /* Sequence check */
        if (f->seq != ctx->expected_seq) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_SEQ, NULL, 0);
            break;
        }
        rc = ota_feed_data(&g_ota_ctx, f->payload, f->payload_len);
        if (rc != OTA_OK) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_FLASH, NULL, 0);
            break;
        }
        ctx->expected_seq = f->seq + 1;
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, NULL, 0);
        break;

    case CMD_OTA_END:
        rc = ota_finish(&g_ota_ctx);
        if (rc == OTA_OK) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, NULL, 0);
        } else {
            uint8_t err = (rc == OTA_ERR_CRC)  ? ACK_ERR_VERIFY :
                          (rc == OTA_ERR_SIZE)  ? ACK_ERR_SIZE :
                                                  ACK_ERR_FLASH;
            proto_send_ack(ctx, f->cmd, f->seq, err, NULL, 0);
        }
        break;

    case CMD_OTA_ABORT:
        ota_abort(&g_ota_ctx);
        ctx->expected_seq = 0;
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, NULL, 0);
        break;

    case CMD_OTA_REBOOT:
        rc = ota_commit(&g_ota_ctx);
        if (rc != OTA_OK) {
            proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_FLASH, NULL, 0);
            break;
        }
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, NULL, 0);
        /* Delay briefly to let ACK transmit, then reboot */
        for (volatile uint32_t i = 0; i < 100000; i++) {}
        NVIC_SystemReset();
        break;

    case CMD_QUERY_STATUS: {
        uint8_t resp[2];
        resp[0] = (uint8_t)ota_get_state(&g_ota_ctx);
        resp[1] = ota_get_progress(&g_ota_ctx);
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, resp, 2);
        break;
    }

    case CMD_QUERY_VERSION: {
        /* TODO: return actual app version from metadata */
        uint32_t ver = 0x00010000U;  /* v1.0.0 placeholder */
        proto_send_ack(ctx, f->cmd, f->seq, ACK_OK, (uint8_t *)&ver, 4);
        break;
    }

    default:
        proto_send_ack(ctx, f->cmd, f->seq, ACK_ERR_CMD, NULL, 0);
        break;
    }
}
