/**
 * @file lv_port_indev.h
 * @brief LVGL 触摸输入接口移植头文件
 *
 * 在初始化阶段调用 lv_port_indev_init() 即可完成触摸输入设备的注册。
 */
#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/**
 * @brief 初始化 LVGL 触摸输入设备
 */
void lv_port_indev_init(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_INDEV_H */
