/**
 * @file lv_port_indev.c
 * @brief LVGL 触摸输入接口移植实现 - 基于 XPT2046 触摸控制器
 *
 * 实现 LVGL 所需的三个输入设备回调函数:
 *   1. touchpad_is_pressed() - 检测触摸屏当前是否被按下
 *   2. touchpad_get_xy()     - 获取当前触摸点的 X/Y 坐标
 *   3. touchpad_read()       - LVGL 轮询调用的主回调函数
 *
 * 使用方法:
 *   在 main 中调用 lv_port_indev_init() 即可
 */
#include "lv_port_indev.h"
#include "ili9341_touch.h"

static lv_indev_t *s_indev_touchpad;

static bool touchpad_is_pressed(void)
{
    return (ILI9341_TouchScan(0) != 0);
}

static void touchpad_get_xy(lv_coord_t *x, lv_coord_t *y)
{
    *x = (lv_coord_t)g_touch_dev.x;
    *y = (lv_coord_t)g_touch_dev.y;
}

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    if (touchpad_is_pressed())
    {
        touchpad_get_xy(&last_x, &last_y);
        data->state = LV_INDEV_STATE_PR;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }

    data->point.x = last_x;
    data->point.y = last_y;
}

void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;

    /* 触摸屏底层硬件初始化 */
    ILI9341_TouchInit();

    /* 注册触摸屏输入设备 */
    lv_indev_drv_init(&indev_drv);
    indev_drv.type     = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb  = touchpad_read;
    s_indev_touchpad   = lv_indev_drv_register(&indev_drv);
}
