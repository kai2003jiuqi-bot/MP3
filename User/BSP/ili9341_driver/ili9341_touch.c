/**
 * @file    ili9341_touch.c
 * @brief   XPT2046 触摸控制器驱动实现, 通过 SPI 读取触摸坐标
 *
 * ========== XPT2046 简介 ==========
 * XPT2046 是一个 4 线电阻式触摸屏控制器,
 * 内置 12 位 ADC, 用于检测触摸屏上的压力位置。
 * 与 ILI9341 共用 SPI1 总线, 独立 CS 引脚 (T_CS)。
 *
 * ========== SPI 通信 ==========
 *   命令格式:
 *     字节 0: 8 位命令 (含起始位、通道选择、模式等)
 *     字节 1-2: 12 位 ADC 值返回 (右对齐)
 *
 *   通道选择:
 *     XPT2046_CMD_RDX (0xD0):  测量 X 坐标
 *     XPT2046_CMD_RDY (0x90):  测量 Y 坐标
 *
 * ========== 滤波算法 ==========
 *   中值平均滤波:
 *     1. 连续采样 TOUCH_FILTER_NUM (10) 次
 *     2. 冒泡排序去除最大值和最小值
 *     3. 剩余 8 个值取平均
 *     该方法能有效抑制触摸检测中的随机噪声。
 *
 * ========== 坐标矫正 ==========
 *   触摸屏原始 ADC 值需要转换为 LCD 像素坐标,
 *   使用线性矫正公式:
 *     pixel_x = xfac * adc_x + xoff
 *     pixel_y = yfac * adc_y + yoff
 *   矫正系数需针对具体屏幕模组校准。
 *
 * ========== 使用方法 ==========
 *   1. 调用 ILI9341_TouchInit() 初始化
 *   2. 周期调用 ILI9341_TouchScan() 扫描触摸
 *   3. 返回 1 表示按下, g_touch_dev.x/y 为坐标
 *
 * @date    2025-11-13
 */
#include "ili9341_touch.h"
#include "app.h"

/* ==================================================================== */
/*               采样/滤波参数                                           */
/* ==================================================================== */

#define TOUCH_FILTER_NUM   10U   /**< 中值平均滤波采样次数 */
#define TOUCH_X_MIN        201U  /**< X 有效范围下限 (低于此值为噪声) */
#define TOUCH_X_MAX        1819U /**< X 有效范围上限 (高于此值为噪声) */
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

/*
 * 函数: ILI9341_TouchWriteCS
 * 功能: 控制 XPT2046 的片选 (T_CS) 引脚
 *
 * SPI 分频: 32 (~1.3MHz) — XPT2046 不需要高速, 慢速更稳定
 *
 * 获取互斥锁 → 配置分频 → 初始化 SPI → 拉低 CS
 * 或: 拉高 CS → 释放互斥锁
 */
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

/*
 * 函数: ILI9341_TouchReadVoltage
 * 功能: 通过 XPT2046 读取一个坐标通道的 ADC 值
 *
 * 操作流程:
 *   1. 拉低 CS
 *   2. 发送 8 位命令 (选择 X/Y 通道)
 *   3. 等待 1ms (ADC 转换时间)
 *   4. 接收 2 字节 12 位 ADC 值 (右对齐, 高 4 位为零)
 *   5. 拉高 CS
 *   6. 返回 12 位 ADC 值 (右移 4 位后为 0~4095)
 */
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

/*
 * 函数: ILI9341_TouchFilteredRead
 * 功能: 对指定通道进行多次采样, 应用中值平均滤波后返回稳定值
 *
 * 算法步骤:
 *   1. 连续采样 FILTER_NUM (10) 次, 存入缓冲
 *   2. 冒泡排序从小到大排列
 *   3. 去掉最小值和最大值 (各去掉一个, 去除极端噪声)
 *   4. 剩余 8 个值取算术平均
 */
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

/*
 * 函数: ILI9341_TouchReadXY
 * 功能: 读取触摸点 X/Y 原始 ADC 值
 *
 * 返回: 1 = 触摸有效 (在有效范围内), 0 = 超出范围 (视为未触摸)
 *
 * 有效范围过滤:
 *   触摸屏边缘的 ADC 值不稳定, 且在物理上位于屏幕有效区域之外,
 *   通过设置 TOUCH_X/Y_MIN/MAX 阈值剔除边缘噪声。
 */
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

/*
 * 函数: ILI9341_TouchInit
 * 功能: 初始化 XPT2046 触摸控制器
 *
 * 初始化步骤:
 *   1. CS 置高 (不选中)
 *   2. 预读取一次坐标 — 解决 LVGL 上电首次显示时的坐标乱码问题
 *      首次读取的 ADC 值可能因外部电容充电未完成而不稳定,
 *      丢弃第一次的读取结果。
 *   3. 设置坐标矫正系数
 *
 * 矫正系数说明:
 *   触摸屏的 ADC 值和 LCD 像素坐标之间是线性关系,
 *   但具体系数因屏幕模组不同而有差异。
 *   这里的系数是硬编码的近似值, 精确校准需要使用三点校准法。
 */
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

/*
 * 函数: ILI9341_TouchScan
 * 功能: 扫描触摸屏, 获取当前触摸点的坐标
 *
 * 模式选择:
 *   raw == 1: 返回原始 ADC 值 (g_touch_dev.x/y 为 12 位 ADC 值)
 *   raw == 0: 返回矫正后的像素坐标
 *     - 水平方向 240 像素
 *     - 垂直方向 320 像素
 *
 * 坐标映射:
 *   根据 ILI9341_TOUCH_DIRECTION 配置,
 *   将触摸屏物理坐标映射到 LCD 显示方向。
 *
 * 返回: 1 = 触摸按下, 0 = 未触摸
 */
uint8_t ILI9341_TouchScan(uint8_t raw)
{
    /* IRQ 引脚高电平 = 未触摸 (XPT2046 在检测到触摸时拉低 IRQ) */
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
            /*
             * 线性矫正: 将 ADC 值映射到像素坐标
             *   temp_x = xfac * adc_x + xoff
             *   temp_y = yfac * adc_y + yoff
             */
            int16_t temp_x = (int16_t)(g_touch_dev.xfac * (float)g_touch_dev.x) + g_touch_dev.xoff;
            int16_t temp_y = (int16_t)(g_touch_dev.yfac * (float)g_touch_dev.y) + g_touch_dev.yoff;

            /* 根据屏幕方向做坐标旋转/镜像 */
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
