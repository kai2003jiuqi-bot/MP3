/**
 * @file    task_key.c
 * @brief   按键扫描任务
 *
 * 本任务负责轮询两个物理按键 (音量加 / 音量减),
 * 并实现两种按键交互方式:
 *
 *   1. 短按 (按下降后 50ms 内释放) -> 调整音量, 每次 ±10 个单位
 *   2. 长按 (按键持续按下超过 500ms) -> 切换屏幕
 *
 * 关于音量变化时调用 File_SaveConfig() 的原因:
 *   系统需要在掉电后恢复上次的音量设置, 每次音量变化时保存。
 */

#include "FreeRTOS.h"
#include "task.h"
#include "Key.h"
#include "gpio.h"
#include "app.h"
#include "vs1003.h"
#include "file.h"

/** @brief 长按判定阈值 (毫秒) */
#define LONG_PRESS_MS  500U

/* ==================================================================== */
/*               内部函数声明                                             */
/* ==================================================================== */

static uint8_t incVolume_IsPressed(void);
static uint8_t decVolume_IsPressed(void);
static void    incVolume_Callback(void);
static void    decVolume_Callback(void);

/* ==================================================================== */
/*               全局变量定义 (匹配 app.h 中的 extern)                    */
/* ==================================================================== */

TaskHandle_t  g_task_key;
KEY_Handle_t  g_key_inc_volume;
KEY_Handle_t  g_key_dec_volume;

/* ==================================================================== */
/*               长按追踪变量                                             */
/* ==================================================================== */

static uint32_t s_inc_press_tick;
static uint8_t  s_inc_was_valid;
static uint8_t  s_inc_long_handled;

static uint32_t s_dec_press_tick;
static uint8_t  s_dec_was_valid;
static uint8_t  s_dec_long_handled;

/* ==================================================================== */
/*               按键电平检测函数                                         */
/* ==================================================================== */

static uint8_t incVolume_IsPressed(void)
{
    return (HAL_GPIO_ReadPin(INC_VOLUME_GPIO_Port, INC_VOLUME_Pin) == GPIO_PIN_RESET) ? 1 : 0;
}

static uint8_t decVolume_IsPressed(void)
{
    return (HAL_GPIO_ReadPin(DEC_VOLUME_GPIO_Port, DEC_VOLUME_Pin) == GPIO_PIN_RESET) ? 1 : 0;
}

/* ==================================================================== */
/*               按键短按回调函数                                         */
/* ==================================================================== */

static void incVolume_Callback(void)
{
    if (s_inc_long_handled) return;

    if (g_volume >= 10) {
        g_volume -= 10;
    } else {
        g_volume = 0;
    }
    VS1003_SetVolume(g_volume);
    g_volume_show = 1;
    printf("Vol+  %d\r\n", g_volume);
    File_SaveConfig();
}

static void decVolume_Callback(void)
{
    if (s_dec_long_handled) return;

    if (g_volume <= VOL_MAX - 10) {
        g_volume += 10;
    } else {
        g_volume = VOL_MAX;
    }
    VS1003_SetVolume(g_volume);
    g_volume_show = 1;
    printf("Vol-  %d\r\n", g_volume);
    File_SaveConfig();
}

/* ==================================================================== */
/*               按键扫描任务主函数                                       */
/* ==================================================================== */

void task_key(void *p)
{
    (void)p;

    KEY_Init(&g_key_inc_volume, 50, incVolume_IsPressed, incVolume_Callback);
    KEY_Init(&g_key_dec_volume, 50, decVolume_IsPressed, decVolume_Callback);

    while (1)
    {
        KEY_Run(&g_key_inc_volume);
        KEY_Run(&g_key_dec_volume);

        /* ---- 音量加键长按检测 -> 向右切换屏幕 ---- */
        if (g_key_inc_volume.valid_flag)
        {
            if (!s_inc_was_valid)
            {
                s_inc_press_tick = HAL_GetTick();
                s_inc_was_valid = 1;
            }
            if (!s_inc_long_handled &&
                (HAL_GetTick() - s_inc_press_tick >= LONG_PRESS_MS))
            {
                s_inc_long_handled = 1;
                g_switch_screen_next = 1;
                File_SavePlaylist();
            }
        }
        else
        {
            s_inc_was_valid = 0;
            if (!incVolume_IsPressed())
            {
                s_inc_long_handled = 0;
            }
        }

        /* ---- 音量减键长按检测 -> 向左切换屏幕 ---- */
        if (g_key_dec_volume.valid_flag)
        {
            if (!s_dec_was_valid)
            {
                s_dec_press_tick = HAL_GetTick();
                s_dec_was_valid = 1;
            }
            if (!s_dec_long_handled &&
                (HAL_GetTick() - s_dec_press_tick >= LONG_PRESS_MS))
            {
                s_dec_long_handled = 1;
                g_switch_screen_prev = 1;
                File_SavePlaylist();
            }
        }
        else
        {
            s_dec_was_valid = 0;
            if (!decVolume_IsPressed())
            {
                s_dec_long_handled = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
