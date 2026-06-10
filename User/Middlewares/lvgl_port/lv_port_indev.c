/**
 * @file lv_port_indev.c
 * @brief LVGL 触摸输入接口移植实现 - 基于 XPT2046 触摸控制器
 *
 * ========== 实现功能 ==========
 *
 * 实现 LVGL 所需的三个输入设备回调函数:
 *   1. touchpad_is_pressed() - 检测触摸屏当前是否被按下
 *      通过 XPT2046 的 IRQ 引脚电平 + SPI 坐标读取来判断
 *   2. touchpad_get_xy()     - 获取当前触摸点的 X/Y 坐标 (像素坐标)
 *      从全局变量 g_touch_dev 中读取 (由 ILI9341_TouchScan 更新)
 *   3. touchpad_read()       - LVGL 轮询调用的主回调函数
 *      LVGL 在每个 LV_TICK 周期内调用此函数获取输入状态
 *
 * ========== 使用方法 ==========
 *   在 main 中调用 lv_port_indev_init() 即可
 */
#include "lv_port_indev.h"
#include "ili9341_touch.h"

static lv_indev_t *s_indev_touchpad;

/**
 * @brief 检测触摸屏是否被按下
 *
 * 调用 ILI9341_TouchScan(0) 扫描触摸,
 * 返回非 0 表示触摸中。
 *
 * @return true  触摸按下
 * @return false 未触摸
 */
static bool touchpad_is_pressed(void)
{
    return (ILI9341_TouchScan(0) != 0);
}

/**
 * @brief 获取当前触摸点的像素坐标
 *
 * 从 g_touch_dev 结构体中读取经过矫正的 X/Y 坐标,
 * 坐标值已在 ILI9341_TouchScan() 中转换为屏幕像素坐标。
 *
 * @param x 输出: X 坐标 (0~239)
 * @param y 输出: Y 坐标 (0~319)
 */
static void touchpad_get_xy(lv_coord_t *x, lv_coord_t *y)
{
    *x = (lv_coord_t)g_touch_dev.x;
    *y = (lv_coord_t)g_touch_dev.y;
}

/**
 * @brief LVGL 触摸输入读取回调 (由 LVGL 内核按周期调用)
 *
 * 回调流程:
 *   1. 检测触摸是否按下
 *   2. 按下时获取坐标, 设置 state = LV_INDEV_STATE_PR (按下)
 *   3. 未按时保持上次的坐标, 设置 state = LV_INDEV_STATE_REL (释放)
 *
 * 使用 static 变量 last_x/last_y 记忆上次触摸坐标,
 * 这样释放时返回的坐标为最后一次触摸的位置,
 * 用于 LVGL 的点击事件处理 (需要知道点击发生在哪个对象上)。
 *
 * @param indev_drv  输入设备驱动对象 (本实现未使用)
 * @param data       输出: LVGL 输入数据结构体 (包含坐标和状态)
 */
static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    (void)indev_drv;

    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    if (touchpad_is_pressed())
    {
        /* 触摸按下: 获取当前坐标 */
        touchpad_get_xy(&last_x, &last_y);
        data->state = LV_INDEV_STATE_PR;  /* 按下状态 */
    }
    else
    {
        /* 触摸释放: 返回最后一次按下的坐标 */
        data->state = LV_INDEV_STATE_REL; /* 释放状态 */
    }

    data->point.x = last_x;
    data->point.y = last_y;
}

void lv_port_indev_init(void)
{
    static lv_indev_drv_t indev_drv;

    /* 触摸屏底层硬件初始化 (XPT2046) */
    ILI9341_TouchInit();

    /* 注册触摸屏输入设备到 LVGL */
    lv_indev_drv_init(&indev_drv);
    indev_drv.type     = LV_INDEV_TYPE_POINTER;  /* 指针类型 (触摸屏) */
    indev_drv.read_cb  = touchpad_read;           /* 绑定读取回调 */

    /* 注册驱动, 返回设备句柄 (当前未使用) */
    s_indev_touchpad   = lv_indev_drv_register(&indev_drv);
}
