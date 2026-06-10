/**
 * @file    ili9341_touch.c
 * @brief   XPT2046 触摸控制器驱动实现, 通过 SPI 读取触摸坐标
 *
 *          使用方法:
 *          1. 调用 ILI9341_TouchInit() 初始化
 *          2. 周期调用 ILI9341_TouchScan() 扫描触摸
 *          3. 返回 1 表示按下, g_touch_dev.x/y 为坐标
 *
 * @date    2025-11-13
 */
#include "ili9341_touch.h"
#include "app.h"

/* ==================================================================== */
/*               采样/滤波参数                                           */
/* ==================================================================== */

#define TOUCH_FILTER_NUM   10U   /**< 中值平均滤波采样次数 */
#define TOUCH_X_MIN        201U  /**< X 有效范围下限 */
#define TOUCH_X_MAX        1819U /**< X 有效范围上限 */
#define TOUCH_Y_MIN        201U  /**< Y 有效范围下限 */
#define TOUCH_Y_MAX        1849U /**< Y 有效范围上限 */

/* ==================================================================== */
/*               静态内部函数: SPI + GPIO                                 */
/* ==================================================================== */

static void ILI9341_TouchDelay(uint32_t ms)
{
    vTaskDelay(ms);
}

static uint8_t ILI9341_TouchReadIRQ(void)
{
    return (uint8_t)HAL_GPIO_ReadPin(T_IRQ_GPIO_Port, T_IRQ_Pin);
}

static void ILI9341_TouchWriteCS(uint8_t state)
{
    if (state == 0)
    {
        xSemaphoreTake(g_spi1_mutex, portMAX_DELAY);
        hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_32;
        HAL_SPI_Init(&hspi1);
    }
    else
    {
        xSemaphoreGive(g_spi1_mutex);
    }
    HAL_GPIO_WritePin(T_CS_GPIO_Port, T_CS_Pin, (GPIO_PinState)state);
}

/* ==================================================================== */
/*               SPI 通信函数                                            */
/* ==================================================================== */

static void ILI9341_TouchSPI_Transmit(const uint8_t *data, uint16_t len)
{
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, 1000);
}

static void ILI9341_TouchSPI_Receive(uint8_t *data, uint16_t len)
{
    HAL_SPI_Receive(&hspi1, data, len, 1000);
}

/* ==================================================================== */
/*               ADC 读取与滤波                                          */
/* ==================================================================== */

static uint16_t ILI9341_TouchReadVoltage(uint8_t cmd)
{
    uint8_t rx_data[2] = {0};
    uint8_t tx_data = cmd;

    ILI9341_TouchWriteCS(0);
    ILI9341_TouchSPI_Transmit(&tx_data, 1);
    ILI9341_TouchDelay(1);
    ILI9341_TouchSPI_Receive(rx_data, 2);
    ILI9341_TouchWriteCS(1);

    return (uint16_t)(((uint16_t)rx_data[0] << 8U) | rx_data[1]) >> 4U;
}

static uint16_t ILI9341_TouchFilteredRead(uint8_t cmd)
{
    uint16_t buf[TOUCH_FILTER_NUM];
    uint16_t temp;
    uint32_t sum = 0;

    /* 连续采集 FILTER_NUM 次 */
    for (uint8_t i = 0; i < TOUCH_FILTER_NUM; i++)
    {
        buf[i] = ILI9341_TouchReadVoltage(cmd);
        ILI9341_TouchDelay(1);
    }

    /* 冒泡排序 */
    for (uint8_t i = 0; i < TOUCH_FILTER_NUM - 1; i++)
    {
        for (uint8_t j = i + 1; j < TOUCH_FILTER_NUM; j++)
        {
            if (buf[i] > buf[j])
            {
                temp = buf[i];
                buf[i] = buf[j];
                buf[j] = temp;
            }
        }
    }

    /* 去掉最小/最大值, 求平均 */
    for (uint8_t i = 1; i < TOUCH_FILTER_NUM - 1; i++)
    {
        sum += buf[i];
    }

    return (uint16_t)(sum / (TOUCH_FILTER_NUM - 2));
}

static uint8_t ILI9341_TouchReadXY(uint16_t *x, uint16_t *y)
{
    *x = ILI9341_TouchFilteredRead(XPT2046_CMD_RDX);
    *y = ILI9341_TouchFilteredRead(XPT2046_CMD_RDY);

    if ((*x > TOUCH_X_MIN && *x < TOUCH_X_MAX) &&
        (*y > TOUCH_Y_MIN && *y < TOUCH_Y_MAX))
    {
        return 1;
    }
    return 0;
}

/* ==================================================================== */
/*               全局变量定义                                            */
/* ==================================================================== */

ILI9341_Touch_t g_touch_dev = {0};

/* ==================================================================== */
/*               公开 API                                                */
/* ==================================================================== */

void ILI9341_TouchInit(void)
{
    /* 引脚初始化 */
    ILI9341_TouchWriteCS(1);

    /* 预读取一次坐标 (解决 LVGL 上电显示乱码问题) */
    ILI9341_TouchReadXY(&g_touch_dev.x, &g_touch_dev.y);

    /* 设置触摸矫正系数 (硬编码, 需针对具体屏幕校准) */
    g_touch_dev.xfac = 0.2f;
    g_touch_dev.xoff = -44;
    g_touch_dev.yfac = 0.15f;
    g_touch_dev.yoff = -37;
}

uint8_t ILI9341_TouchScan(uint8_t raw)
{
    if (ILI9341_TouchReadIRQ() != 0)
    {
        return 0; /* 未触摸 */
    }

    if (raw)
    {
        /* 模式1: 返回原始 ADC 值 */
        ILI9341_TouchReadXY(&g_touch_dev.x, &g_touch_dev.y);
    }
    else
    {
        /* 模式0: 使用矫正数据, 转换为屏幕像素坐标 */
        if (ILI9341_TouchReadXY(&g_touch_dev.x, &g_touch_dev.y))
        {
            int16_t temp_x = (int16_t)(g_touch_dev.xfac * (float)g_touch_dev.x) + g_touch_dev.xoff;
            int16_t temp_y = (int16_t)(g_touch_dev.yfac * (float)g_touch_dev.y) + g_touch_dev.yoff;

        #if ILI9341_TOUCH_DIRECTION == 0
            g_touch_dev.x = (uint16_t)(240 - temp_y);
            g_touch_dev.y = (uint16_t)(320 - temp_x);
        #elif ILI9341_TOUCH_DIRECTION == 1
            g_touch_dev.y = (uint16_t)temp_x;
            g_touch_dev.x = (uint16_t)temp_y;
        #elif ILI9341_TOUCH_DIRECTION == 2
            g_touch_dev.x = (uint16_t)(320 - temp_x);
            g_touch_dev.y = (uint16_t)temp_y;
        #elif ILI9341_TOUCH_DIRECTION == 3
            g_touch_dev.y = (uint16_t)(240 - temp_y);
            g_touch_dev.x = (uint16_t)temp_x;
        #endif
        }
        else
        {
            return 0;
        }
    }

    return 1;
}
