/**
 * @file lv_port_disp.c
 * @brief LVGL 显示接口移植实现 - 基于 ILI9341 驱动 + 单行缓冲刷新
 *
 * ========== 显示缓冲策略 ==========
 *
 * 本实现使用 **单行缓冲模式**, 即只有一个缓冲区:
 *   缓冲区大小 = ILI9341_WIDTH x LV_PORT_DISP_BUF_LINES x 2(RGB565)
 *
 * 当 LVGL 需要刷新屏幕时:
 *   1. LVGL 将一个或多个脏矩形区域渲染到 s_disp_buf 中
 *   2. 渲染完成后触发 disp_flush() 回调
 *   3. disp_flush() 通过 ILI9341 SPI 接口将缓冲区内容推送到屏幕
 *   4. 通知 LVGL 刷新完成 (lv_disp_flush_ready)
 *
 * 单缓冲 vs 双缓冲:
 *   - 单缓冲: 内存占用小, 但渲染和刷新不能同时进行
 *   - 双缓冲: 可以流水线作业, 但需要双倍内存
 *   在本项目中 (240×320 分辨率), 单行缓冲约 480 字节,
 *   内存压力不大, 但考虑到 MCU 的 SRAM 有限, 选择单缓冲。
 *
 * ========== 使用方法 ==========
 *   1. 调用 lv_port_disp_init() 初始化
 *   2. 之后便可使用 LVGL 的 API 创建 UI
 */
#include "lv_port_disp.h"

/*
 * 显示缓冲区:
 *   数据类型: lv_color_t (STM32 上为 uint16_t, RGB565)
 *   大小: ILI9341_WIDTH (240) × LV_PORT_DISP_BUF_LINES 行
 *   第二缓冲参数传 NULL, 表示单缓冲模式
 */
static lv_color_t s_disp_buf[ILI9341_WIDTH * LV_PORT_DISP_BUF_LINES];
static lv_disp_draw_buf_t s_draw_buf;

/**
 * @brief 显示刷新回调函数 (由 LVGL 在需要刷新脏区域时调用)
 *
 * 工作流程:
 *   1. 计算需要刷新的像素区域尺寸
 *   2. 调用 ILI9341_SetRegion() 设置屏幕写入窗口
 *   3. 调用 ILI9341_WritePixels() 批量写入像素数据
 *   4. 调用 lv_disp_flush_ready() 通知 LVGL 刷新完成
 *
 * @param disp_drv  LVGL 显示驱动对象
 * @param area      需要刷新的屏幕区域 (矩形)
 * @param color_p   像素数据缓冲区指针
 */
static void disp_flush(lv_disp_drv_t *disp_drv,
                       const lv_area_t *area,
                       lv_color_t *color_p)
{
    uint32_t refresh_w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t refresh_h = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t total_pixels = refresh_w * refresh_h;

    /* 1. 设置屏幕刷新区域 (GRAM 写入窗口) */
    ILI9341_SetRegion((uint16_t)area->x1, (uint16_t)area->y1,
                      (uint16_t)area->x2, (uint16_t)area->y2);

    /* 2. 批量写入像素数据 */
    ILI9341_WritePixels((const uint16_t *)&(color_p->full), total_pixels);

    /* 3. 通知 LVGL 刷新完成 */
    lv_disp_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* 1. 初始化显示缓冲区 (单缓冲模式) */
    lv_disp_draw_buf_init(&s_draw_buf,
                          s_disp_buf,
                          NULL,   /* 第二缓冲 = NULL, 单缓冲模式 */
                          ILI9341_WIDTH * LV_PORT_DISP_BUF_LINES);

    /* 2. 初始化 LVGL 显示驱动结构体 */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    /* 设置分辨率: 240 × 320 */
    disp_drv.hor_res   = ILI9341_WIDTH;
    disp_drv.ver_res   = ILI9341_HEIGHT;

    /* 绑定刷新回调函数 */
    disp_drv.flush_cb  = disp_flush;

    /* 绑定显示缓冲区 */
    disp_drv.draw_buf  = &s_draw_buf;

    /* 3. 初始化底层硬件 (ILI9341 屏幕) */
    ILI9341_Init();

    /* 4. 注册驱动到 LVGL, 此后 LVGL 的 UI 创建将使用此驱动进行显示输出 */
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
