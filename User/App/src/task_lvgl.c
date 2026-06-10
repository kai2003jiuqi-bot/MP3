/**
 * @file    task_lvgl.c
 * @brief   LVGL 图形刷新任务
 *
 * 本任务负责驱动 LVGL 图形库, 包含两部分工作:
 *   1. lv_tick_inc(5)  —— 通知 LVGL 内核经过 5ms (用于动画/超时计时)
 *   2. lv_task_handler() —— 执行 LVGL 的内部任务处理 (重绘脏区域、运行动画等)
 *   3. vTaskDelay(5)    —— 让出 CPU, 保持 5ms 的刷新周期
 *
 * 注意事项:
 *   - 刷新周期 5ms 意味着理论最大帧率约 200fps
 *   - 实际帧率受显示刷新速度限制 (ILI9341 SPI 刷新约 5~15ms/帧)
 *   - lv_tick_inc() 必须在固定时间间隔调用, 以保证 LVGL 定时器准确性
 */

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include <stdio.h>
#include "ui.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"

/** @brief LVGL 任务句柄 */
TaskHandle_t g_task_lvgl_handler;

void task_lvgl_handler(void *p)
{
    (void)p;

    /* 1. LVGL 内核初始化 */
    lv_init();

    /* 2. 初始化显示接口驱动 (ILI9341 + 单行刷新缓冲) */
    lv_port_disp_init();

    /* 3. 初始化触摸输入接口驱动 (XPT2046) */
    lv_port_indev_init();
    printf("lvgl init finished\r\n");

    /* 4. 创建 LVGL UI 界面 (三个主屏: player/playlist/library) */
    UI_Init();
    printf("GUI created\r\n");

    /*
     * 5. 主循环: 以 5ms 周期驱动 LVGL 内核
     *
     * 工作流程:
     *   a. lv_task_handler()   — 处理 LVGL 内部的定时器、动画、重绘请求
     *   b. lv_tick_inc(5)      — 通知 LVGL 内核过去了 5ms
     *   c. vTaskDelay(5)       — 等待 5ms, 让出 CPU 给其他任务
     */
    while (1)
    {
        lv_task_handler();
        lv_tick_inc(5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
