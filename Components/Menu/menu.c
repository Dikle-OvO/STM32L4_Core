/**
 * @file  menu.c
 * @brief 极客风菜单系统 —— 单击切换 / 双击确认 / 长按返回
 *
 * 页面:
 *   MAIN     ── 主菜单 (System Info / LED Control / About / Test)
 *   SYSINFO  ── 系统信息 (MCU、时钟、Flash、HAL 版本)
 *   LED      ── LED 亮灯控制 (R / G / B 独立开关)
 *   ABOUT    ── 关于 (固件版本、编译日期、LCD 型号)
 *   TEST     ── 测试 (全屏刷色 / OTA参数查看)
 */

#include "menu.h"
#include "main.h"
#include "ota_core.h"
#include "ota_proto.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* OTA 全局上下文 (定义在 main.c) */
extern ota_ctx_t   g_ota_ctx;
extern proto_ctx_t g_proto_ctx;
extern volatile uint8_t g_frame_ready;

/* ota_proto.h 中的帧处理函数 */
void proto_process(proto_ctx_t *ctx);

/* ====================== 页面枚举 ====================== */
typedef enum {
    PAGE_MAIN = 0,
    PAGE_SYSINFO,
    PAGE_LED,
    PAGE_ABOUT,
    PAGE_TEST,
} page_t;

#define MAIN_ITEMS   4
#define TEST_ITEMS   2
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

/* ---------- OTA 参数查询 (像素级局部刷新, 不闪屏) ---------- */
/* FONT_1608 每字符 8px 宽, mline 每行固定 30 字符 = 240px = 满宽 */
/* 每行布局: "  LABEL : VALUE________________" (VALUE 从固定列开始) */
#define COL_VAL_STATE     (13 * 8)   /* "  State   : " = 13 字符 → x=104 */
#define COL_VAL_PROGRESS  (11 * 8)   /* " Progress: "    = 11 字符 → x=88  */
#define COL_VAL_SIZE      (11 * 8)   /* " FW Size : "    = 11 字符 → x=88  */
#define COL_VAL_OTAVER    (11 * 8)   /* " OTA Ver : "    = 11 字符 → x=88  */
#define WID_VAL           17         /* value 区域字符数 (30 - prefix) */

