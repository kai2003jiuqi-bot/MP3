/**
 * @file    app_main.c
 * @brief   系统主入口
 *
 * app_main() 在系统初始化后被调用, 负责:
 *   1. 创建所有 RTOS 同步原语
 *   2. 创建所有应用任务
 *   3. 启动 FreeRTOS 调度器
 *
 * 任务优先级 (数值越大优先级越高):
 *   task_key            : 优先级 11
 *   task_lvgl_handler   : 优先级 10
 *   task_uart_rx        : 优先级 10
 *   task_play_music     : 优先级 10
 *   task_usb_device     : 优先级 10
 */

#include "app.h"
#include "ili9341_touch.h"
#include "ili9341_display.h"
#include "vs1003.h"
#include "bootCount.h"
#include <stdio.h>

/* ==================================================================== */
/*               互斥锁: 保护 SPI1 总线免于多任务竞争                     */
/* ==================================================================== */

SemaphoreHandle_t g_spi1_mutex;

/* ==================================================================== */
/*               全局变量定义                                             */
/* ==================================================================== */

volatile uint8_t  g_volume            = 10;
volatile uint8_t  g_volume_show       = 0;
volatile uint8_t  g_switch_screen_next = 0;
volatile uint8_t  g_switch_screen_prev = 0;
volatile uint8_t  g_play_mode         = PLAY_MODE_ALL;
volatile uint32_t g_song_file_size    = 0;
volatile uint16_t g_song_bitrate      = 0;
volatile uint32_t g_song_audio_start  = 0;
volatile uint8_t  g_reset_progress    = 0;
volatile uint32_t g_song_decode_base  = 0;
volatile uint32_t g_song_seek_target  = 0;
volatile uint8_t  g_seek_active       = 0;
volatile uint8_t  g_usb_connected     = 0;

/* ==================================================================== */
/*               应用主函数                                               */
/* ==================================================================== */

void app_main(void)
{
    /* 创建串口接收流缓冲 */
    g_stream_uart_rx = xStreamBufferCreate(10, 1);

    /* 创建播放控制命令队列 */
    g_queue_play_music = xQueueCreate(1, sizeof(STATE_PLAYMUSIC));

    /* 创建 SPI1 互斥锁 */
    g_spi1_mutex = xSemaphoreCreateMutex();

    /* 创建串口 DMA 发送信号量 */
    g_sem_uart_tx = xSemaphoreCreateBinary();
    xSemaphoreGive(g_sem_uart_tx);

    /* 创建 LVGL GUI 任务 */
    xTaskCreate(task_lvgl_handler, "task_lvgl", 256 * 3, NULL, 10, &g_task_lvgl_handler);

    /* 创建串口接收任务 */
    xTaskCreate(task_uart_rx, "task_uartRX", 256, NULL, 10, &g_task_uart_rx);

    /* 创建音乐播放任务 */
    xTaskCreate(task_play_music, "task_playMusic", 256 * 11, NULL, 10, &g_task_play_music);

    /* 创建按键扫描任务 */
    xTaskCreate(task_key, "task_key", 256 * 2, NULL, 11, &g_task_key);

    /* 创建 USB 状态检测任务 */
    xTaskCreate(task_usb_device, "task_usbDevice", 128, NULL, 10, &g_task_usb_device);

    /* 输出启动信息 */
    printf("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\nin main\r\n");
    printf("bootCount: %lu\r\n", BOOTCOUNT_Get());

    /* 启动调度器 (永不返回) */
    vTaskStartScheduler();

    while (1) { }
}
