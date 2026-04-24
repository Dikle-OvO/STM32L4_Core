/**
 * @file  key.c
 * @brief 按键驱动 —— 中断唤醒 + SysTick 轮询状态机
 *
 * 设计思路
 * --------
 * EXTI 中断仅做 "唤醒标记"，实际消抖与手势识别全部在 key_tick()
 * 中以 1 ms 周期轮询引脚电平完成。这样做的好处：
 *   - ISR 极短（几条指令），不会占用 CPU 时间
 *   - 机械抖动产生的多次中断只会重复设 flag，不影响逻辑
 *   - 长按检测 **必须** 依赖周期性采样（中断只能感知边沿，
 *     无法感知"持续按住"），所以中断+轮询是最自然的混合方案
 *
 * 资源开销
 * --------
 *   - 中断: 每次按键触发 1~2 次 EXTI，ISR < 10 条指令
 *   - 轮询: key_tick() 每 1 ms 执行一次，正常路径 < 20 条指令
 *   - RAM:  ~8 字节静态变量
 *   → 对 80 MHz Cortex-M4 几乎零负担
 */

#include "key.h"

/* ---------- 内部状态 ---------- */
typedef enum {
    S_IDLE,             // 空闲，等待按下
    S_DOWN_DEBOUNCE,    // 消抖：确认按下
    S_PRESSED,          // 已按下，计时长按
    S_UP_DEBOUNCE,      // 消抖：确认释放
    S_WAIT_2ND,         // 等待第二次按下（双击窗口）
    S_DOWN2_DEBOUNCE,   // 消抖：确认第二次按下
    S_WAIT_RELEASE,     // 等待完全释放（长按/双击后）
} state_t;

static state_t          state;
static uint32_t         timer;
static volatile key_event_t pending_event;

/* 上拉按键: 按下 = LOW */
static inline int key_is_down(void)
{
    return HAL_GPIO_ReadPin(KEY_GPIO_Port, KEY_Pin) == GPIO_PIN_RESET;
}

/* ---------- API 实现 ---------- */

void key_init(void)
{
    state         = S_IDLE;
    timer         = 0;
    pending_event = KEY_EVENT_NONE;
}

void key_exti_cb(uint16_t pin)
{
    /* 中断只起唤醒作用，逻辑全在 key_tick() */
    (void)pin;
}

key_event_t key_read(void)
{
    key_event_t e = pending_event;
    pending_event = KEY_EVENT_NONE;
    return e;
}

void key_tick(void)
{
    int down = key_is_down();

    switch (state) {

    /* ---- 空闲 ---- */
    case S_IDLE:
        if (down) {
            state = S_DOWN_DEBOUNCE;
            timer = 0;
        }
        break;

    /* ---- 第一次按下消抖 ---- */
    case S_DOWN_DEBOUNCE:
        if (++timer >= KEY_DEBOUNCE_MS) {
            if (down) {
                state = S_PRESSED;
                timer = 0;
            } else {
                state = S_IDLE;         // 抖动，忽略
            }
        }
        break;

    /* ---- 已按下, 等待释放或长按 ---- */
    case S_PRESSED:
        ++timer;
        if (timer >= KEY_LONG_PRESS_MS) {
            pending_event = KEY_EVENT_LONG_PRESS;
            state = S_WAIT_RELEASE;
            timer = 0;
        } else if (!down) {
            state = S_UP_DEBOUNCE;
            timer = 0;
        }
        break;

    /* ---- 释放消抖 ---- */
    case S_UP_DEBOUNCE:
        if (++timer >= KEY_DEBOUNCE_MS) {
            if (!down) {
                state = S_WAIT_2ND;     // 进入双击等待窗口
                timer = 0;
            } else {
                state = S_PRESSED;      // 仍在按住（抖动）
                timer = 0;
            }
        }
        break;

    /* ---- 等待第二次按下（双击窗口） ---- */
    case S_WAIT_2ND:
        if (down) {
            state = S_DOWN2_DEBOUNCE;
            timer = 0;
        } else if (++timer >= KEY_DOUBLE_GAP_MS) {
            pending_event = KEY_EVENT_CLICK;    // 超时 → 单击
            state = S_IDLE;
        }
        break;

    /* ---- 第二次按下消抖 ---- */
    case S_DOWN2_DEBOUNCE:
        if (++timer >= KEY_DEBOUNCE_MS) {
            if (down) {
                pending_event = KEY_EVENT_DOUBLE_CLICK;
                state = S_WAIT_RELEASE;
                timer = 0;
            } else {
                state = S_WAIT_2ND;     // 抖动，退回等待
                timer = 0;
            }
        }
        break;

    /* ---- 等待按键完全释放 ---- */
    case S_WAIT_RELEASE:
        if (!down) {
            if (++timer >= KEY_DEBOUNCE_MS) {
                state = S_IDLE;
                timer = 0;
            }
        } else {
            timer = 0;
        }
        break;
    }
}
