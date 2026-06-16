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
