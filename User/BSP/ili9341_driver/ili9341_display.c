/**
 * @file    ili9341_display.c
 * @brief   ILI9341 LCD 驱动实现, 通过 SPI1 接口控制 240x320 TFT 屏幕
 *
 * ========== 硬件接口 ==========
 *   - SPI1 总线 (与 VS1003/XPT2046/W25Q64 共享, 通过互斥锁保护)
 *   - CS:  片选 (SPI 分频 2 — 最高速 ~21MHz, 因为 LCD 数据传输量大)
 *   - DC:  数据/命令选择引脚
 *   - RST: 硬件复位引脚
 *
 * ========== 通信协议 ==========
 *   ILI9341 使用 SPI 模式 0 (CPOL=0, CPHA=0):
 *     1. 拉低 CS 获取总线
 *     2. DC=0: 发送命令字节
 *     3. DC=1: 发送参数/像素数据
 *     4. 拉高 CS 释放总线
 *
 * ========== 像素格式 ==========
 *   16 位 RGB565:
 *     bit15-11: R (红色, 5 位)
 *     bit10-5:  G (绿色, 6 位)
 *     bit4-0:   B (蓝色, 5 位)
 *
 * ========== 使用方法 ==========
 *   1. 调用 ILI9341_Init() 初始化
 *   2. 调用 ILI9341_Clear() 清屏
 *   3. 配合 LVGL 图形库使用
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

/*
 * 函数: ILI9341_WriteCS
 * 功能: 控制 ILI9341 的片选 (CS) 引脚
 *
 * 操作流程:
 *   拉低 CS 前:
 *     - 获取 SPI 互斥锁 (xSemaphoreTake)
 *     - 配置 SPI 为 2 分频 (~21MHz) — 最高速率, 提高刷新率
 *     - 重新初始化 SPI 以应用新分频
 *   拉高 CS 后:
 *     - 释放 SPI 互斥锁 (xSemaphoreGive)
 */
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

/*
 * 函数: ILI9341_WriteCmd
 * 功能: 向 ILI9341 写入一个命令字节 (DC=0)
 *
 * 典型命令如: 0x11 (SLPOUT — 退出睡眠), 0x29 (DISPON — 显示开启)
 */
void ILI9341_WriteCmd(uint8_t cmd)
{
    ILI9341_WriteCS(0);
    ILI9341_WriteDC(0);
    ILI9341_SPI_Transmit(&cmd, 1);
    ILI9341_WriteCS(1);
}

/*
 * 函数: ILI9341_WriteData8
 * 功能: 向 ILI9341 写入一个数据字节 (DC=1)
 */
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

/*
 * 函数: ILI9341_WritePixels
 * 功能: 批量写入像素数据到 ILI9341 GRAM (必须先在 SetRegion 后调用)
 *
 * 数据流优化:
 *   将像素数据分割为 ILI9341_CHUNK_SIZE (512) 个一批,
 *   每批转换为 SPI 友好的连续字节流 (RGB565 高低字节分开),
 *   一次性发送以减少 SPI 操作次数。
 *
 * 输入 data: RGB565 像素数组
 * 输出:      SPI 字节流 (高字节在前, 低字节在后)
 *
 * @param data  像素数组指针
 * @param len   像素数量 (注意: 不是字节数)
 */
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
        /*
         * 分批处理: 每次最多 CHUNK_SIZE 个像素
         * 转换 RGB565 像素为 SPI 字节流 (大端序)
         */
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

/*
 * 函数: ILI9341_SetRegion
 * 功能: 设置 ILI9341 的 GRAM 写入窗口区域
 *
 * ILI9341 的命令序列:
 *   CASET (0x2A): 设置列起始和结束地址
 *   PASET (0x2B): 设置行起始和结束地址
 *   RAMWR (0x2C): 开始写入 GRAM
 *
 * 之后调用 WritePixels 时, 像素将从 (x_start, y_start) 开始填充,
 * 自动换行到 (x_end, y_end) 结束。
 */
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

/*
 * 函数: ILI9341_DrawPixel
 * 功能: 在指定坐标绘制一个像素
 *
 * 实现: 设置 1×1 窗口区域后写入一个 RGB565 像素
 * 注意: 逐个像素绘制效率很低, 仅用于测试或画点
 */
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