/* 仅在指定 (x,y) 处绘制 max_w 字符的文本, 自动清除旧值残余 */
static void draw_val(uint16_t x, uint16_t y, uint16_t fg,
                     uint8_t max_w, const char *fmt, ...)
{
    char buf[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* 填充空格到 max_w, 覆盖可能更长的旧值 */
    int len = (int)strlen(buf);
    while (len < max_w) buf[len++] = ' ';
    buf[max_w] = '\0';

    lcd_set_font(dev, FONT_1608, fg, C_BG);
    lcd_show_string(dev, x, y, (const uint8_t *)buf);
}

static void run_ota_query(void)
{
    static const char *state_names[] = {
        "IDLE", "HEADER", "RECEIVING",
        "VERIFYING", "DONE", "ERROR"
    };
    uint32_t app_ver = 0x00010000U;

    ota_state_t last_state   = 99;
    uint8_t     last_pct     = 0xFF;
    uint32_t    last_fw_size = 0xFFFFFFFFU;
    uint32_t    last_fw_ver  = 0xFFFFFFFFU;
    uint32_t    refresh_tick = 0;

    /* ── 首帧: 全屏绘制 (只这一次全刷) ── */
    lcd_clear(dev, C_BG);
    mline(0,    C_TITLE, "  <*> OTA STATUS <*>");
    mline(16,   C_BAR,   "==============================");

    /* 静态标签行 (label 部分不变, value 留给 draw_val 更新) */
    mline(32,   C_TEXT,  " State   :");
    mline(48,   C_TEXT,  " Progress:");
    mline(64,   C_TEXT,  " FW Size :");

    mline(80,   C_TEXT,  " App Ver : v%lu.%lu.%lu (0x%08lX)",
          (unsigned long)((app_ver >> 16) & 0xFF),
          (unsigned long)((app_ver >> 8)  & 0xFF),
          (unsigned long)( app_ver        & 0xFF),
          (unsigned long)app_ver);
    mline(96,   C_TEXT,  " OTA Ver :");
    mline(112,  C_DIM,   " Any key to return");

    while (1) {
        uint32_t now = HAL_GetTick();

        if ((now - refresh_tick) >= 300 || refresh_tick == 0) {
            refresh_tick = now;

            ota_state_t state = ota_get_state(&g_ota_ctx);
            uint8_t    pct    = ota_get_progress(&g_ota_ctx);
            uint32_t   fw_size = g_ota_ctx.header.fw_size;
            uint32_t   fw_ver  = g_ota_ctx.header.fw_version;

            /* State (仅变化时更新) */
            if (state != last_state) {
                last_state = state;
                const char *sname = (state <= OTA_STATE_ERROR)
                                    ? state_names[state] : "???";
                uint16_t sc = C_TEXT;
                if (state == OTA_STATE_ERROR)      sc = RED;
                else if (state == OTA_STATE_DONE)  sc = GREEN;
                else if (state != OTA_STATE_IDLE)  sc = YELLOW;
                draw_val(COL_VAL_STATE, 32, sc, WID_VAL, "%s", sname);
            }

            /* Progress (仅百分比变化时更新) */
            if (pct != last_pct) {
                last_pct = pct;
                draw_val(COL_VAL_PROGRESS, 48, C_TEXT, WID_VAL,
                         "%u%%", (unsigned)pct);
            }

            /* FW Size (仅变化时更新) */
            if (fw_size != last_fw_size) {
                last_fw_size = fw_size;
                draw_val(COL_VAL_SIZE, 64, C_TEXT, WID_VAL,
                         "%lu B", (unsigned long)fw_size);
            }

            /* OTA Ver (状态或版本变化时更新) */
            if (state != last_state || fw_ver != last_fw_ver) {
                last_fw_ver = fw_ver;
                if (state != OTA_STATE_IDLE && fw_ver != 0) {
                    draw_val(COL_VAL_OTAVER, 96, C_TEXT, WID_VAL,
                             "v%lu.%lu.%lu",
                             (unsigned long)((fw_ver >> 16) & 0xFF),
                             (unsigned long)((fw_ver >> 8)  & 0xFF),
                             (unsigned long)( fw_ver        & 0xFF));
                } else {
                    draw_val(COL_VAL_OTAVER, 96, C_DIM, WID_VAL, "N/A");
                }
            }
        }

        /* 处理 OTA 帧 */
        if (g_frame_ready) {
            g_frame_ready = 0;
            proto_process(&g_proto_ctx);
        }

        /* 任意键退出 (事件被消耗, 返回后 menu_process 直接 draw_page) */
        key_event_t evt = key_read();
        if (evt != KEY_EVENT_NONE) {
            break;
        }

        HAL_Delay(10);
    }
}

static void draw_test(void)
{
    static const char *items[] = {"Fill Screen", "OTA Status"};

    mline(0,   C_TITLE, "  <*> TEST <*>");
    mline(16,  C_BAR,   "==============================");

    for (int i = 0; i < TEST_ITEMS; i++) {
        uint16_t fg = (i == cursor) ? C_SEL : C_TEXT;
        char cur    = (i == cursor) ? '>' : ' ';
        mline(32 + i * 16, fg, "  %c %s", cur, items[i]);
    }

    /* 空白占位从 item 列表末尾开始 */
    mline(32 + TEST_ITEMS * 16,  C_BG,  "");
    mline(32 + TEST_ITEMS * 16 + 16, C_BG, "");
    mline(32 + TEST_ITEMS * 16 + 32, C_BG, "");
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
            else if (cursor == 1) run_ota_query();
            /* 子函数退出后立即回到测试页, 只需按一次键 */
            draw_page();
            return;
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
