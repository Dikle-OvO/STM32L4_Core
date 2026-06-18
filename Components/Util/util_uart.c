#include "util_uart.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* 内部持有的发送回调 */
static util_uart_tx_fn_t s_tx_fn = NULL;

/* printf 缓冲区大小 (Bootloader 场景无需太大) */
#define UTIL_UART_BUF_SIZE  128

void util_uart_init(util_uart_tx_fn_t tx_fn)
{
    s_tx_fn = tx_fn;
}

int util_uart_send(const uint8_t *data, uint16_t len)
{
    if (s_tx_fn == NULL || data == NULL || len == 0) {
        return -1;
    }
    return s_tx_fn(data, len);
}

int util_uart_puts(const char *str)
{
    if (s_tx_fn == NULL || str == NULL) {
        return -1;
    }
    uint16_t len = (uint16_t)strlen(str);
    if (len == 0) {
        return 0;
    }
    return s_tx_fn((const uint8_t *)str, len);
}

int util_uart_printf(const char *fmt, ...)
{
    if (s_tx_fn == NULL || fmt == NULL) {
        return -1;
    }

    char buf[UTIL_UART_BUF_SIZE];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len <= 0) {
        return len;
    }

    /* 截断保护 */
    if ((size_t)len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    int rc = s_tx_fn((const uint8_t *)buf, (uint16_t)len);
    return (rc == 0) ? len : rc;
}
