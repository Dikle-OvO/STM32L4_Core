/**
 * @file  menu.c
 * @brief 极客风菜单系统 —— 单击切换 / 双击确认 / 长按返回
 *
 * 页面:
 *   MAIN     ── 主菜单 (System Info / LED Control / About)
 *   SYSINFO  ── 系统信息 (MCU、时钟、Flash、HAL 版本)
 *   LED      ── LED 亮灯控制 (R / G / B 独立开关)
 *   ABOUT    ── 关于 (固件版本、编译日期、LCD 型号)
 */

#include "menu.h"
#include "main.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* ====================== 页面枚举 ====================== */
typedef enum {
    PAGE_MAIN = 0,
    PAGE_SYSINFO,
    PAGE_LED,
    PAGE_ABOUT,
    PAGE_TEST,
} page_t;

#define MAIN_ITEMS   4
#define TEST_ITEMS   1
#define LED_ITEMS    3

/* ============ 极客配色 (绿色终端风格) ============ */
#define C_TITLE  CYAN
#define C_TEXT   GREEN
#define C_SEL    CYAN
#define C_DIM    GRAY
#define C_BG     BLACK
#define C_BAR    GREEN

/* ====================== 内部状态 ====================== */
static lcd     *dev;
static page_t   page;
static uint8_t  cursor;

/* ---------- 辅助: 画一行 30 字符宽，自动补空格 ---------- */
static void mline(uint16_t y, uint16_t fg, const char *fmt, ...)
{
    char buf[31];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 补空格至 30 字符宽 */
    int len = (int)strlen(buf);
    while (len < 30) buf[len++] = ' ';
    buf[30] = '\0';

    lcd_set_font(dev, FONT_1608, fg, C_BG);
    lcd_show_string(dev, 0, y, (const uint8_t *)buf);
}

/* ====================================================== */
/*                    各页面绘制                           */
/* ====================================================== */

static void draw_main(void)
{
    static const char *items[] = {"System Info", "LED Control", "About", "Test"};

    mline(0,   C_TITLE, "  <*> STM32L4 CORE <*>");
    mline(16,  C_BAR,   "==============================");

    for (int i = 0; i < MAIN_ITEMS; i++) {
        uint16_t fg = (i == cursor) ? C_SEL : C_TEXT;
        char cur    = (i == cursor) ? '>' : ' ';
        mline(32 + i * 16, fg, "  %c %s", cur, items[i]);
    }

    mline(96,  C_BAR, "==============================");
    mline(112, C_DIM, " Clk:Nav DClk:OK Long:Back");
}

static void draw_sysinfo(void)
{
    unsigned clk_mhz  = (unsigned)(HAL_RCC_GetSysClockFreq() / 1000000U);
    uint16_t flash_kb = *(volatile uint16_t *)0x1FFF75E0;
    uint32_t hal_ver  = HAL_GetHalVersion();

    mline(0,   C_TITLE, "  <*> SYSTEM INFO <*>");
    mline(16,  C_BAR,   "==============================");
    mline(32,  C_TEXT,  " MCU  : STM32L431CBT6");
    mline(48,  C_TEXT,  " Core : Cortex-M4 @%uMHz", clk_mhz);
    mline(64,  C_TEXT,  " Flash: %uKB  RAM: 64KB", flash_kb);
    mline(80,  C_TEXT,  " HAL  : v%u.%u.%u",
          (unsigned)((hal_ver >> 24) & 0xFF),
          (unsigned)((hal_ver >> 16) & 0xFF),
          (unsigned)((hal_ver >> 8)  & 0xFF));
    mline(96,  C_BAR,   "==============================");
    mline(112, C_DIM,   " BACK: DClk / LongPress");
}

static void draw_led(void)
{
    static const char     *names[]  = {"RED  ", "GREEN", "BLUE "};
    static const uint16_t  colors[] = {RED, GREEN, BLUE};

    GPIO_PinState st[3];
    st[0] = HAL_GPIO_ReadPin(RED_GPIO_Port,   RED_Pin);
    st[1] = HAL_GPIO_ReadPin(GREEN_GPIO_Port, GREEN_Pin);
    st[2] = HAL_GPIO_ReadPin(BLUE_GPIO_Port,  BLUE_Pin);

    mline(0,   C_TITLE, "  <*> LED CONTROL <*>");
    mline(16,  C_BAR,   "==============================");

    for (int i = 0; i < LED_ITEMS; i++) {
        char cur = (i == cursor) ? '>' : ' ';
        const char *on_off = st[i] ? " ON" : "OFF";
        uint16_t fg;
        if (i == cursor)
            fg = C_SEL;
        else if (st[i])
            fg = colors[i];
        else
            fg = C_DIM;
        mline(32 + i * 16, fg, "  %c %s  [%s]", cur, names[i], on_off);
    }

    mline(80,  C_BG,  "");
    mline(96,  C_BAR, "==============================");
    mline(112, C_DIM, " Clk:Nav DClk:Tog Long:Back");
}

static void draw_about(void)
{
    mline(0,   C_TITLE, "  <*> ABOUT <*>");
    mline(16,  C_BAR,   "==============================");
    mline(32,  C_TEXT,  " FW    : v1.0.0");
    mline(48,  C_TEXT,  " Build : %s", __DATE__);
    mline(64,  C_TEXT,  " LCD   : 1.14\" 240x135");
    mline(80,  C_TEXT,  " Repo  : STM32L4_Core");
    mline(96,  C_BAR,   "==============================");
    mline(112, C_DIM,   " BACK: DClk / LongPress");
}

