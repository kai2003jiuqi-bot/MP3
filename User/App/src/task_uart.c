/**
 * @file    task_uart.c
 * @brief   串口调试命令处理任务
 *
 * 本模块实现了基于 UART 的交互式调试接口, 使用 "AT" 命令格式。
 * 工作流程:
 *   1. UART RX 中断逐字节接收数据, 写入 g_stream_uart_rx 流缓冲
 *   2. task_uart_rx 从流缓冲读取字节, 送入 RX 状态机解析
 *   3. 状态机以空格 ' ' 为起始标志、回车 '\r' 为结束标志,
 *      提取完整命令后调用 s_UART_RxHandler() 回调执行
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stream_buffer.h"
#include "rx_stateMachine.h"
#include "usart.h"
#include "app.h"
#include "file.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdarg.h>

/** @brief 串口接收缓冲区最大长度 */
#define UART_RX_BUF_SIZE    10U
/** @brief DMA 发送超时时间 (毫秒) */
#define UART_TX_OVERTIME_MS 100U

/** @brief 串口接收状态机实例 */
static RX_StateMachine_t s_uart_rx_machine;

/** @brief 串口接收缓冲区: 状态机解析后存放完整命令字符串 */
static uint8_t s_uart_rx_buf[UART_RX_BUF_SIZE];

/* ==================================================================== */
/*               全局变量定义                                             */
/* ==================================================================== */

StreamBufferHandle_t g_stream_uart_rx;
StreamBufferHandle_t g_stream_uart_tx;
TaskHandle_t         g_task_uart_rx;
TaskHandle_t         g_task_uart_tx;
SemaphoreHandle_t    g_sem_uart_tx;

/* ==================================================================== */
/*               内部函数声明                                             */
/* ==================================================================== */

static void s_UART_RxHandler(void);

/* ==================================================================== */
/*               串口命令回调                                            */
/* ==================================================================== */

static void s_UART_RxHandler(void)
{
    /* ---- AT: 通信测试 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "AT") == 0)
    {
        printf("OK\r\n");
        return;
    }

    /* ---- stack: 查看各任务剩余栈空间 (words / KB) ---- */
    if (strcmp((const char *)s_uart_rx_buf, "stack") == 0)
    {
        printf("stack (words / KB):\r\n");
        printf("  task_lvgl_handler: %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_lvgl_handler),
               (int)(uxTaskGetStackHighWaterMark(g_task_lvgl_handler) * 4 / 1024));
        printf("  task_uartRX:       %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_uart_rx),
               (int)(uxTaskGetStackHighWaterMark(g_task_uart_rx) * 4 / 1024));
        printf("  task_uartTX:       %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_uart_tx),
               (int)(uxTaskGetStackHighWaterMark(g_task_uart_tx) * 4 / 1024));
        printf("  task_playMusic:    %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_play_music),
               (int)(uxTaskGetStackHighWaterMark(g_task_play_music) * 4 / 1024));
        printf("  task_key:          %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_key),
               (int)(uxTaskGetStackHighWaterMark(g_task_key) * 4 / 1024));
        printf("  task_usbDevice:    %4d / %d KB\r\n",
               (int)uxTaskGetStackHighWaterMark(g_task_usb_device),
               (int)(uxTaskGetStackHighWaterMark(g_task_usb_device) * 4 / 1024));
        return;
    }

    /* ---- currentMusic: 查看当前播放歌曲 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "currentMusic") == 0)
    {
        if (g_file_current_music != NULL)
        {
            printf("current music: %s\r\n", g_file_current_music->utf8_name);
        }
        else
        {
            printf("current music: (none)\r\n");
        }
        return;
    }

    /* ---- key: 查看按键状态 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "key") == 0)
    {
        printf("incVolume key: %d\r\n", g_key_inc_volume.valid_flag);
        printf("decVolume key: %d\r\n", g_key_dec_volume.valid_flag);
        return;
    }

    /* ---- heap: FreeRTOS 堆内存 (KB) ---- */
    if (strcmp((const char *)s_uart_rx_buf, "heap") == 0)
    {
        printf("FreeRTOS heap:\r\n");
        printf("  total:    %d KB\r\n",
               (int)(configTOTAL_HEAP_SIZE / 1024));
        printf("  free:     %d KB\r\n",
               (int)(xPortGetFreeHeapSize() / 1024));
        printf("  used:     %d KB (%d%%)\r\n",
               (int)((configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize()) / 1024),
               (int)((configTOTAL_HEAP_SIZE - xPortGetFreeHeapSize()) * 100
                     / configTOTAL_HEAP_SIZE));
        printf("  min_free: %d KB\r\n",
               (int)(xPortGetMinimumEverFreeHeapSize() / 1024));
        return;
    }

    /* ---- free: LVGL 堆内存 (KB) ---- */
    if (strcmp((const char *)s_uart_rx_buf, "free") == 0)
    {
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        printf("LVGL memory:\r\n");
        printf("  total:    %d KB\r\n", (int)(mon.total_size / 1024));
        printf("  free:     %d KB\r\n", (int)(mon.free_size / 1024));
        printf("  used:     %d%%\r\n", (int)mon.used_pct);
        printf("  frag:     %d%%\r\n", (int)mon.frag_pct);
        printf("  max_used: %d KB\r\n", (int)(mon.max_used / 1024));
        return;
    }

    /* ---- play ---- */
    if (strcmp((const char *)s_uart_rx_buf, "play") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_PLAY;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("play music\r\n");
        return;
    }

    /* ---- pause ---- */
    if (strcmp((const char *)s_uart_rx_buf, "pause") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("pause music\r\n");
        return;
    }

    /* ---- next ---- */
    if (strcmp((const char *)s_uart_rx_buf, "next") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_NEXT;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("next music\r\n");
        return;
    }

    /* ---- prev ---- */
    if (strcmp((const char *)s_uart_rx_buf, "prev") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_PREV;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("prev music\r\n");
        return;
    }
}

/* ==================================================================== */
/*               printf 底层输出                                         */
/* ==================================================================== */

int fputc(int ch, FILE *f)
{
    (void)f;
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFFU);
    return ch;
}

/* ==================================================================== */
/*               中断回调                                                */
/* ==================================================================== */

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart1)
    {
        xStreamBufferSendFromISR(g_stream_uart_rx,
                                 &s_uart_rx_machine.byte, 1, NULL);
        HAL_UART_Receive_IT(&huart1, &s_uart_rx_machine.byte, 1);
    }
}

/* ==================================================================== */
/*               串口接收任务                                            */
/* ==================================================================== */

void task_uart_rx(void *p)
{
    (void)p;

    RX_Init(&s_uart_rx_machine,
            ' ',
            '\r',
            g_stream_uart_rx,
            UART_RX_BUF_SIZE,
            s_uart_rx_buf,
            s_UART_RxHandler);

    HAL_UART_Receive_IT(&huart1, &s_uart_rx_machine.byte, 1);

    printf("uartRX_Task start\r\n");

    while (1)
    {
        RX_Run(&s_uart_rx_machine);
    }
}