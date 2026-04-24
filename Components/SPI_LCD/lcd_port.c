#include "main.h"
#include "lcd.h"
#include "lcd_port.h"

/************ Hardware Port ************/
void lcd_delay(uint32_t delay)
{
    HAL_Delay(delay);
}

static void lcd_io_ctrl(gpio_io* io, bool flag)
{
    if(io && io->port)
        HAL_GPIO_WritePin(io->port, io->pin, flag ^ io->invert);
}

/* 短数据阻塞发送 (寄存器命令 / 1~2 字节) */
static void lcd_spi_transmit(void* spi, uint8_t* data, uint32_t len)
{
    while(spi && len) {
        if(len > 0xffff) {
            len -= 0xffff;
            HAL_SPI_Transmit(spi, data, 0xffff, 0xffff);
        } else {
            HAL_SPI_Transmit(spi, data, len, 0xffff);
            break;
        }
    }
}

/* 大块数据 DMA 发送，自动等待完成 */
static void lcd_spi_transmit_dma(void* spi, uint8_t* data, uint32_t len)
{
    if(!spi) return;
    while(len) {
        uint16_t chunk = (len > 0xffff) ? 0xffff : (uint16_t)len;
        HAL_SPI_Transmit_DMA(spi, data, chunk);
        while(HAL_SPI_GetState(spi) != HAL_SPI_STATE_READY) {}
        data += chunk;
        len  -= chunk;
    }
}

/************ GPIO ************/
void lcd_io_rst(lcd_io* lcdio, bool flag)
{
    lcd_io_ctrl(&lcdio->rst, flag);
}

void lcd_io_bl(lcd_io* lcdio, bool flag)
{
    lcd_io_ctrl(&lcdio->bl, flag);
}

void lcd_io_cs(lcd_io* lcdio, bool flag)
{
    lcd_io_ctrl(&lcdio->cs, flag);
}

void lcd_io_dc(lcd_io* lcdio, bool flag)
{
    lcd_io_ctrl(&lcdio->dc, flag);
}

/************ SPI ************/
void lcd_write_byte(lcd_io* lcdio, uint8_t data)
{
    lcd_io_dc(lcdio, 1);
    lcd_spi_transmit(lcdio->spi, &data, 0x01);
}

void lcd_write_halfword(lcd_io* lcdio, uint16_t data)
{
    lcd_io_dc(lcdio, 1);
    /* note: 使用HAL库一次发送两个字节顺序与屏幕定义顺序相反 */
    data = (data << 8) | (data >> 8);
    lcd_spi_transmit(lcdio->spi, (uint8_t *)&data, 0x02);
}

void lcd_write_bulk(lcd_io* lcdio, uint8_t* data, uint32_t len)
{
    lcd_io_dc(lcdio, 1);
    lcd_spi_transmit_dma(lcdio->spi, (uint8_t *)data, len);
}

void lcd_write_reg(lcd_io* lcdio, uint8_t data)
{
    lcd_io_dc(lcdio, 0);
    lcd_spi_transmit(lcdio->spi, &data, 0x01);
}
