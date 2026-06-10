/**
 * @file lv_port_disp.c
 * @brief LVGL 显示接口移植实现 - 基于 ILI9341 驱动 + 单行缓冲刷新
 *
 * 显示缓冲策略:
 *   使用单行缓冲模式, 缓冲区大小 = ILI9341_WIDTH x LV_PORT_DISP_BUF_LINES x 2(RGB565)
 *   该缓冲通过 lv_disp_draw_buf_init() 注册, 第二缓冲参数传 NULL 表示单缓冲模式。
 *
 * 使用方法:
 *   1. 调用 lv_port_disp_init() 初始化
 *   2. 之后便可使用 LVGL 的 API 创建 UI
 */
#include "lv_port_disp.h"

static lv_color_t s_disp_buf[ILI9341_WIDTH * LV_PORT_DISP_BUF_LINES];
static lv_disp_draw_buf_t s_draw_buf;

/**
 * @brief 显示刷新回调函数 (由 LVGL 在需要刷新脏区域时调用)
 */
static void disp_flush(lv_disp_drv_t *disp_drv,
                       const lv_area_t *area,
                       lv_color_t *color_p)
{
    uint32_t refresh_w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t refresh_h = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t total_pixels = refresh_w * refresh_h;

    /* 1. 设置屏幕刷新区域 */
    ILI9341_SetRegion((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2);

    /* 2. 批量写入像素数据 */
    ILI9341_WritePixels((const uint16_t *)&(color_p->full), total_pixels);

    /* 3. 通知 LVGL 刷新完成 */
    lv_disp_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* 1. 初始化单缓冲 */
    lv_disp_draw_buf_init(&s_draw_buf,
                          s_disp_buf,
                          NULL,
                          ILI9341_WIDTH * LV_PORT_DISP_BUF_LINES);

    /* 2. 初始化 LVGL 显示驱动结构体 */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    disp_drv.hor_res   = ILI9341_WIDTH;
    disp_drv.ver_res   = ILI9341_HEIGHT;
    disp_drv.flush_cb  = disp_flush;
    disp_drv.draw_buf  = &s_draw_buf;

    /* 3. 初始化底层硬件 (ILI9341 屏幕) */
    ILI9341_Init();

    /* 4. 注册驱动到 LVGL */
    lv_disp_drv_register(&disp_drv);
}

void disp_enable_update(void)
{
    /* 当前无实现: 默认始终使能 */
}

void disp_disable_update(void)
{
    /* 当前无实现 */
}
