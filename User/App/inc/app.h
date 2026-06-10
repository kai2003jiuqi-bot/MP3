/**
 * @file    app.h
 * @brief   应用层全局声明头文件
 *
 * 本文件定义了 MP3 播放器应用层中所有任务间通信的原语
 * (队列、信号量、流缓冲)以及全局共享状态变量。
 *
 * 音量映射说明:
 *   VS1003 硬件音量寄存器范围 0~254 (0=最大声, 254=最小声)。
 *   UI 层使用 0~VOL_MAX(80) 的整数值, 通过 VS1003_SetVolume()
 *   内部比例映射到 VS1003 的 0~254 范围, 其中 0 对应最大声。
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stream_buffer.h"
#include "semphr.h"
#include "queue.h"
#include "Key.h"

/** @brief UI 层音量最大值 */
#define VOL_MAX  80U

/* ==================================================================== */
/*                              枚举类型定义                              */
/* ==================================================================== */

/**
 * @brief 播放控制命令枚举
 */
typedef enum
{
    PLAYMUSIC_PLAY,       /*!< 恢复/开始播放 */
    PLAYMUSIC_PAUSE,      /*!< 暂停播放 */
    PLAYMUSIC_PREV,       /*!< 切换到上一首 */
    PLAYMUSIC_NEXT,       /*!< 切换到下一首 */
    PLAYMUSIC_FADE_PAUSE, /*!< 淡出后暂停 */
    PLAYMUSIC_FADE_NEXT,  /*!< 淡出后切到下一首 */
    PLAYMUSIC_FADE_PREV,  /*!< 淡出后切到上一首 */
    PLAYMUSIC_FADE_IN,    /*!< 淡入恢复播放 */
    PLAYMUSIC_SEEK,       /*!< 拖动进度条跳转到指定位置 */
    PLAYMUSIC_REINIT,     /*!< USB 拔出后重新扫描 SD 卡 */
} STATE_PLAYMUSIC;

/**
 * @brief 播放模式枚举
 */
typedef enum
{
    PLAY_MODE_ALL     = 0, /*!< 顺序循环播放 */
    PLAY_MODE_ONE     = 1, /*!< 单曲循环 */
    PLAY_MODE_SHUFFLE = 2, /*!< 随机播放 */
} PLAY_MODE;

/* ==================================================================== */
/*                            任务句柄声明                                */
/* ==================================================================== */

extern TaskHandle_t g_task_lvgl_handler;
extern TaskHandle_t g_task_uart_rx;
extern TaskHandle_t g_task_uart_tx;
extern TaskHandle_t g_task_play_music;
extern TaskHandle_t g_task_key;
extern TaskHandle_t g_task_usb_device;

/* ==================================================================== */
/*                          RTOS 同步原语声明                             */
/* ==================================================================== */

extern QueueHandle_t         g_queue_play_music;
extern SemaphoreHandle_t     g_spi1_mutex;
extern SemaphoreHandle_t     g_sem_uart_tx;
extern StreamBufferHandle_t  g_stream_uart_rx;
extern StreamBufferHandle_t  g_stream_uart_tx;

/* ==================================================================== */
/*                          全局共享状态变量                               */
/* ==================================================================== */

extern KEY_Handle_t  g_key_inc_volume;
extern KEY_Handle_t  g_key_dec_volume;

extern volatile uint8_t  g_volume;
extern volatile uint8_t  g_volume_show;
extern volatile uint8_t  g_switch_screen_next;
extern volatile uint8_t  g_switch_screen_prev;
extern volatile uint8_t  g_play_mode;
extern volatile uint32_t g_song_file_size;
extern volatile uint16_t g_song_bitrate;
extern volatile uint32_t g_song_audio_start;
extern volatile uint8_t  g_reset_progress;
extern volatile uint32_t g_song_decode_base;
extern volatile uint32_t g_song_seek_target;
extern volatile uint8_t  g_seek_active;
extern volatile uint8_t  g_usb_connected;

/* ==================================================================== */
/*                             任务函数声明                               */
/* ==================================================================== */

void task_lvgl_handler(void *p);
void task_uart_rx(void *p);
void task_uart_tx(void *p);
void task_play_music(void *pvParameters);
void task_key(void *p);
void task_usb_device(void *p);

#endif /* APP_H */
