/**
 * @file    task_usbDevice.c
 * @brief   USB 连接状态检测任务
 *
 * 本任务以 20ms 为周期轮询 USB 设备状态, 处理插入和拔出事件:
 *   - USB 插入: 暂停播放, 设置 g_usb_connected = 1
 *   - USB 拔出: 发送 PLAYMUSIC_REINIT 触发 SD 卡重新扫描
 *
 * 为什么需要这个任务:
 *   当 USB 插入电脑时, STM32 进入 USB 设备模式,
 *   电脑可以访问 SD 卡 (MSC 大容量存储设备)。
 *   此时播放任务无法读取 SD 卡, 必须暂停播放。
 *   拔出后 SD 卡重新成为可用文件系统, 需要重新扫描歌曲列表。
 *
 * 为什么使用 xQueueOverwrite 而非 xQueueSend:
 *   因为播放命令队列深度为 1, Overwrite 保证命令一定能写进去,
 *   即使上次的命令还没被处理也能覆盖。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "usbd_def.h"
#include "app.h"
#include <stdio.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/** @brief USB 状态检测任务句柄 */
TaskHandle_t g_task_usb_device;

void task_usb_device(void *p)
{
    (void)p;

    while (1)
    {
        /*
         * 读取 USB 设备当前状态:
         *   USBD_STATE_CONFIGURED = USB 已配置完成 (主机已识别设备)
         *   非 CONFIGURED 状态包括: 未连接、连接中、挂起等
         */
        uint8_t usb_now = (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) ? 1U : 0U;

        /*
         * 上升沿检测: USB 刚插入
         *   usb_now == 1 && g_usb_connected == 0
         *   → 发送 PAUSE 命令暂停播放 (SD 卡即将被主机独占)
         */
        if (usb_now && (g_usb_connected == 0))
        {
            STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
            xQueueOverwrite(g_queue_play_music, &cmd);
            g_usb_connected = 1;
        }
        /*
         * 下降沿检测: USB 刚拔出
         *   usb_now == 0 && g_usb_connected == 1
         *   → 发送 REINIT 命令重新扫描 SD 卡并重建播放列表
         */
        else if ((usb_now == 0) && g_usb_connected)
        {
            STATE_PLAYMUSIC cmd = PLAYMUSIC_REINIT;
            xQueueOverwrite(g_queue_play_music, &cmd);
            g_usb_connected = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));  /* 20ms 轮询周期 */
    }
}
