#ifndef KEY_H
#define KEY_H

#include "main.h"

/* ---- 事件类型 ---- */
typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_CLICK,            // 单击
    KEY_EVENT_DOUBLE_CLICK,     // 双击
    KEY_EVENT_LONG_PRESS,       // 长按 (>= LONG_PRESS 阈值)
} key_event_t;

/* ---- 时间参数 (ms), 可按需调整 ---- */
#define KEY_DEBOUNCE_MS       20
#define KEY_LONG_PRESS_MS     800
#define KEY_DOUBLE_GAP_MS     300

/* ---- API ---- */
void        key_init(void);
void        key_tick(void);         // 每 1 ms 调用 (SysTick)
key_event_t key_read(void);         // 读取并清除待处理事件
void        key_exti_cb(uint16_t pin);  // 放入 HAL_GPIO_EXTI_Callback

#endif /* KEY_H */
