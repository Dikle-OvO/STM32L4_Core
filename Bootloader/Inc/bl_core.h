#ifndef __BL_CORE_H
#define __BL_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* APP 入口地址 (Slot 0 起始) */
#define APP_ADDR        (FLASH_BASE_ADDR + PART_SLOT0_OFFSET)

/**
 * @brief  Bootloader 主逻辑: 检查元数据、执行 swap、跳转 APP
 * @note   此函数正常情况不返回 (跳转到 APP)
 */
void bl_run(void);

/**
 * @brief  验证 APP 镜像有效性 (检查 SP 和 Reset_Handler)
 * @param  addr: APP 起始地址
 * @return 0 有效, -1 无效
 */
int bl_validate_image(uint32_t addr);

/**
 * @brief  跳转到 APP
 * @param  addr: APP 起始地址
 * @note   此函数不返回
 */
void bl_jump_to_app(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* __BL_CORE_H */
