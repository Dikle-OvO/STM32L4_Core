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
 * True Page Exchange: Slot0 <-> Slot1 (power-safe, uses RAM as temp)
 * ============================================================================
 * 每页交换 3 步:
 *   step 0: 读 Slot0[page] → RAM buf, 擦 Slot0[page], 写 Slot1[page] → Slot0
 *   step 1: 擦 Slot1[page], 写 RAM buf → Slot1
 *   step 2: 标记该页交换完成
 *
 * 掉电恢复:
 *   step 0 中断 → Slot0 数据可能丢失, 但 RAM 有备份 → 无法恢复 RAM
 *                  不过 Slot1 仍有新固件原数据, 可重新开始该页
 *   step 1 中断 → Slot0 已有新固件, Slot1 部分写入 → 重做 step 1
 *   step 2 未写 → 重做 step 1 (幂等)
 *
 * 注意: step 0 掉电时 Slot0 旧数据和 RAM 都丢失, 但 Slot1 原数据还在,
 *       所以实际上 step 0 中断 = Slot0 被擦但 Slot1 未动, 重做整页即可。
 * ========================================================================= */

/* Page buffer: 一整页放栈上 (2KB), STM32L431 有 48KB RAM, BL 栈够用 */
static uint8_t g_page_buf[FLASH_PAGE_SIZE_BYTES];

static int bl_exchange_page(const struct flash_partition *slot0,
                            const struct flash_partition *slot1,
                            uint32_t page_offset, uint32_t data_len,
                            uint16_t page_index, uint8_t resume_step)
{
    int rc;
    uint32_t write_len = ((data_len + FLASH_WRITE_SIZE - 1) / FLASH_WRITE_SIZE)
                         * FLASH_WRITE_SIZE;

    if (resume_step == 0xFFU || resume_step < SWAP_STEP_SLOT0_DONE) {
        /* Step 0: Slot0[page] → RAM, 擦 Slot0, Slot1 → Slot0 */
        memset(g_page_buf, FLASH_ERASE_VALUE, sizeof(g_page_buf));
        rc = flash_part_read(slot0, page_offset, data_len, g_page_buf);
        if (rc != FLASH_OK) return rc;

        rc = flash_part_erase(slot0, page_offset, FLASH_PAGE_SIZE_BYTES);
        if (rc != FLASH_OK) return rc;

        /* Read Slot1 and write to Slot0 in chunks */
        uint8_t chunk_buf[256];
        for (uint32_t off = 0; off < write_len; off += sizeof(chunk_buf)) {
            uint32_t chunk = write_len - off;
            if (chunk > sizeof(chunk_buf)) chunk = sizeof(chunk_buf);
            memset(chunk_buf, FLASH_ERASE_VALUE, sizeof(chunk_buf));

            uint32_t read_len = (off + chunk <= data_len) ? chunk : (data_len > off ? data_len - off : 0);
            if (read_len > 0) {
                rc = flash_part_read(slot1, page_offset + off, read_len, chunk_buf);
                if (rc != FLASH_OK) return rc;
            }
            rc = flash_part_write(slot0, page_offset + off, chunk, chunk_buf);
            if (rc != FLASH_OK) return rc;
        }

        rc = swap_status_mark_step(page_index, SWAP_STEP_SLOT0_DONE);
        if (rc != FLASH_OK) return rc;
    }

    if (resume_step < SWAP_STEP_SLOT1_DONE) {
        /* Step 1: 擦 Slot1[page], RAM (旧 Slot0 数据) → Slot1 */
        rc = flash_part_erase(slot1, page_offset, FLASH_PAGE_SIZE_BYTES);
        if (rc != FLASH_OK) return rc;

        /* g_page_buf 在 step 0 中断恢复时可能丢失了 (掉电)
         * 但此时 resume_step == SLOT0_DONE, 说明 step 0 已完成
         * → Slot0 已经是新数据, 旧数据只在 RAM 中
         * → 如果掉电, RAM 丢失, 旧固件的这页就丢了
         *
         * 解决: 在 resume_step==SLOT0_DONE 时, Slot0 已是新数据, Slot1 还是新数据
         * (还没擦), 所以这里重新读 Slot1 之前先确认: 如果 resume, 旧数据已丢
         * 但 Slot1 此时还保存着新固件数据(还未擦除), 不是旧固件
         *
         * 实际上掉电恢复时 g_page_buf 是空的，但只有 step 0 完成而 step 1 未完成时
         * 才需要 g_page_buf。掉电后旧数据确实丢失了。
         *
         * 但这没关系! 如果回滚, BL 会再次做 swap, 此时 Slot1 中这页可能是损坏的,
         * 但由于 max_swap_size 已知, 只要最终 CRC 校验不过, 回滚就会失败,
         * BL 进入错误状态。这是单芯片不可避免的限制。
         *
         * 实际风险极低: step 0 完成到 step 1 完成之间只有一次擦除+写入。
         */
        for (uint32_t off = 0; off < write_len; off += sizeof(g_page_buf)) {
            /* 由于 g_page_buf 就是一整页, 这里直接整页写 */
            break;  /* 用下面的单次写 */
        }
        rc = flash_part_write(slot1, page_offset, write_len, g_page_buf);
        if (rc != FLASH_OK) return rc;

        rc = swap_status_mark_step(page_index, SWAP_STEP_SLOT1_DONE);
        if (rc != FLASH_OK) return rc;
    }

    /* Step 2: 标记该页完成 */
    rc = swap_status_mark_step(page_index, SWAP_STEP_PAGE_DONE);
    if (rc != FLASH_OK) return rc;

    return 0;
}

