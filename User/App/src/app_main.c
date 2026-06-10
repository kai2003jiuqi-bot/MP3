/**
 * @file    app_main.c
 * @brief   系统主入口
 *
 * app_main() 在系统初始化后被调用, 负责:
 *   1. 创建所有 RTOS 同步原语 (队列/信号量/互斥锁)
 *   2. 创建所有应用任务
 *   3. 启动 FreeRTOS 调度器
 *
 * 任务优先级 (数值越大优先级越高):
 *   task_key            : 优先级 11  (最高, 保证按键响应)
 *   task_lvgl_handler   : 优先级 10
 *   task_uart_rx        : 优先级 10
 *   task_play_music     : 优先级 10  (栈空间最大: 256*11 words)
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

/*
 * g_spi1_mutex 保护 SPI1 总线, 因为以下外设共享 SPI1:
 *   - VS1003 (音频解码器)
 *   - ILI9341 (LCD 显示屏, 含 XPT2046 触摸控制器)
 *   - W25Q64 (外部 Flash 存储器)
 *
 * 任一外设在操作 SPI 前必须获取该互斥锁, 操作完成后释放。
 * 这是保证 SPI 总线数据不混乱的关键同步机制。
 */
SemaphoreHandle_t g_spi1_mutex;

/* ==================================================================== */
/*               全局变量定义                                             */
/* ==================================================================== */

/*
 * 这里集中定义应用层全局变量, 对应 app.h 中的 extern 声明。
 * 所有 volatile 变量均可能被中断服务程序或不同任务修改,
 * 加 volatile 防止编译器优化导致读取失效。
 */

volatile uint8_t  g_volume            = 10;      /* 当前音量 (0~VOL_MAX, 越大声音越小) */
volatile uint8_t  g_volume_show       = 0;       /* 通知 UI 显示/隐藏音量条 */
volatile uint8_t  g_switch_screen_next = 0;      /* 标记向右切换屏幕 */
volatile uint8_t  g_switch_screen_prev = 0;      /* 标记向左切换屏幕 */
volatile uint8_t  g_play_mode         = PLAY_MODE_ALL;  /* 播放模式: ALL/ONE/SHUFFLE */
volatile uint32_t g_song_file_size    = 0;       /* 当前歌曲文件字节数 */
volatile uint16_t g_song_bitrate      = 0;       /* 当前歌曲码率 (kbps) */
volatile uint32_t g_song_audio_start  = 0;       /* 音频数据在文件中的起始偏移 (跳过 ID3v2) */
volatile uint8_t  g_reset_progress    = 0;       /* 通知 UI 进度条复位 */
volatile uint32_t g_song_decode_base  = 0;       /* VS1003 解码时间基值 (用于计算播放进度) */
volatile uint32_t g_song_seek_target  = 0;       /* SEEK 目标时间 (秒) */
volatile uint8_t  g_seek_active       = 0;       /* SEEK 操作进行中标记 */
volatile uint8_t  g_usb_connected     = 0;       /* USB 连接状态 */

/* ==================================================================== */
/*               应用主函数                                               */
/* ==================================================================== */

/*
 * 函数: app_main
 * 功能: 系统初始化后调用, 创建所有 RTOS 对象和任务, 启动调度器
 *
 * 创建顺序说明:
 *   1. 先创建同步原语 (队列/信号量/互斥锁), 再创建任务
 *      因为任务启动后可能立即使用这些原语
 *   2. task_uart_tx 为"写"方向, 使用 DMA + 信号量方式,
 *      初始化时先 give 信号量表示 DMA 空闲
 *
 * 栈空间分配:
 *   task_play_music 栈最大 (256*11 words ≈ 11KB),
 *   因为它需要维护文件 I/O 缓冲和状态机
 *   task_usb_device 栈最小 (128 words), 仅做状态轮询
 */

void app_main(void)
{
    /* === 1. 创建串口接收流缓冲 (长度 10 字节) === */
    g_stream_uart_rx = xStreamBufferCreate(10, 1);

    /* === 2. 创建播放控制命令队列 (深度 1, 每次覆盖写入) === */
    g_queue_play_music = xQueueCreate(1, sizeof(STATE_PLAYMUSIC));

    /* === 3. 创建 SPI1 互斥锁 === */
    g_spi1_mutex = xSemaphoreCreateMutex();

    /* === 4. 创建串口 DMA 发送信号量 === */
    g_sem_uart_tx = xSemaphoreCreateBinary();
    xSemaphoreGive(g_sem_uart_tx);   /* 初始状态: DMA 空闲 */

    /* === 5. 创建应用任务 (按优先级/重要性排序) === */

    /* LVGL GUI 任务 (优先级 10, 栈 768 words) */
    xTaskCreate(task_lvgl_handler, "task_lvgl", 256 * 3, NULL, 10, &g_task_lvgl_handler);

    /* 串口接收任务 (优先级 10, 栈 256 words) */
    xTaskCreate(task_uart_rx, "task_uartRX", 256, NULL, 10, &g_task_uart_rx);

    /* 音乐播放任务 (优先级 10, 栈 2816 words — 最大任务) */
    xTaskCreate(task_play_music, "task_playMusic", 256 * 11, NULL, 10, &g_task_play_music);

    /* 按键扫描任务 (优先级 11 — 最高, 栈 512 words) */
    xTaskCreate(task_key, "task_key", 256 * 2, NULL, 11, &g_task_key);

    /* USB 状态检测任务 (优先级 10, 栈 128 words — 最小任务) */
    xTaskCreate(task_usb_device, "task_usbDevice", 128, NULL, 10, &g_task_usb_device);

    /* === 6. 输出启动信息 === */
    printf("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\nin main\r\n");
    printf("bootCount: %lu\r\n", BOOTCOUNT_Get());  /* 显示开机次数 */

    /* === 7. 启动 FreeRTOS 调度器 (永不返回) === */
    vTaskStartScheduler();

    /* 正常情况下不会执行到这里 */
    while (1) { }
}
