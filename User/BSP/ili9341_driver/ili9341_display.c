/**
 * @file    ili9341_display.c
 * @brief   ILI9341 LCD 驱动实现, 通过 SPI1 接口控制 240x320 TFT 屏幕
 *
 *          使用方法:
 *          1. 调用 ILI9341_Init() 初始化
 *          2. 调用 ILI9341_Clear() 清屏
 *          3. 配合 LVGL 图形库使用
 *
 * @date    2025-11-12
 */
#include "ili9341_display.h"
#include "app.h"

/* ==================================================================== */
/*               延迟与 SPI 底层接口                                      */
/* ==================================================================== */

static void ILI9341_Delay(uint32_t ms)
{
    vTaskDelay(ms);
}

static void ILI9341_SPI_Transmit(const uint8_t *data, uint16_t len)
{
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, 100);
}

/* ==================================================================== */
/*               GPIO 引脚控制                                            */
/* ==================================================================== */

static void ILI9341_WriteCS(uint8_t cs)
{
    if (cs == 0)
    {
        xSemaphoreTake(g_spi1_mutex, portMAX_DELAY);
        hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
        HAL_SPI_Init(&hspi1);
    }
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin,
                      cs ? GPIO_PIN_SET : GPIO_PIN_RESET);
    if (cs == 1)
    {
        xSemaphoreGive(g_spi1_mutex);
    }
}

static void ILI9341_WriteDC(uint8_t state)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin,
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ILI9341_WriteRST(uint8_t state)
{
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin,
                      state ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ILI9341_PinInit(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    ILI9341_WriteDC(1);
    ILI9341_WriteRST(1);
}

/* ==================================================================== */
/*               写命令/数据函数                                         */
/* ==================================================================== */

void ILI9341_WriteCmd(uint8_t cmd)
{
    ILI9341_WriteCS(0);
    ILI9341_WriteDC(0);
    ILI9341_SPI_Transmit(&cmd, 1);
    ILI9341_WriteCS(1);
}

void ILI9341_WriteData8(uint8_t data)
{
    ILI9341_WriteCS(0);
    ILI9341_WriteDC(1);
    ILI9341_SPI_Transmit(&data, 1);
    ILI9341_WriteCS(1);
}

/* ==================================================================== */
/*               高级绘图函数                                            */
/* ==================================================================== */

void ILI9341_WritePixels(const uint16_t *data, uint32_t len)
{
#define ILI9341_CHUNK_SIZE 512U

    uint8_t spi_buf[ILI9341_CHUNK_SIZE * 2];
    uint32_t remaining = len;
    uint32_t offset = 0;

    ILI9341_WriteCS(0);
    ILI9341_WriteDC(1);

    while (remaining > 0)
    {
        uint32_t chunk_len = (remaining > ILI9341_CHUNK_SIZE) ? ILI9341_CHUNK_SIZE : remaining;

        for (uint32_t i = 0; i < chunk_len; i++)
        {
            spi_buf[2U * i]     = (uint8_t)(data[offset + i] >> 8U);
            spi_buf[2U * i + 1] = (uint8_t)(data[offset + i] & 0xFFU);
        }

        ILI9341_SPI_Transmit(spi_buf, (uint16_t)(chunk_len * 2U));
        offset += chunk_len;
        remaining -= chunk_len;
    }

    ILI9341_WriteCS(1);
}

void ILI9341_SetRegion(uint16_t x_start, uint16_t y_start,
                       uint16_t x_end,   uint16_t y_end)
{
    ILI9341_WriteCmd(ILI9341_CMD_CASET);
    ILI9341_WriteData8((uint8_t)(x_start >> 8U));
    ILI9341_WriteData8((uint8_t)(x_start & 0xFFU));
    ILI9341_WriteData8((uint8_t)(x_end   >> 8U));
    ILI9341_WriteData8((uint8_t)(x_end   & 0xFFU));

    ILI9341_WriteCmd(ILI9341_CMD_PASET);
    ILI9341_WriteData8((uint8_t)(y_start >> 8U));
    ILI9341_WriteData8((uint8_t)(y_start & 0xFFU));
    ILI9341_WriteData8((uint8_t)(y_end   >> 8U));
    ILI9341_WriteData8((uint8_t)(y_end   & 0xFFU));

    ILI9341_WriteCmd(ILI9341_CMD_RAMWR);
}

void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color)
{
    ILI9341_SetRegion(x, y, x, y);
    ILI9341_WriteCS(0);
    ILI9341_WriteDC(1);
    {
        uint8_t high = (uint8_t)(color >> 8U);
        uint8_t low  = (uint8_t)(color & 0xFFU);
        ILI9341_SPI_Transmit(&high, 1);
        ILI9341_SPI_Transmit(&low, 1);
    }
    ILI9341_WriteCS(1);
}

void ILI9341_Clear(uint16_t color)
{
    const uint32_t total_pixels = (uint32_t)ILI9341_WIDTH * ILI9341_HEIGHT;
    const uint32_t chunk_pixels = 64U;

    ILI9341_SetRegion(0, 0, ILI9341_WIDTH - 1, ILI9341_HEIGHT - 1);

    uint16_t color_chunk[64];
    for (uint32_t i = 0; i < chunk_pixels; i++)
    {
        color_chunk[i] = color;
    }

    uint32_t remaining = total_pixels;
    ILI9341_WriteCS(0);
    ILI9341_WriteDC(1);

    while (remaining > 0)
    {
        uint32_t send_len = (remaining > chunk_pixels) ? chunk_pixels : remaining;
        uint8_t spi_buf[128];
        for (uint32_t i = 0; i < send_len; i++)
        {
            spi_buf[2U * i]     = (uint8_t)(color_chunk[i] >> 8U);
            spi_buf[2U * i + 1] = (uint8_t)(color_chunk[i] & 0xFFU);
        }
        ILI9341_SPI_Transmit(spi_buf, (uint16_t)(send_len * 2U));
        remaining -= send_len;
    }

    ILI9341_WriteCS(1);
}