/*
 * 函数: ILI9341_Clear
 * 功能: 用指定颜色填充整个屏幕
 *
 * 实现:
 *   1. 设置全屏窗口区域
 *   2. 创建一个 64 像素的颜色块
 *   3. 循环发送直到铺满全部 240×320=76800 个像素
 *
 * 优化: 使用固定颜色块 + 循环发送, 避免逐像素操作, 提高清屏速度
 */
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

/*
 * 函数: ILI9341_HardwareReset
 * 功能: 硬件复位 ILI9341 (拉低 RST 100ms → 释放 50ms)
 *
 * 复位时序要求:
 *   - RST 低电平保持至少 10us
 *   - 释放后需等待 120ms 以上 (退出睡眠模式)
 *   这里的延时留足了余量
 */
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

/*
 * ILI9341 初始化命令序列结构体:
 *   cmd:         命令字节
 *   params:      参数字节数组
 *   param_count: 参数个数
 */
typedef struct {
    uint8_t cmd;
    const uint8_t *params;
    uint8_t param_count;
} ILI9341_InitCmd_t;

/*
 * 电源/时序初始化命令序列:
 *   这些命令由 ILI9341 数据手册提供, 用于配置内部电源、VCOM、时序等。
 *   不同厂家/批次的 ILI9341 模组可能有不同的推荐值,
 *   以下是针对本项目使用的屏幕模组调试出的稳定参数。
 */
static const ILI9341_InitCmd_t s_init_sequence[] = {
    {0xCFU, (const uint8_t[]){0x00U, 0xC1U, 0x30U}, 3},   /* 电源控制 B */
    {0xEDU, (const uint8_t[]){0x64U, 0x03U, 0x12U, 0x81U}, 4}, /* 电源序列 */
    {0xE8U, (const uint8_t[]){0x85U, 0x11U, 0x78U}, 3},   /* 驱动时序 A */
    {0xF6U, (const uint8_t[]){0x01U, 0x30U, 0x00U}, 3},   /* 接口控制 */
    {0xCBU, (const uint8_t[]){0x39U, 0x2CU, 0x00U, 0x34U, 0x05U}, 5}, /* 电源控制 A */
    {0xF7U, (const uint8_t[]){0x20U}, 1},                  /* PUMP 控制 */
    {0xEAU, (const uint8_t[]){0x00U, 0x00U}, 2},           /* 电源序列参数 */
    {0xC0U, (const uint8_t[]){0x20U}, 1},                   /* 电源控制: BT = 0x20 */
    {0xC1U, (const uint8_t[]){0x11U}, 1},                   /* VCOM 控制 */
    {0xC5U, (const uint8_t[]){0x31U, 0x3CU}, 2},           /* VCOM 设置 */
    {0xC7U, (const uint8_t[]){0xA9U}, 1},                   /* VCOM 偏移 */
};

/* 正伽马校正曲线 (15 个参数, 控制 R/G/B 各灰阶的电压) */
static const uint8_t s_gamma_pos[] = {
    0x0FU, 0x17U, 0x14U, 0x09U, 0x0CU,
    0x06U, 0x43U, 0x75U, 0x36U, 0x08U,
    0x13U, 0x05U, 0x10U, 0x0BU, 0x08U
};

/* 负伽马校正曲线 (15 个参数) */
static const uint8_t s_gamma_neg[] = {
    0x00U, 0x1FU, 0x23U, 0x03U, 0x0EU,
    0x04U, 0x39U, 0x25U, 0x4DU, 0x06U,
    0x0DU, 0x0BU, 0x33U, 0x37U, 0x0FU
};

/*
 * 函数: ILI9341_Init
 * 功能: ILI9341 完整初始化序列
 *
 * 初始化步骤:
 *   1. 引脚初始化 + 硬件复位
 *   2. 退出睡眠模式 (SLPOUT) + 等待 120ms
 *   3. 发送电源/时序初始化命令序列
 *   4. 设置像素格式为 16 位 RGB565 (PIXFMT=0x55)
 *   5. 设置内存访问控制 (屏幕方向)
 *   6. 帧速率控制
 *   7. 显示功能控制
 *   8. 伽马校正
 *   9. 显示开启 (DISPON)
 */
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
