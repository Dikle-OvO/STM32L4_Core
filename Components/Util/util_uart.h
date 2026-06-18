#ifndef __UTIL_UART_H
#define __UTIL_UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  底层发送回调函数类型
 * @param  data: 待发送数据指针
 * @param  len:  数据长度
 * @return 0 成功, 负值失败
 */
typedef int (*util_uart_tx_fn_t)(const uint8_t *data, uint16_t len);

/**
 * @brief  注册 UART 发送回调
 * @param  tx_fn: 底层发送函数指针
 */
void util_uart_init(util_uart_tx_fn_t tx_fn);

/**
 * @brief  发送原始字节
 * @param  data: 数据指针
 * @param  len:  数据长度
 * @return 0 成功, 负值失败
 */
int util_uart_send(const uint8_t *data, uint16_t len);

/**
 * @brief  发送字符串 (不含末尾 '\0')
 * @param  str: 以 '\0' 结尾的字符串
 * @return 0 成功, 负值失败
 */
int util_uart_puts(const char *str);

/**
 * @brief  格式化打印 (类 printf, 输出到注册的 UART)
 * @param  fmt: 格式字符串
 * @return 实际发送字符数, 负值失败
 */
int util_uart_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __UTIL_UART_H */
