#include "flash_port.h"
#include "stm32l4xx_hal.h"
#include <string.h>

/* ============================================================================
 * STM32L4 Internal Flash Low-Level Operations
 * ========================================================================= */

static int stm32l4_flash_read(uint32_t addr, uint32_t size, void *dst)
{
    memcpy(dst, (const void *)addr, size);
    return FLASH_OK;
}

static int stm32l4_flash_write(uint32_t addr, uint32_t size, const void *src)
{
    HAL_StatusTypeDef status;
    const uint64_t *src64 = (const uint64_t *)src;
    uint32_t remaining = size;
    uint32_t write_addr = addr;

    status = HAL_FLASH_Unlock();
    if (status != HAL_OK) {
        return FLASH_ERR_LOCK;
    }

    /* Clear all error flags */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    while (remaining >= FLASH_WRITE_SIZE) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, write_addr, *src64);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return FLASH_ERR_WRITE;
        }
        write_addr += FLASH_WRITE_SIZE;
        src64++;
        remaining -= FLASH_WRITE_SIZE;
    }

    HAL_FLASH_Lock();
    return FLASH_OK;
}

static int stm32l4_flash_erase(uint32_t addr, uint32_t size)
{
    HAL_StatusTypeDef status;
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0;
    uint32_t start_page;
    uint32_t num_pages;

    start_page = (addr - FLASH_BASE_ADDR) / FLASH_PAGE_SIZE_BYTES;
    num_pages = size / FLASH_PAGE_SIZE_BYTES;

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = FLASH_BANK_1;
    erase_init.Page = start_page;
    erase_init.NbPages = num_pages;

    status = HAL_FLASH_Unlock();
    if (status != HAL_OK) {
        return FLASH_ERR_LOCK;
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    status = HAL_FLASHEx_Erase(&erase_init, &page_error);

    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        return FLASH_ERR_ERASE;
    }

    return FLASH_OK;
}

/* ============================================================================
 * Flash Device Instance
 * ========================================================================= */

static const struct flash_device g_internal_flash = {
    .base_addr  = FLASH_BASE_ADDR,
    .total_size = FLASH_TOTAL_SIZE,
    .page_size  = FLASH_PAGE_SIZE_BYTES,
    .write_size = FLASH_WRITE_SIZE,
    .read       = stm32l4_flash_read,
    .write      = stm32l4_flash_write,
    .erase      = stm32l4_flash_erase,
};

/* ============================================================================
 * Partition Instances
 * ========================================================================= */

static const struct flash_partition g_part_boot = {
    .flash  = &g_internal_flash,
    .offset = PART_BOOT_OFFSET,
    .size   = PART_BOOT_SIZE,
};

static const struct flash_partition g_part_slot0 = {
    .flash  = &g_internal_flash,
    .offset = PART_SLOT0_OFFSET,
    .size   = PART_SLOT0_SIZE,
};

static const struct flash_partition g_part_slot1 = {
    .flash  = &g_internal_flash,
    .offset = PART_SLOT1_OFFSET,
    .size   = PART_SLOT1_SIZE,
};

static const struct flash_partition g_part_meta = {
    .flash  = &g_internal_flash,
    .offset = PART_META_OFFSET,
    .size   = PART_META_SIZE,
};

static const struct flash_partition g_part_swap_status = {
    .flash  = &g_internal_flash,
    .offset = PART_SWAP_STATUS_OFFSET,
    .size   = PART_SWAP_STATUS_SIZE,
};

/* ============================================================================
 * Parameter Validation Helpers
 * ========================================================================= */

static int validate_partition_access(const struct flash_partition *part,
                                     uint32_t offset, uint32_t size)
{
    if (part == NULL || part->flash == NULL) {
        return FLASH_ERR_PARAM;
    }
    if (size == 0) {
        return FLASH_ERR_PARAM;
    }
    /* Overflow check & range check */
    if ((offset + size) < offset || (offset + size) > part->size) {
        return FLASH_ERR_RANGE;
    }
    return FLASH_OK;
}

/* ============================================================================
 * Public API Implementation
 * ========================================================================= */

int flash_part_read(const struct flash_partition *part,
                    uint32_t offset, uint32_t size, void *dst)
{
    int rc;

    if (dst == NULL) {
        return FLASH_ERR_PARAM;
    }

    rc = validate_partition_access(part, offset, size);
    if (rc != FLASH_OK) {
        return rc;
    }

    uint32_t abs_addr = part->flash->base_addr + part->offset + offset;
    return part->flash->read(abs_addr, size, dst);
}

int flash_part_write(const struct flash_partition *part,
                     uint32_t offset, uint32_t size, const void *src)
{
    int rc;

    if (src == NULL) {
        return FLASH_ERR_PARAM;
    }

    rc = validate_partition_access(part, offset, size);
    if (rc != FLASH_OK) {
        return rc;
    }

    /* Write alignment check */
    if ((offset % part->flash->write_size) != 0 ||
        (size % part->flash->write_size) != 0) {
        return FLASH_ERR_ALIGN;
    }

    uint32_t abs_addr = part->flash->base_addr + part->offset + offset;
    return part->flash->write(abs_addr, size, src);
}

int flash_part_erase(const struct flash_partition *part,
                     uint32_t offset, uint32_t size)
{
    int rc;

    rc = validate_partition_access(part, offset, size);
    if (rc != FLASH_OK) {
        return rc;
    }

    /* Erase alignment check (must be page-aligned) */
    if ((offset % part->flash->page_size) != 0 ||
        (size % part->flash->page_size) != 0) {
        return FLASH_ERR_ALIGN;
    }

    uint32_t abs_addr = part->flash->base_addr + part->offset + offset;
    return part->flash->erase(abs_addr, size);
}

/* ============================================================================
 * Partition Getters
 * ========================================================================= */

const struct flash_partition *flash_get_partition_boot(void)
{
    return &g_part_boot;
}

const struct flash_partition *flash_get_partition_slot0(void)
{
    return &g_part_slot0;
}

const struct flash_partition *flash_get_partition_slot1(void)
{
    return &g_part_slot1;
}

const struct flash_partition *flash_get_partition_meta(void)
{
    return &g_part_meta;
}

const struct flash_partition *flash_get_partition_swap_status(void)
{
    return &g_part_swap_status;
}

const struct flash_device *flash_get_device(void)
{
    return &g_internal_flash;
}
