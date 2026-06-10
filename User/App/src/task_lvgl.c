/**
 * @file    task_lvgl.c
 * @brief   LVGL 图形刷新任务
 *
 * 本任务负责驱动 LVGL 图形库, 包含两部分工作:
 *   1. lv_tick_inc(5)  —— 通知 LVGL 内核经过 5ms
 *   2. lv_task_handler() —— 执行 LVGL 的任务处理
 *   3. vTaskDelay(5)    —— 让出 CPU
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

    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    printf("lvgl init finished\r\n");

    UI_Init();
    printf("GUI created\r\n");

    while (1)
    {
        lv_task_handler();
        lv_tick_inc(5);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
