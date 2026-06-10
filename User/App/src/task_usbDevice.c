/**
 * @file    task_usbDevice.c
 * @brief   USB 连接状态检测任务
 *
 * 本任务以 20ms 为周期轮询 USB 设备状态, 处理插入和拔出事件:
 *   - USB 插入: 暂停播放, 设置 g_usb_connected = 1
 *   - USB 拔出: 发送 PLAYMUSIC_REINIT 触发 SD 卡重新扫描
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
        uint8_t usb_now = (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED) ? 1U : 0U;

        if (usb_now && (g_usb_connected == 0))
        {
            STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
            xQueueOverwrite(g_queue_play_music, &cmd);
            g_usb_connected = 1;
        }
        else if ((usb_now == 0) && g_usb_connected)
        {
            STATE_PLAYMUSIC cmd = PLAYMUSIC_REINIT;
            xQueueOverwrite(g_queue_play_music, &cmd);
            g_usb_connected = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
