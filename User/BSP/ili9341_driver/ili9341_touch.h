/**
 * @file    ili9341_touch.h
 * @brief   XPT2046 触摸控制器驱动头文件, 通过 SPI 读取触摸坐标
 * @details 本驱动使用 SPI 接口与 XPT2046 触摸控制器通信,
 *          通过读取触摸屏上的电压值, 计算触摸点的 X/Y 坐标。
 *
 * @note    ILI9341_TOUCH_DIRECTION 必须与 ili9341_display.h 中的
 *          ILI9341_DISPLAY_ORIENTATION 保持一致, 否则触摸点与显示位置不匹配。
 */
#ifndef ILI9341_TOUCH_H
#define ILI9341_TOUCH_H

#include <stdint.h>
#include "main.h"
#include "spi.h"

/**
 * @brief 触摸屏幕方向设置
 * @note 必须与 ILI9341_DISPLAY_ORIENTATION 保持一致
 */
#define ILI9341_TOUCH_DIRECTION 1

/* ==================================================================== */
/*                   XPT2046 命令定义                                    */
/* ==================================================================== */

#define XPT2046_CMD_RDX          0xD0U   /**< 读 X 坐标差分模式 */
#define XPT2046_CMD_RDY          0x90U   /**< 读 Y 坐标差分模式 */
#define XPT2046_IRQ_PRESSED      0x00U   /**< IRQ 低电平表示触摸按下 */

/**
 * @brief 触摸设备结构体
 */
typedef struct {
    uint16_t x;     /**< 当前读取到的 X 轴屏幕坐标 */
    uint16_t y;     /**< 当前读取到的 Y 轴屏幕坐标 */
    float    xfac;  /**< X 轴矫正斜率 */
    float    yfac;  /**< Y 轴矫正斜率 */
    int16_t  xoff;  /**< X 轴矫正截距 */
    int16_t  yoff;  /**< Y 轴矫正截距 */
} ILI9341_Touch_t;

/** 触摸设备全局实例 */
extern ILI9341_Touch_t g_touch_dev;

/**
 * @brief   初始化触摸屏
 */
void ILI9341_TouchInit(void);

/**
 * @brief   扫描触摸屏, 获取坐标
 * @param   raw  0=使用矫正数据 (转换为屏幕坐标); 1=返回原始 ADC 值
 * @return  0=未按下, 1=已按下
 */
uint8_t ILI9341_TouchScan(uint8_t raw);

#endif /* ILI9341_TOUCH_H */