/* ---------- 全屏刷色测试 ---------- */
static void run_fill_test(void)
{
    static const uint16_t colors[] = {RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE, BLACK};
    uint32_t total_ms = 0;

    /* 先刷一帧同步相位：连续两帧对齐到相同的 VSYNC 相位 */
    lcd_clear(dev, BLACK);
    HAL_Delay(17);  /* 等待一个完整 VSYNC 周期 (16.7ms) */

    for (int i = 0; i < 8; i++) {
        uint32_t t0 = HAL_GetTick();
        lcd_clear(dev, colors[i]);
        uint32_t dt = HAL_GetTick() - t0;
        total_ms += dt;
        /* 等够一个 VSYNC 周期，保持与面板刷新同相 */
        if (dt < 17) HAL_Delay(17 - dt);
        /* 再额外停留让颜色能看清 */
        HAL_Delay(283);
    }

    lcd_clear(dev, C_BG);
    mline(0,   C_TITLE, "  <*> FILL RESULT <*>");
    mline(16,  C_BAR,   "==============================");
    mline(32,  C_TEXT,  " 8 colors fullscreen fill");
    mline(48,  C_TEXT,  " Total : %lu ms", (unsigned long)total_ms);
    mline(64,  C_TEXT,  " Avg   : %lu ms/frame", (unsigned long)(total_ms / 8));
    mline(80,  C_TEXT,  " %lux%lu  @SPI/DMA",
          (unsigned long)dev->hw->width,
          (unsigned long)dev->hw->height);
    mline(96,  C_BAR,   "==============================");
    mline(112, C_DIM,   " Any key to return");
}

static void draw_test(void)
{
    static const char *items[] = {"Fill Screen"};

    mline(0,   C_TITLE, "  <*> TEST <*>");
    mline(16,  C_BAR,   "==============================");

    for (int i = 0; i < TEST_ITEMS; i++) {
        uint16_t fg = (i == cursor) ? C_SEL : C_TEXT;
        char cur    = (i == cursor) ? '>' : ' ';
        mline(32 + i * 16, fg, "  %c %s", cur, items[i]);
    }

    mline(48,  C_BG,  "");
    mline(64,  C_BG,  "");
    mline(80,  C_BG,  "");
    mline(96,  C_BAR, "==============================");
    mline(112, C_DIM, " Clk:Nav DClk:Run Long:Back");
}

/* ====================================================== */
/*                     统一刷新                            */
/* ====================================================== */

static void draw_page(void)
{
    switch (page) {
    case PAGE_MAIN:    draw_main();    break;
    case PAGE_SYSINFO: draw_sysinfo(); break;
    case PAGE_LED:     draw_led();     break;
    case PAGE_ABOUT:   draw_about();   break;
    case PAGE_TEST:    draw_test();    break;
    }
}

/* ====================================================== */
/*                     公开 API                            */
/* ====================================================== */

void menu_init(lcd *plcd)
{
    dev    = plcd;
    page   = PAGE_MAIN;
    cursor = 0;
    draw_page();
}

static const page_t main_targets[] = {PAGE_SYSINFO, PAGE_LED, PAGE_ABOUT, PAGE_TEST};

void menu_process(key_event_t evt)
{
    if (evt == KEY_EVENT_NONE) return;

    switch (page) {

    /* ---- 主菜单 ---- */
    case PAGE_MAIN:
        if (evt == KEY_EVENT_CLICK) {
            cursor = (cursor + 1) % MAIN_ITEMS;
        } else if (evt == KEY_EVENT_DOUBLE_CLICK) {
            page   = main_targets[cursor];
            cursor = 0;
        }
        break;

    /* ---- 系统信息 / 关于: 双击或长按返回 ---- */
    case PAGE_SYSINFO:
    case PAGE_ABOUT:
        if (evt == KEY_EVENT_DOUBLE_CLICK || evt == KEY_EVENT_LONG_PRESS) {
            page   = PAGE_MAIN;
            cursor = 0;
        }
        break;

    /* ---- 测试: 单击切换项, 双击执行, 长按返回 ---- */
    case PAGE_TEST:
        if (evt == KEY_EVENT_CLICK) {
            cursor = (cursor + 1) % TEST_ITEMS;
        } else if (evt == KEY_EVENT_DOUBLE_CLICK) {
            if (cursor == 0) run_fill_test();
            return;  /* 结果页面已绘制，等待任意键 */
        } else if (evt == KEY_EVENT_LONG_PRESS) {
            page   = PAGE_MAIN;
            cursor = 0;
        }
        break;

    /* ---- LED 控制: 单击切换项, 双击开关, 长按返回 ---- */
    case PAGE_LED:
        if (evt == KEY_EVENT_CLICK) {
            cursor = (cursor + 1) % LED_ITEMS;
        } else if (evt == KEY_EVENT_DOUBLE_CLICK) {
            switch (cursor) {
            case 0: HAL_GPIO_TogglePin(RED_GPIO_Port,   RED_Pin);   break;
            case 1: HAL_GPIO_TogglePin(GREEN_GPIO_Port, GREEN_Pin); break;
            case 2: HAL_GPIO_TogglePin(BLUE_GPIO_Port,  BLUE_Pin);  break;
            }
        } else if (evt == KEY_EVENT_LONG_PRESS) {
            page   = PAGE_MAIN;
            cursor = 0;
        }
        break;
    }

    draw_page();
}