void ILI9341_HardwareReset(void)
{
    ILI9341_WriteRST(0);
    ILI9341_Delay(100);
    ILI9341_WriteRST(1);
    ILI9341_Delay(50);
}

/* ==================================================================== */
/*               初始化命令序列                                          */
/* ==================================================================== */

typedef struct {
    uint8_t cmd;
    const uint8_t *params;
    uint8_t param_count;
} ILI9341_InitCmd_t;

/* 初始化命令表: cmd, params[], param_count */
static const ILI9341_InitCmd_t s_init_sequence[] = {
    {0xCFU, (const uint8_t[]){0x00U, 0xC1U, 0x30U}, 3},
    {0xEDU, (const uint8_t[]){0x64U, 0x03U, 0x12U, 0x81U}, 4},
    {0xE8U, (const uint8_t[]){0x85U, 0x11U, 0x78U}, 3},
    {0xF6U, (const uint8_t[]){0x01U, 0x30U, 0x00U}, 3},
    {0xCBU, (const uint8_t[]){0x39U, 0x2CU, 0x00U, 0x34U, 0x05U}, 5},
    {0xF7U, (const uint8_t[]){0x20U}, 1},
    {0xEAU, (const uint8_t[]){0x00U, 0x00U}, 2},
    {0xC0U, (const uint8_t[]){0x20U}, 1},
    {0xC1U, (const uint8_t[]){0x11U}, 1},
    {0xC5U, (const uint8_t[]){0x31U, 0x3CU}, 2},
    {0xC7U, (const uint8_t[]){0xA9U}, 1},
};

static const uint8_t s_gamma_pos[] = { /* 正伽马 15 参数 */
    0x0FU, 0x17U, 0x14U, 0x09U, 0x0CU,
    0x06U, 0x43U, 0x75U, 0x36U, 0x08U,
    0x13U, 0x05U, 0x10U, 0x0BU, 0x08U
};

static const uint8_t s_gamma_neg[] = { /* 负伽马 15 参数 */
    0x00U, 0x1FU, 0x23U, 0x03U, 0x0EU,
    0x04U, 0x39U, 0x25U, 0x4DU, 0x06U,
    0x0DU, 0x0BU, 0x33U, 0x37U, 0x0FU
};

void ILI9341_Init(void)
{
    /* 引脚初始化 + 硬件复位 */
    ILI9341_PinInit();
    ILI9341_HardwareReset();

    /* 退出睡眠模式 */
    ILI9341_WriteCmd(ILI9341_CMD_SLPOUT);
    ILI9341_Delay(120);

    /* 发送电源/时序初始化命令序列 */
    for (size_t i = 0; i < (sizeof(s_init_sequence) / sizeof(s_init_sequence[0])); i++)
    {
        ILI9341_WriteCmd(s_init_sequence[i].cmd);
        for (uint8_t j = 0; j < s_init_sequence[i].param_count; j++)
        {
            ILI9341_WriteData8(s_init_sequence[i].params[j]);
        }
    }

    /* 像素格式: 16位 RGB565 */
    ILI9341_WriteCmd(ILI9341_CMD_PIXFMT);
    ILI9341_WriteData8(0x55U);
    ILI9341_Delay(1);

    /* 内存访问控制 (屏幕方向) */
    ILI9341_WriteCmd(ILI9341_CMD_MADCTL);
#if ILI9341_DISPLAY_ORIENTATION == 0
    ILI9341_WriteData8(ILI9341_MADCTL_PORTRAIT_NORMAL);
#elif ILI9341_DISPLAY_ORIENTATION == 1
    ILI9341_WriteData8(ILI9341_MADCTL_PORTRAIT_REV);
#elif ILI9341_DISPLAY_ORIENTATION == 2
    ILI9341_WriteData8(ILI9341_MADCTL_LANDSCAPE_NORMAL);
#elif ILI9341_DISPLAY_ORIENTATION == 3
    ILI9341_WriteData8(ILI9341_MADCTL_LANDSCAPE_REV);
#endif
    ILI9341_Delay(1);

    /* 帧速率控制 */
    ILI9341_WriteCmd(0xB1U);
    ILI9341_WriteData8(0x00U);
    ILI9341_WriteData8(0x18U);

    /* 显示功能控制 */
    ILI9341_WriteCmd(0xB4U);
    ILI9341_WriteData8(0x00U);
    ILI9341_WriteData8(0x00U);

    /* 伽马校正 */
    ILI9341_WriteCmd(0xF2U);
    ILI9341_WriteData8(0x00U);

    ILI9341_WriteCmd(0x26U);
    ILI9341_WriteData8(0x01U);

    ILI9341_WriteCmd(0xE0U);
    for (int i = 0; i < 15; i++)
    {
        ILI9341_WriteData8(s_gamma_pos[i]);
    }

    ILI9341_WriteCmd(0xE1U);
    for (int i = 0; i < 15; i++)
    {
        ILI9341_WriteData8(s_gamma_neg[i]);
    }

    /* 显示开启 */
    ILI9341_WriteCmd(ILI9341_CMD_DISPON);
}