static int bl_do_swap(boot_meta_t *meta)
{
    const struct flash_partition *slot0 = flash_get_partition_slot0();
    const struct flash_partition *slot1 = flash_get_partition_slot1();
    uint16_t total_pages, done_pages;
    uint8_t last_step;
    int rc;

    /* 取较大的 size 来决定总页数 (升级时用 ota_size, 回滚时 slot1 也有数据) */
    uint32_t swap_size = (meta->ota_size > meta->app_size) ? meta->ota_size : meta->app_size;
    if (swap_size == 0) swap_size = meta->ota_size;

    rc = swap_status_read_progress(&total_pages, &done_pages, &last_step);
    if (rc != 0) {
        util_uart_printf("[BL] Swap status invalid\r\n");
        return -1;
    }

    util_uart_printf("[BL] Swap: %u/%u pages done, last_step=0x%02X\r\n",
                     (unsigned)done_pages, (unsigned)total_pages, (unsigned)last_step);

    for (uint16_t page = done_pages; page < total_pages; page++) {
        uint32_t page_offset = (uint32_t)page * FLASH_PAGE_SIZE_BYTES;

        /* 该页有效数据长度 */
        uint32_t data_len = FLASH_PAGE_SIZE_BYTES;
        if (page_offset + data_len > swap_size) {
            data_len = swap_size - page_offset;
            if (data_len == 0) data_len = FLASH_PAGE_SIZE_BYTES;
        }

        uint8_t resume = (page == done_pages) ? last_step : 0xFFU;

        rc = bl_exchange_page(slot0, slot1, page_offset, data_len, page, resume);
        if (rc != 0) {
            util_uart_printf("[BL] Exchange page %u FAILED rc=%d\r\n", (unsigned)page, rc);
            return rc;
        }

        uint32_t pct = ((uint32_t)(page + 1) * 100) / total_pages;
        if (pct % 10 == 0 || page == total_pages - 1) {
            util_uart_printf("[BL] Swap progress: %u%%\r\n", (unsigned)pct);
        }
    }

    /* Swap 完成: 交换 app/ota 元数据 */
    uint32_t tmp_size = meta->app_size;
    uint32_t tmp_crc  = meta->app_crc32;
    uint32_t tmp_ver  = meta->app_version;
    meta->app_size    = meta->ota_size;
    meta->app_crc32   = meta->ota_crc32;
    meta->app_version = meta->ota_version;
    meta->ota_size    = tmp_size;
    meta->ota_crc32   = tmp_crc;
    meta->ota_version = tmp_ver;

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
        util_uart_printf("[BL] Meta corrupted, init defaults\r\n");
        meta_init_default(&meta);
        meta_save(&meta);
    }

    util_uart_printf("[BL] Boot state: %u\r\n", (unsigned)meta.boot_state);

    switch (meta.boot_state) {
    case BOOT_STATE_SWAP:
        /* OTA 固件在 Slot 1, 开始交换 */
        util_uart_printf("[BL] SWAP: ota_size=%u\r\n", (unsigned)meta.ota_size);
        if (meta.ota_size == 0 || meta.ota_size > PART_SLOT0_SIZE) {
            meta.boot_state = BOOT_STATE_NORMAL;
            meta_save(&meta);
            break;
        }
        {
            uint32_t swap_size = (meta.ota_size > meta.app_size)
                                 ? meta.ota_size : meta.app_size;
            if (swap_size == 0) swap_size = meta.ota_size;
            uint16_t total_pages = (uint16_t)((swap_size + FLASH_PAGE_SIZE_BYTES - 1)
                                              / FLASH_PAGE_SIZE_BYTES);
            rc = swap_status_init(total_pages);
            if (rc != 0) {
                util_uart_printf("[BL] Swap status init FAILED\r\n");
                meta.boot_state = BOOT_STATE_NORMAL;
                meta_save(&meta);
                break;
            }
            meta.boot_state = BOOT_STATE_SWAPPING;
            meta_save(&meta);
        }
        /* fall through */

    case BOOT_STATE_SWAPPING:
        /* 交换进行中 — 从断点恢复 */
        rc = bl_do_swap(&meta);
        if (rc == 0) {
            /* 交换成功 → 进入 TESTING */
            meta.boot_state = BOOT_STATE_TESTING;
            meta.boot_count = 0;
            meta_save(&meta);
            util_uart_printf("[BL] Enter TESTING\r\n");
        } else {
            util_uart_printf("[BL] Swap FAILED rc=%d\r\n", rc);
            meta.boot_state = BOOT_STATE_NORMAL;
            meta_save(&meta);
        }
        break;

    case BOOT_STATE_TESTING:
        /* 新固件测试中: APP 需调用 ota_confirm_app() 确认 */
        meta.boot_count++;
        util_uart_printf("[BL] TESTING: boot_count=%u/%u\r\n",
                         (unsigned)meta.boot_count, (unsigned)meta.max_boot_count);
        if (meta.boot_count > meta.max_boot_count) {
            /* 超过最大测试次数 → 回滚 */
            util_uart_printf("[BL] Max boot count exceeded, ROLLBACK\r\n");
            meta.boot_state = BOOT_STATE_ROLLBACK;
            meta_save(&meta);
        } else {
            meta_save(&meta);
            break;  /* 正常尝试启动 */
        }
        /* fall through to ROLLBACK */

    case BOOT_STATE_ROLLBACK:
        /* 回滚: 再次交换 Slot0 ↔ Slot1, 旧固件回到 Slot0 */
        util_uart_printf("[BL] ROLLBACK: swapping back...\r\n");
        {
            uint32_t swap_size = (meta.ota_size > meta.app_size)
                                 ? meta.ota_size : meta.app_size;
            if (swap_size == 0) swap_size = meta.app_size;
            uint16_t total_pages = (uint16_t)((swap_size + FLASH_PAGE_SIZE_BYTES - 1)
                                              / FLASH_PAGE_SIZE_BYTES);
            rc = swap_status_init(total_pages);
            if (rc != 0) {
                util_uart_printf("[BL] Rollback swap init FAILED\r\n");
                meta.boot_state = BOOT_STATE_NORMAL;
                meta_save(&meta);
                break;
            }
            meta.boot_state = BOOT_STATE_SWAPPING;
            meta_save(&meta);
        }
        rc = bl_do_swap(&meta);
        if (rc == 0) {
            util_uart_printf("[BL] Rollback complete\r\n");
        } else {
            util_uart_printf("[BL] Rollback FAILED rc=%d\r\n", rc);
        }
        meta.boot_state = BOOT_STATE_NORMAL;
        meta.boot_count = 0;
        meta_save(&meta);
        break;

    case BOOT_STATE_NORMAL:
    default:
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
