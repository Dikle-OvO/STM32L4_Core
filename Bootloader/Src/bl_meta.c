#include "bl_meta.h"
#include "flash_port.h"
#include <string.h>

/* Primary metadata at offset 0, backup at offset PAGE_SIZE */
#define META_PRIMARY_OFFSET     0U
#define META_BACKUP_OFFSET      FLASH_PAGE_SIZE_BYTES

/* ============================================================================
 * CRC32 (polynomial 0x04C11DB7, standard Ethernet CRC)
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

/* ============================================================================
 * Internal Helpers
 * ========================================================================= */

static int meta_validate(const boot_meta_t *meta)
{
    if (meta->magic != META_MAGIC) {
        return -1;
    }
    /* CRC covers everything except the meta_crc32 field itself */
    uint32_t crc_offset = offsetof(boot_meta_t, meta_crc32);
    uint32_t crc = crc32_calc(meta, crc_offset);
    if (crc != meta->meta_crc32) {
        return -1;
    }
    return 0;
}

static void meta_compute_crc(boot_meta_t *meta)
{
    uint32_t crc_offset = offsetof(boot_meta_t, meta_crc32);
    meta->meta_crc32 = crc32_calc(meta, crc_offset);
}

/* ============================================================================
 * Public API
 * ========================================================================= */

int meta_load(boot_meta_t *meta)
{
    const struct flash_partition *part = flash_get_partition_meta();
    boot_meta_t primary, backup;
    int primary_ok, backup_ok;

    /* Read primary */
    flash_part_read(part, META_PRIMARY_OFFSET, sizeof(primary), &primary);
    primary_ok = meta_validate(&primary);

    /* Read backup */
    flash_part_read(part, META_BACKUP_OFFSET, sizeof(backup), &backup);
    backup_ok = meta_validate(&backup);

    if (primary_ok == 0) {
        *meta = primary;
        /* If backup is corrupted, repair it */
        if (backup_ok != 0) {
            flash_part_erase(part, META_BACKUP_OFFSET, FLASH_PAGE_SIZE_BYTES);
            flash_part_write(part, META_BACKUP_OFFSET, sizeof(boot_meta_t), &primary);
        }
        return 0;
    }

    if (backup_ok == 0) {
        *meta = backup;
        /* Repair primary */
        flash_part_erase(part, META_PRIMARY_OFFSET, FLASH_PAGE_SIZE_BYTES);
        flash_part_write(part, META_PRIMARY_OFFSET, sizeof(boot_meta_t), &backup);
        return 0;
    }

    /* Both corrupted */
    return -1;
}

int meta_save(boot_meta_t *meta)
{
    const struct flash_partition *part = flash_get_partition_meta();
    int rc;

    meta->magic = META_MAGIC;
    meta->version = META_VERSION;
    meta_compute_crc(meta);

    /* Erase and write primary */
    rc = flash_part_erase(part, META_PRIMARY_OFFSET, FLASH_PAGE_SIZE_BYTES);
    if (rc != FLASH_OK) return rc;
    rc = flash_part_write(part, META_PRIMARY_OFFSET, sizeof(boot_meta_t), meta);
    if (rc != FLASH_OK) return rc;

    /* Erase and write backup */
    rc = flash_part_erase(part, META_BACKUP_OFFSET, FLASH_PAGE_SIZE_BYTES);
    if (rc != FLASH_OK) return rc;
    rc = flash_part_write(part, META_BACKUP_OFFSET, sizeof(boot_meta_t), meta);
    if (rc != FLASH_OK) return rc;

    return 0;
}

void meta_init_default(boot_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->magic = META_MAGIC;
    meta->version = META_VERSION;
    meta->boot_state = BOOT_STATE_NORMAL;
    meta->boot_count = 0;
    meta->max_boot_count = 3;
    meta_compute_crc(meta);
}

/* ============================================================================
 * Swap Status API
 * ============================================================================
 * Swap Status 区 (4KB) 布局:
 *   offset 0:                swap_header_t  (8 bytes)
 *   offset 8 + i*8*3 + s*8:  swap_marker_t  (8 bytes)
 *     i = page_index, s = step (0,1,2)
 *   每页 3 个 marker 槽位, 共占 24 bytes/页
 *   4KB 可支持: (4096 - 8) / 24 = 170 页 > 26 页 (够用)
 * ========================================================================= */

/* marker slot offset for page i, step s */
static inline uint32_t swap_marker_offset(uint16_t page, uint8_t step)
{
    return sizeof(swap_header_t)
           + (uint32_t)page * 3U * sizeof(swap_marker_t)
           + (uint32_t)step * sizeof(swap_marker_t);
}

int swap_status_init(uint16_t total_pages)
{
    const struct flash_partition *part = flash_get_partition_swap_status();
    int rc;

    rc = flash_part_erase(part, 0, PART_SWAP_STATUS_SIZE);
    if (rc != FLASH_OK) return rc;

    swap_header_t hdr = {
        .magic = SWAP_MAGIC,
        .total_pages = total_pages,
        .reserved = 0,
    };
    return flash_part_write(part, 0, sizeof(hdr), &hdr);
}

int swap_status_read_progress(uint16_t *total_pages, uint16_t *done_pages,
                              uint8_t *last_step)
{
    const struct flash_partition *part = flash_get_partition_swap_status();
    swap_header_t hdr;

    flash_part_read(part, 0, sizeof(hdr), &hdr);
    if (hdr.magic != SWAP_MAGIC || hdr.total_pages == 0) {
        return -1;
    }
    *total_pages = hdr.total_pages;

    uint16_t fully_done = 0;
    *last_step = 0xFFU;  /* 未开始 */

    for (uint16_t i = 0; i < hdr.total_pages; i++) {
        /* 检查 step 2 (PAGE_DONE) */
        swap_marker_t m;
        uint32_t off = swap_marker_offset(i, 2);
        flash_part_read(part, off, sizeof(m), &m);
        if (m.page_index == i && m.step == SWAP_STEP_PAGE_DONE) {
            fully_done++;
            continue;
        }
        /* 这页未完全完成, 检查已完成的最后 step */
        off = swap_marker_offset(i, 1);
        flash_part_read(part, off, sizeof(m), &m);
        if (m.page_index == i && m.step == SWAP_STEP_SLOT1_DONE) {
            *last_step = SWAP_STEP_SLOT1_DONE;
        } else {
            off = swap_marker_offset(i, 0);
            flash_part_read(part, off, sizeof(m), &m);
            if (m.page_index == i && m.step == SWAP_STEP_SLOT0_DONE) {
                *last_step = SWAP_STEP_SLOT0_DONE;
            }
            /* else: 0xFF = 未开始 */
        }
        break;  /* 找到第一个未完成页 */
    }
    *done_pages = fully_done;
    return 0;
}

int swap_status_mark_step(uint16_t page_index, uint8_t step)
{
    const struct flash_partition *part = flash_get_partition_swap_status();
    uint8_t slot;
    if (step == SWAP_STEP_SLOT0_DONE)      slot = 0;
    else if (step == SWAP_STEP_SLOT1_DONE) slot = 1;
    else if (step == SWAP_STEP_PAGE_DONE)  slot = 2;
    else return -1;

    swap_marker_t marker = {
        .page_index = page_index,
        .step = step,
        .reserved = {0},
    };
    uint32_t offset = swap_marker_offset(page_index, slot);
    return flash_part_write(part, offset, sizeof(marker), &marker);
}
