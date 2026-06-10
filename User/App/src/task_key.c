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

/*
 * 每个按键维护三组独立的长按追踪变量:
 *   s_xxx_press_tick:   按键按下时的 HAL Tick 时间戳, 用于计算已持续按下的时长
 *   s_xxx_was_valid:    标记按键是否已被 KEY_Run() 确认为有效按下状态
 *   s_xxx_long_handled: 标记长按事件是否已经被处理过, 防止同一按键周期内多次触发
 */

static uint32_t s_inc_press_tick;
static uint8_t  s_inc_was_valid;
static uint8_t  s_inc_long_handled;

static uint32_t s_dec_press_tick;
static uint8_t  s_dec_was_valid;
static uint8_t  s_dec_long_handled;

/* ==================================================================== */
/*               按键电平检测函数                                         */
/* ==================================================================== */

/*
 * 函数: incVolume_IsPressed / decVolume_IsPressed
 * 功能: 读取物理按键对应 GPIO 引脚的电平状态
 * 返回: 1 = 按下 (低电平), 0 = 松开 (高电平)
 * 原理: 按键按下时对地短路, GPIO 读回低电平 (GPIO_PIN_RESET)
 */

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

/*
 * 函数: incVolume_Callback / decVolume_Callback
 * 触发: 按键释放消抖完成后, 由 KEY_Run() 调用
 * 功能: 调整音量 (±10), 写入 VS1003 硬件, 保存配置到 Flash
 *
 * 音量取值范围: 0 ~ VOL_MAX (VOL_MAX 在 app.h 中定义)
 *   - incVolume (音量加): g_volume 减小 (VS1003 音量值越大输出越小)
 *   - decVolume (音量减): g_volume 增大
 *
 * 保护逻辑:
 *   - 如果该按键的长按事件已被处理 (s_xxx_long_handled), 则短按回调不执行
 *     防止长按结束后释放时误触发短按
 */

static void incVolume_Callback(void)
{
    if (s_inc_long_handled) return;  /* 长按已触发, 禁止释放时的短按误触 */

    if (g_volume >= 10) {
        g_volume -= 10;
    } else {
        g_volume = 0;
    }
    VS1003_SetVolume(g_volume);
    g_volume_show = 1;          /* 通知 UI 显示音量条 */
    printf("Vol+  %d\r\n", g_volume);
    File_SaveConfig();          /* 保存音量到 Flash, 掉电恢复用 */
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

/*
 * 函数: task_key
 * 功能: FreeRTOS 按键扫描任务入口
 *
 * 主循环工作流程:
 *   1. 调用 KEY_Run() 驱动按键状态机运行 (内置硬件消抖)
 *   2. 如果按键被确认为有效按下 (valid_flag == 1):
 *      a. 首次有效按下时记录时间戳, 设置 was_valid 标记
 *      b. 持续检测是否超过长按阈值 (500ms)
 *      c. 长按触发: 设置屏幕切换标志 (g_switch_screen_next/prev)
 *      d. 短按由 KEY_Run() 内部的释放消抖回调处理
 *   3. 如果按键不在按下状态, 清理 was_valid 和 long_handled 标记
 *   4. 固定 30ms 周期扫描
 *
 * 长按 vs 短按的区分机制:
 *   - 短按: 由 Key.c 状态机在释放消抖后回调 incVolume_Callback 处理
 *   - 长按: 在此任务中独立检测, 超过 LONG_PRESS_MS 后触发
 *   - 防冲突: short callback 中检查 long_handled 标志, 如长按已处理则跳过
 */

void task_key(void *p)
{
    (void)p;

    /* 初始化两个按键状态机, 消抖时间 50ms */
    KEY_Init(&g_key_inc_volume, 50, incVolume_IsPressed, incVolume_Callback);
    KEY_Init(&g_key_dec_volume, 50, decVolume_IsPressed, decVolume_Callback);

    while (1)
    {
        /* 驱动按键状态机运行 */
        KEY_Run(&g_key_inc_volume);
        KEY_Run(&g_key_dec_volume);

        /* ---- 音量加键长按检测 -> 向右切换屏幕 ---- */
        if (g_key_inc_volume.valid_flag)
        {
            /*
             * 首次进入有效按下状态: 记录时间戳
             * 后续循环中持续检查是否达到长按阈值
             */
            if (!s_inc_was_valid)
            {
                s_inc_press_tick = HAL_GetTick();
                s_inc_was_valid = 1;
            }
            /* 达到长按阈值且尚未处理 → 触发屏幕右切 */
            if (!s_inc_long_handled &&
                (HAL_GetTick() - s_inc_press_tick >= LONG_PRESS_MS))
            {
                s_inc_long_handled = 1;
                g_switch_screen_next = 1;
                File_SavePlaylist();  /* 切屏时顺便保存播放列表 */
            }
        }
        else
        {
            /*
             * 按键已释放:
             * 清除 was_valid 标记
             * 当物理电平确认松开后, 清除 long_handled 标记
             */
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

        vTaskDelay(pdMS_TO_TICKS(30));  /* 30ms 扫描周期 */
    }
}
