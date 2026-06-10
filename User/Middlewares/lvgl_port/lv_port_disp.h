/**
 * @file lv_port_disp.h
 * @brief LVGL 显示接口移植头文件 - 屏幕方向与缓冲大小配置
 */
#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "ili9341_display.h"
#include <string.h>

/** @brief 显示缓冲行数 (影响内存占用和刷新效率) */
#define LV_PORT_DISP_BUF_LINES  30U

/**
 * @brief 初始化 LVGL 显示接口
 */
void lv_port_disp_init(void);

/**
 * @brief 使能显示刷新 (默认已使能)
 */
void disp_enable_update(void);

/**
 * @brief 禁止显示刷新
 */
void disp_disable_update(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISP_H */
