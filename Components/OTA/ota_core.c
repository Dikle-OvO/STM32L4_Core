#include "ota_core.h"
#include "flash_port.h"
#include <string.h>

/* bl_meta.h is shared between BL and APP for metadata structure */
#include "bl_meta.h"

/* ============================================================================
 * CRC32 (same algorithm as bl_meta.c)
 * ========================================================================= */

static uint32_t crc32_calc(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t crc32_update(uint32_t crc_state, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    /* crc_state is stored as final XOR'd value, undo it */
    uint32_t crc = crc_state ^ 0xFFFFFFFFU;

    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ============================================================================
 * Internal: flush write buffer to Flash
 * ========================================================================= */

static int flush_buffer(ota_ctx_t *ctx)
{
    if (ctx->buf_used == 0) {
        return OTA_OK;
    }

    const struct flash_partition *slot1 = flash_get_partition_slot1();

    /* Pad remaining buffer to FLASH_WRITE_SIZE alignment */
    uint32_t write_len = ((uint32_t)ctx->buf_used + FLASH_WRITE_SIZE - 1)
                         & ~(FLASH_WRITE_SIZE - 1);

    /* Fill padding with 0xFF */
    if (write_len > ctx->buf_used) {
        memset(&ctx->write_buf[ctx->buf_used], FLASH_ERASE_VALUE,
               write_len - ctx->buf_used);
    }

    int rc = flash_part_write(slot1, ctx->write_offset, write_len, ctx->write_buf);
    if (rc != FLASH_OK) {
        return OTA_ERR_FLASH;
    }

    ctx->write_offset += write_len;
    ctx->buf_used = 0;

    return OTA_OK;
}

/* ============================================================================
 * Public API
 * ========================================================================= */

void ota_init(ota_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = OTA_STATE_IDLE;
    ctx->running_crc = 0xFFFFFFFFU ^ 0xFFFFFFFFU; /* Initial CRC = 0x00000000 after XOR trick */
}

int ota_begin(ota_ctx_t *ctx)
{
    if (ctx->state != OTA_STATE_IDLE && ctx->state != OTA_STATE_ERROR) {
        return OTA_ERR_STATE;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->state = OTA_STATE_HEADER;
    /* running_crc starts as CRC of empty data = 0x00000000 */
    ctx->running_crc = 0x00000000U;

    return OTA_OK;
}

int ota_feed_header(ota_ctx_t *ctx, const ota_header_t *header)
{
    if (ctx->state != OTA_STATE_HEADER) {
        return OTA_ERR_STATE;
    }

    /* Validate header magic */
    if (header->magic != OTA_HEADER_MAGIC) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_HEADER;
    }

    /* Validate header CRC (covers everything except header_crc32) */
    uint32_t hdr_crc = crc32_calc(header, offsetof(ota_header_t, header_crc32));
    if (hdr_crc != header->header_crc32) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_HEADER;
    }

    /* Validate firmware size fits in Slot 1 */
    if (header->fw_size == 0 || header->fw_size > PART_SLOT1_SIZE) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_SIZE;
    }

    /* Save header */
    memcpy(&ctx->header, header, sizeof(ota_header_t));

    /* Erase Slot 1 (only pages needed) */
    const struct flash_partition *slot1 = flash_get_partition_slot1();
    uint32_t erase_size = ((header->fw_size + FLASH_PAGE_SIZE_BYTES - 1)
                           / FLASH_PAGE_SIZE_BYTES) * FLASH_PAGE_SIZE_BYTES;

    int rc = flash_part_erase(slot1, 0, erase_size);
    if (rc != FLASH_OK) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_FLASH;
    }

    /* Ready to receive data */
    ctx->state = OTA_STATE_RECEIVING;
    ctx->received_size = 0;
    ctx->write_offset = 0;
    ctx->buf_used = 0;
    ctx->running_crc = 0x00000000U;

    return OTA_OK;
}

int ota_feed_data(ota_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    if (ctx->state != OTA_STATE_RECEIVING) {
        return OTA_ERR_STATE;
    }

    if (data == NULL || len == 0) {
        return OTA_ERR_STATE;
    }

    /* Check for overflow */
    if ((ctx->received_size + len) > ctx->header.fw_size) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_SIZE;
    }

    /* Update running CRC */
    ctx->running_crc = crc32_update(ctx->running_crc, data, len);

    /* Buffer and flush */
    uint32_t remaining = len;
    const uint8_t *src = data;

    while (remaining > 0) {
        uint32_t space = sizeof(ctx->write_buf) - ctx->buf_used;
        uint32_t chunk = (remaining < space) ? remaining : space;

        memcpy(&ctx->write_buf[ctx->buf_used], src, chunk);
        ctx->buf_used += (uint16_t)chunk;
        src += chunk;
        remaining -= chunk;

        /* Buffer full: flush to Flash */
        if (ctx->buf_used >= sizeof(ctx->write_buf)) {
            int rc = flush_buffer(ctx);
            if (rc != OTA_OK) {
                ctx->state = OTA_STATE_ERROR;
                return rc;
            }
        }
    }

    ctx->received_size += len;
    return OTA_OK;
}

int ota_finish(ota_ctx_t *ctx)
{
    if (ctx->state != OTA_STATE_RECEIVING) {
        return OTA_ERR_STATE;
    }

    ctx->state = OTA_STATE_VERIFYING;

    /* Check size match */
    if (ctx->received_size != ctx->header.fw_size) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_SIZE;
    }

    /* Flush remaining buffer */
    int rc = flush_buffer(ctx);
    if (rc != OTA_OK) {
        ctx->state = OTA_STATE_ERROR;
        return rc;
    }

    /* Verify CRC */
    if (ctx->running_crc != ctx->header.fw_crc32) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_CRC;
    }

    ctx->state = OTA_STATE_DONE;
    return OTA_OK;
}

int ota_commit(ota_ctx_t *ctx)
{
    if (ctx->state != OTA_STATE_DONE) {
        return OTA_ERR_STATE;
    }

    /* Load current metadata */
    boot_meta_t meta;
    int rc = meta_load(&meta);
    if (rc != 0) {
        meta_init_default(&meta);
    }

    /* Update metadata: mark for swap on next boot */
    meta.ota_size = ctx->header.fw_size;
    meta.ota_crc32 = ctx->header.fw_crc32;
    meta.ota_version = ctx->header.fw_version;
    meta.boot_state = BOOT_STATE_SWAP;
    meta.boot_count = 0;

    rc = meta_save(&meta);
    if (rc != 0) {
        ctx->state = OTA_STATE_ERROR;
        return OTA_ERR_FLASH;
    }

    return OTA_OK;
}

void ota_abort(ota_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = OTA_STATE_IDLE;
}

ota_state_t ota_get_state(const ota_ctx_t *ctx)
{
    return ctx->state;
}

uint8_t ota_get_progress(const ota_ctx_t *ctx)
{
    if (ctx->state != OTA_STATE_RECEIVING || ctx->header.fw_size == 0) {
        return 0;
    }
    return (uint8_t)((ctx->received_size * 100U) / ctx->header.fw_size);
}

int ota_confirm_app(void)
{
    boot_meta_t meta;
    int rc = meta_load(&meta);
    if (rc != 0) {
        return OTA_ERR_FLASH;
    }

    if (meta.boot_state == BOOT_STATE_TESTING) {
        meta.boot_state = BOOT_STATE_NORMAL;
        meta.boot_count = 0;
        rc = meta_save(&meta);
        if (rc != 0) {
            return OTA_ERR_FLASH;
        }
    }

    return OTA_OK;
}
