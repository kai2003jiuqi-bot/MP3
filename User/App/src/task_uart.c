/**
 * @file    task_uart.c
 * @brief   串口调试命令处理任务
 *
 * 本模块实现了基于 UART 的交互式调试接口, 使用 "AT" 命令格式。
 *
 * 工作流程:
 *   1. UART RX 中断逐字节接收数据, 以中断方式写入 g_stream_uart_rx 流缓冲
 *   2. task_uart_rx 从流缓冲中读取字节, 送入 RX 状态机解析
 *   3. 状态机以空格 ' ' 为帧头 (起始), 回车 '\r' 为帧尾 (结束)
 *   4. 收到完整帧后调用 s_UART_RxHandler() 回调执行对应命令
 *   5. printf 输出重定向到 UART1 (通过 fputc 重写)
 *
 * 支持的命令列表:
 *   - AT:      通信测试, 返回 OK
 *   - stack:   查看各任务剩余栈空间 (调试用)
 *   - currentMusic: 查看当前播放歌曲名称
 *   - key:     查看按键状态
 *   - heap:    查看 FreeRTOS 堆内存使用情况
 *   - free:    查看 LVGL 堆内存使用情况
 *   - play:    发送播放命令
 *   - pause:   发送暂停命令
 *   - next:    发送下一首命令
 *   - prev:    发送上一首命令
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

/*
 * 函数: s_UART_RxHandler
 * 触发: RX 状态机收到完整帧 (以 '\r' 结尾) 后调用
 *
 * 命令处理方式:
 *   - 使用 strcmp() 匹配命令字符串
 *   - 匹配后执行对应操作 (查询状态 / 发送控制命令等)
 *   - 通过 printf 输出结果
 *
 * 音乐控制命令通过 xQueueOverwrite() 发送到播放任务队列:
 *   使用 Overwrite 而非 Send, 确保命令一定能覆盖进入队列
 */

static void s_UART_RxHandler(void)
{
    /* ---- AT: 通信测试, 返回 OK ---- */
    if (strcmp((const char *)s_uart_rx_buf, "AT") == 0)
    {
        printf("OK\r\n");
        return;
    }

    /*
     * ---- stack: 查看各任务剩余栈空间 (words / KB) ----
     *
     * uxTaskGetStackHighWaterMark() 返回任务运行至今的最小剩余栈空间,
     * 反映最极端情况下的栈使用量, 用于判断栈大小是否足够。
     * 单位: words (STM32 上 1 word = 4 bytes)
     */
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

    /* ---- currentMusic: 查看当前播放歌曲的 UTF-8 名称 ---- */
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

    /* ---- key: 查看两个按键当前的 valid_flag 状态 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "key") == 0)
    {
        printf("incVolume key: %d\r\n", g_key_inc_volume.valid_flag);
        printf("decVolume key: %d\r\n", g_key_dec_volume.valid_flag);
        return;
    }

    /* ---- heap: FreeRTOS 堆内存统计 (KB) ---- */
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

    /* ---- free: LVGL 堆内存统计 (KB) ---- */
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

    /* ---- play: 发送恢复播放命令 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "play") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_PLAY;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("play music\r\n");
        return;
    }

    /* ---- pause: 发送暂停命令 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "pause") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("pause music\r\n");
        return;
    }

    /* ---- next: 发送下一首命令 ---- */
    if (strcmp((const char *)s_uart_rx_buf, "next") == 0)
    {
        STATE_PLAYMUSIC cmd = PLAYMUSIC_NEXT;
        xQueueOverwrite(g_queue_play_music, &cmd);
        printf("next music\r\n");
        return;
    }

    /* ---- prev: 发送上一首命令 ---- */
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

/*
 * 函数: fputc
 * 功能: 重写 C 库 printf 的底层输出函数
 * 实现: 通过 UART1 以阻塞方式发送单个字符
 * 注意: 阻塞发送的超时时间为 0xFFFF (约 65 秒, 实际不会达到)
 */
int fputc(int ch, FILE *f)
{
    (void)f;
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFFU);
    return ch;
}

/* ==================================================================== */
/*               中断回调                                                */
/* ==================================================================== */

/*
 * 函数: HAL_UART_RxCpltCallback
 * 功能: UART1 接收中断完成回调 (每收到一个字节触发一次)
 *
 * 工作流程:
 *   1. 将收到的字节通过 xStreamBufferSendFromISR() 写入流缓冲
 *      这样可以在 ISR 中快速退出, 实际处理在 task_uart_rx 中进行
 *   2. 重新启动 UART 中断接收, 准备接收下一个字节
 *
 * 注意: 使用 FromISR 版本的 API, 因为此函数在中断上下文中执行
 */
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

/*
 * 函数: task_uart_rx
 * 功能: FreeRTOS 串口接收任务入口
 *
 * 工作流程:
 *   1. 初始化 RX 状态机:
 *      - 帧头: ' ' (空格)
 *      - 帧尾: '\r' (回车)
 *      - 流缓冲: g_stream_uart_rx (由 UART RX ISR 写入)
 *      - 最大长度: UART_RX_BUF_SIZE
 *      - 接收缓冲: s_uart_rx_buf
 *      - 回调函数: s_UART_RxHandler
 *   2. 启动 UART1 中断接收 (逐字节模式)
 *   3. 进入主循环, 持续运行 RX 状态机
 *      RX_Run() 内部会阻塞等待流缓冲数据 (portMAX_DELAY)
 */

void task_uart_rx(void *p)
{
    (void)p;

    /* 初始化 RX 状态机: 帧头=' ', 帧尾='\r' */
    RX_Init(&s_uart_rx_machine,
            ' ',
            '\r',
            g_stream_uart_rx,
            UART_RX_BUF_SIZE,
            s_uart_rx_buf,
            s_UART_RxHandler);

    /* 启动 UART 中断接收 (单字节模式) */
    HAL_UART_Receive_IT(&huart1, &s_uart_rx_machine.byte, 1);

    printf("uartRX_Task start\r\n");

    /* 主循环: RX_Run() 阻塞等待并解析数据 */
    while (1)
    {
        RX_Run(&s_uart_rx_machine);
    }
}
