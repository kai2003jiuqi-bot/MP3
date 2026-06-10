/**
 * @file    ili9341_display.h
 * @brief   ILI9341 LCD 驱动头文件, 通过 SPI 接口控制 240x320 TFT 屏幕
 * @details 本驱动通过 STM32F407VET6 的 SPI1 接口与 ILI9341 通信,
 *          支持 4 种屏幕方向切换, 16位 RGB565 色彩格式。
 *
 *          引脚定义:
 *          - LCD_CS:   SPI 片选 (低电平有效)
 *          - LCD_DC:   数据/命令选择 (0=命令, 1=数据)
 *          - LCD_RST:  硬件复位 (低电平复位)
 *          - LCD_BL:   背光控制 (PWM)
 *
 *          屏幕方向:
 *          ILI9341_DISPLAY_ORIENTATION 宏控制屏幕方向, 可选 0~3:
 *          0 = 正竖屏 (Portrait), 1 = 反竖屏 (Portrait Reverse)
 *          2 = 正横屏 (Landscape), 3 = 反横屏 (Landscape Reverse)
 *
 * @note    触摸方向宏 (ILI9341_TOUCH_DIRECTION) 必须与此保持一致
 */
#ifndef ILI9341_DISPLAY_H
#define ILI9341_DISPLAY_H

#include <stdint.h>
#include "main.h"
#include "spi.h"

/* ==================================================================== */
/*                   屏幕方向配置                                         */
/* ==================================================================== */

#define ILI9341_DISPLAY_ORIENTATION 1   /* 0: 正竖屏 1: 反竖屏 2: 正横屏 3: 反横屏 */

#if (ILI9341_DISPLAY_ORIENTATION == 0) || (ILI9341_DISPLAY_ORIENTATION == 1)
    #define ILI9341_WIDTH   240U
    #define ILI9341_HEIGHT  320U
#else
    #define ILI9341_WIDTH   320U
    #define ILI9341_HEIGHT  240U
#endif

/* ==================================================================== */
/*                   颜色宏定义 (RGB565)                                  */
/* ==================================================================== */

#define ILI9341_COLOR_BLACK    0x0000U
#define ILI9341_COLOR_WHITE    0xFFFFU
#define ILI9341_COLOR_RED      0xF800U
#define ILI9341_COLOR_GREEN    0x07E0U
#define ILI9341_COLOR_BLUE     0x001FU
#define ILI9341_COLOR_YELLOW   0xFFE0U
#define ILI9341_COLOR_CYAN     0x07FFU
#define ILI9341_COLOR_MAGENTA  0xF81FU

/* ==================================================================== */
/*                   命令宏定义                                           */
/* ==================================================================== */

#define ILI9341_CMD_SLPOUT     0x11U
#define ILI9341_CMD_SWRESET    0x01U
#define ILI9341_CMD_CASET      0x2AU
#define ILI9341_CMD_PASET      0x2BU
#define ILI9341_CMD_RAMWR      0x2CU
#define ILI9341_CMD_MADCTL     0x36U
#define ILI9341_CMD_PIXFMT     0x3AU
#define ILI9341_CMD_DISPON     0x29U

/* MADCTL 方向值 */
#define ILI9341_MADCTL_PORTRAIT_NORMAL   0x88U
#define ILI9341_MADCTL_PORTRAIT_REV      0x48U
#define ILI9341_MADCTL_LANDSCAPE_NORMAL  0xE8U
#define ILI9341_MADCTL_LANDSCAPE_REV     0x28U

/* ==================================================================== */
/*                   函数声明                                             */
/* ==================================================================== */

/**
 * @brief   初始化 ILI9341 显示屏
 */
void ILI9341_Init(void);

/**
 * @brief   硬件复位 ILI9341
 */
void ILI9341_HardwareReset(void);

/**
 * @brief   在指定坐标绘制一个像素点
 * @param   x      X 坐标
 * @param   y      Y 坐标
 * @param   color  16位 RGB565 颜色值
 */
void ILI9341_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

/**
 * @brief   用指定颜色清空全屏
 * @param   color  16位 RGB565 颜色值
 */
void ILI9341_Clear(uint16_t color);

/**
 * @brief   设置矩形显示区域 (用于批量填充)
 * @param   x_start, y_start, x_end, y_end  矩形区域边界
 */
void ILI9341_SetRegion(uint16_t x_start, uint16_t y_start,
                       uint16_t x_end,   uint16_t y_end);

/**
 * @brief   批量写入 16位 像素数据
 * @param   data  像素数据数组
 * @param   len   像素数量
 */
void ILI9341_WritePixels(const uint16_t *data, uint32_t len);

/**
 * @brief   写命令索引
 * @param   cmd  命令字节
 */
void ILI9341_WriteCmd(uint8_t cmd);

/**
 * @brief   写 8位 数据
 * @param   data  数据字节
 */
void ILI9341_WriteData8(uint8_t data);

#endif /* ILI9341_DISPLAY_H */
