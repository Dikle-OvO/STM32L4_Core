/**
 * Bootloader main entry point
 * Minimal initialization -> check metadata -> swap/jump to APP
 */
#include "stm32l4xx_hal.h"
#include "bl_core.h"
#include "util_uart.h"

/* Forward declarations for minimal HW init */
static void SystemClock_Config(void);
static void BL_USART1_Init(void);

/* UART handle for bootloader debug output */
static UART_HandleTypeDef bl_huart1;

/**
 * @brief  SysTick 中断处理 (HAL_Init 会启用 SysTick, 必须提供此函数)
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/**
 * @brief  UART 发送回调 (直接寄存器轮询, 不依赖 HAL tick)
 */
static int bl_uart_tx(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        while (!(USART1->ISR & USART_ISR_TXE)) {}
        USART1->TDR = data[i];
    }
    while (!(USART1->ISR & USART_ISR_TC)) {}
    return 0;
}

int main(void)
{
    /* BL 向量表在 0x08000000, 必须在 HAL_Init 之前矫正 VTOR
     * (system_stm32l4xx.c 的 SystemInit 会将 VTOR 设为 APP 偏移 0x4000) */
    SCB->VTOR = FLASH_BASE;

    /* Minimal HAL init */
    HAL_Init();
    SystemClock_Config();

    /* Initialize debug UART and register callback */
    BL_USART1_Init();
    util_uart_init(bl_uart_tx);

    util_uart_printf("[BL] Bootloader started\r\n");

    /* Run bootloader logic (does not return if APP is valid) */
    bl_run();

    util_uart_printf("[BL] No valid APP, staying in bootloader\r\n");

    /* Should never reach here */
    while (1) {
        HAL_Delay(1000);
    }
}

/* HAL assert stub */
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    while (1) {}
}
#endif

/**
 * @brief Minimal clock config for bootloader
 *        Use HSI 16MHz - fast startup, no external crystal dependency
 */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Configure HSI as system clock source */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    /* Select HSI as SYSCLK, no prescaler */
    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

/**
 * @brief  Bootloader 专用 USART1 初始化 (PA9-TX, PA10-RX, 115200)
 */
static void BL_USART1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Enable clocks */
    __HAL_RCC_USART1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA9 = TX, PA10 = RX */
    GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* UART config: 115200, 8N1 */
    bl_huart1.Instance = USART1;
    bl_huart1.Init.BaudRate = 115200;
    bl_huart1.Init.WordLength = UART_WORDLENGTH_8B;
    bl_huart1.Init.StopBits = UART_STOPBITS_1;
    bl_huart1.Init.Parity = UART_PARITY_NONE;
    bl_huart1.Init.Mode = UART_MODE_TX;
    bl_huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    bl_huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    bl_huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    bl_huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    HAL_UART_Init(&bl_huart1);
}
