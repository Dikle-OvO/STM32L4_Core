#include "bl_core.h"
#include "bl_meta.h"
#include "flash_port.h"
#include "util_uart.h"
#include "stm32l4xx_hal.h"
#include <string.h>

/* ============================================================================
 * Image Validation
 * ========================================================================= */

int bl_validate_image(uint32_t addr)
{
    uint32_t sp = *(volatile uint32_t *)addr;
    uint32_t reset = *(volatile uint32_t *)(addr + 4);

    /* SP must be within RAM range */
    if (sp < 0x20000000U || sp > (0x20000000U + 48U * 1024U)) {
        return -1;
    }

    /* Reset handler must be within Flash range and odd (Thumb) */
    if (reset < 0x08000000U || reset > (0x08000000U + FLASH_TOTAL_SIZE)) {
        return -1;
    }
    if ((reset & 1U) == 0) {
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Slot Swap: Copy Slot1 -> Slot0
 * ========================================================================= */

int bl_swap_slot1_to_slot0(uint32_t size)
{
    const struct flash_partition *slot0 = flash_get_partition_slot0();
    const struct flash_partition *slot1 = flash_get_partition_slot1();
    int rc;
    uint8_t buf[256];
    uint32_t offset = 0;

    if (size == 0 || size > PART_SLOT0_SIZE) {
        return -1;
    }

    /* Erase Slot 0 (only pages needed) */
    uint32_t erase_size = ((size + FLASH_PAGE_SIZE_BYTES - 1) / FLASH_PAGE_SIZE_BYTES) * FLASH_PAGE_SIZE_BYTES;
    util_uart_printf("[BL] Erasing slot0 %u bytes...\r\n", (unsigned)erase_size);
    rc = flash_part_erase(slot0, 0, erase_size);
    if (rc != FLASH_OK) {
        util_uart_printf("[BL] Erase FAILED rc=%d\r\n", rc);
        return rc;
    }

    /* Copy from Slot 1 to Slot 0 in chunks */
    uint32_t last_pct = 0;
    while (offset < size) {
        uint32_t chunk = sizeof(buf);
        if ((size - offset) < chunk) {
            chunk = size - offset;
        }

        /* Pad to write alignment */
        uint32_t write_len = ((chunk + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE) * FLASH_WRITE_SIZE;
        memset(buf, FLASH_ERASE_VALUE, sizeof(buf));

        rc = flash_part_read(slot1, offset, chunk, buf);
        if (rc != FLASH_OK) {
            util_uart_printf("[BL] Read FAILED at 0x%X rc=%d\r\n", (unsigned)offset, rc);
            return rc;
        }

        rc = flash_part_write(slot0, offset, write_len, buf);
        if (rc != FLASH_OK) {
            util_uart_printf("[BL] Write FAILED at 0x%X rc=%d\r\n", (unsigned)offset, rc);
            return rc;
        }

        offset += chunk;

        /* 每 10% 打印一次进度 */
        uint32_t pct = (offset * 100) / size;
        if (pct / 10 > last_pct / 10) {
            last_pct = pct;
            util_uart_printf("[BL] Swap progress: %u%%\r\n", (unsigned)pct);
        }
    }

    util_uart_printf("[BL] Swap complete\r\n");
    return 0;
}

/* ============================================================================
 * Jump to Application
 * ========================================================================= */

void bl_jump_to_app(uint32_t addr)
{
    typedef void (*app_entry_t)(void);

    uint32_t app_sp = *(volatile uint32_t *)addr;
    uint32_t app_reset = *(volatile uint32_t *)(addr + 4);

    /* Disable all interrupts */
    __disable_irq();

    /* Disable SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    /* Clear all pending interrupts */
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* Relocate vector table to APP */
    SCB->VTOR = addr;

    /* Set MSP to APP's stack pointer */
    __set_MSP(app_sp);

    /* Enable interrupts (APP will configure them) */
    __enable_irq();

    /* Jump to APP Reset_Handler */
    app_entry_t app_entry = (app_entry_t)app_reset;
    app_entry();

    /* Should never reach here */
    while (1) {}
}

/* ============================================================================
 * Bootloader Main Logic
 * ========================================================================= */

void bl_run(void)
{
    boot_meta_t meta;
    int rc;

    /* Load metadata */
    rc = meta_load(&meta);
    if (rc != 0) {
        /* Metadata corrupted - initialize defaults and try boot */
        util_uart_printf("[BL] Meta corrupted, init defaults\r\n");
        meta_init_default(&meta);
        meta_save(&meta);
    }

    util_uart_printf("[BL] Boot state: %u\r\n", (unsigned)meta.boot_state);

    switch (meta.boot_state) {
    case BOOT_STATE_SWAP:
        /* OTA firmware ready in Slot 1, swap to Slot 0 */
        util_uart_printf("[BL] SWAP: size=%u\r\n", (unsigned)meta.ota_size);
        if (meta.ota_size > 0 && meta.ota_size <= PART_SLOT0_SIZE) {
            rc = bl_swap_slot1_to_slot0(meta.ota_size);
            if (rc == 0) {
                /* Swap success: enter TESTING state */
                util_uart_printf("[BL] Swap OK, enter TESTING\r\n");
                meta.boot_state = BOOT_STATE_TESTING;
                meta.boot_count = 0;
                meta.app_size = meta.ota_size;
                meta.app_crc32 = meta.ota_crc32;
                meta.app_version = meta.ota_version;
                meta_save(&meta);
            } else {
                /* Swap failed: stay NORMAL, boot old APP */
                util_uart_printf("[BL] Swap FAILED rc=%d\r\n", rc);
                meta.boot_state = BOOT_STATE_NORMAL;
                meta_save(&meta);
            }
        } else {
            meta.boot_state = BOOT_STATE_NORMAL;
            meta_save(&meta);
        }
        break;

    case BOOT_STATE_TESTING:
        /* Increment boot count; if exceeded, rollback */
        meta.boot_count++;
        util_uart_printf("[BL] TESTING: boot_count=%u/%u\r\n",
                         (unsigned)meta.boot_count, (unsigned)meta.max_boot_count);
        if (meta.boot_count > meta.max_boot_count) {
            /* Too many unsuccessful boots - mark rollback */
            meta.boot_state = BOOT_STATE_ROLLBACK;
            meta_save(&meta);
            /* TODO: actual rollback needs backup of old firmware */
            /* For now, just try to boot whatever is in Slot 0 */
        } else {
            meta_save(&meta);
        }
        break;

    case BOOT_STATE_ROLLBACK:
        /* Rollback state - just try to boot Slot 0 */
        meta.boot_state = BOOT_STATE_NORMAL;
        meta_save(&meta);
        break;

    case BOOT_STATE_NORMAL:
    default:
        /* Normal boot */
        break;
    }

    /* Validate and jump to APP */
    uint32_t app_addr = FLASH_BASE_ADDR + PART_SLOT0_OFFSET;
    if (bl_validate_image(app_addr) == 0) {
        util_uart_printf("[BL] APP valid, jumping to 0x%08X\r\n", (unsigned)app_addr);
        bl_jump_to_app(app_addr);
    }

    util_uart_printf("[BL] APP invalid at 0x%08X\r\n", (unsigned)app_addr);
    /* APP invalid: stay in bootloader (wait for recovery via UART, etc.) */
    while (1) {
        HAL_Delay(1000);
    }
}
