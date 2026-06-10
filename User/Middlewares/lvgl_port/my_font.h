/**
 * @file my_font.h
 * @brief LVGL 自定义字体声明头文件
 *
 * 声明由 LVGL 字体工具预编译生成的自定义字体, 供 UI 模块引用。
 * 字体数据存储在 my_font.c 中, 以 lv_font_t 结构体形式导出。
 */
#ifndef MY_FONT_H
#define MY_FONT_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 当前使用的自定义字体(24px 仿宋, 1bpp, 含中文)
extern lv_font_t my_font;

#ifdef __cplusplus
} // extern "C"
#endif

#endif
