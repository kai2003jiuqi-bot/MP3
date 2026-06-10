/**
 * @file    Key.c
 * @brief   通用按键状态机驱动实现 (四状态硬件消抖)
 *
 * ========== 按键消抖原理 ==========
 *
 * 机械按键在按下/释放时, 金属触点会产生 5~20ms 的弹跳 (抖动),
 * 如果直接读取 GPIO 电平, 一次按键会产生多次电平跳变,
 * 导致一次按下被误判为多次按下。
 *
 * 本驱动通过四状态状态机来解决该问题:
 *   在检测到状态变化后, 不立即确认, 而是等待消抖时间后再次确认,
 *   如果消抖后的电平与变化前的预测一致, 才判定为有效操作。
 *
 * ========== 状态转移图 ==========
 *
 *   KEY_STATE_IDLE (空闲)
 *       |  isPressed()==1 (检测到按下)
 *       v
 *   KEY_STATE_PRESS_DEB (按下消抖)
 *       |  debounce_time_ms 到, 再次确认 isPressed()==1
 *       v
 *   KEY_STATE_PRESSED (确认按下)
 *       |  isPressed()==0 (检测到释放)
 *       v
 *   KEY_STATE_RELEASE_DEB (释放消抖)
 *       |  debounce_time_ms 到, 再次确认 isPressed()==0
 *       v
 *   KEY_STATE_IDLE (回到空闲, 完成一次按键周期)
 *
 * ========== 使用方法 ==========
 *   1. 编写 isPressed() 函数, 检测按键 GPIO 电平
 *   2. 调用 KEY_Init() 初始化状态机
 *   3. 在定时中断或主循环中周期调用 KEY_Run()
 *   4. 通过 key->valid_flag 判断是否按下
 *   5. 按键回调在释放消抖确认后执行, 实现"释放时触发"
 *
 * @note    状态机必须周期性调用, 建议周期 1~10ms
 *          消抖时间建议 20~50ms (取决于具体按键特性)
 */
#include "Key.h"
#include "FreeRTOS.h"   /* HAL_GetTick */

/**
 * @brief   按键状态机初始化
 *
 * 初始化按键句柄的所有字段:
 *   - valid_flag 清零
 *   - 状态置为 IDLE
 *   - 记录当前 tick 作为计时基准
 *   - 绑定用户提供的电平检测函数和回调函数
 *
 * @param   key             按键句柄指针
 * @param   debounce_time_ms 消抖时间 (ms)
 * @param   isPressed       按键电平检测函数 (用户实现, 返回 1=按下/0=松开)
 * @param   pressed_callback 按键按下回调函数 (在释放消抖确认后调用)
 * @return  ::KEY_OK         初始化成功
 * @return  ::KEY_ERR_NULL   key 为空指针
 */
KEY_Result_t KEY_Init(KEY_Handle_t *key,
                      uint32_t debounce_time_ms,
                      uint8_t (*isPressed)(void),
                      void (*pressed_callback)(void))
{
    if ((key == NULL) || (isPressed == NULL) || (pressed_callback == NULL))
    {
        return KEY_ERR_NULL;
    }

    key->valid_flag        = 0;
    key->curr_state        = KEY_STATE_IDLE;
    key->last_state        = KEY_STATE_IDLE;
    key->tick_ms           = HAL_GetTick();
    key->isPressed         = isPressed;
    key->debounce_time_ms  = debounce_time_ms;
    key->pressed_callback  = pressed_callback;

    return KEY_OK;
}

/**
 * @brief   按键状态机运行函数
 *
 * 必须在主循环或定时器中周期调用 (建议 1~10ms)。
 * 四状态消抖流程:
 *
 *   [IDLE] 检测到按下 -> [PRESS_DEB] 等待消抖时间
 *   -> 确认按下 -> [PRESSED] 等待释放
 *   -> [RELEASE_DEB] 等待消抖时间 -> 确认释放, 回调, 回到 IDLE
 *
 *  任一消抖阶段检测到电平抖动, 则回退到上一稳定状态,
 *  有效避免机械抖动引起的误触发。
 *
 * @param   key  按键句柄指针
 * @return  ::KEY_OK        运行成功
 * @return  ::KEY_ERR_NULL  key 为空指针
 */
KEY_Result_t KEY_Run(KEY_Handle_t *key)
{
    if (key == NULL)
    {
        return KEY_ERR_NULL;
    }

    switch (key->curr_state)
    {
        /* ================================================================
         * [状态0] KEY_STATE_IDLE (空闲)
         *
         * 状态说明: 按键处于释放状态, 等待下一次按下
         *
         * 转移条件: isPressed() == 1
         *   检测到 GPIO 电平变为按下状态
         *   记录当前时间戳 (用于后续消抖计时)
         *   进入按下消抖状态
         * ================================================================ */
        case KEY_STATE_IDLE:
            if (key->isPressed() == 1)
            {
                key->tick_ms = HAL_GetTick();
                key->curr_state = KEY_STATE_PRESS_DEB;
            }
            break;

        /* ================================================================
         * [状态1] KEY_STATE_PRESS_DEB (按下消抖)
         *
         * 状态说明: 检测到按下, 但正在等待消抖时间, 排除机械抖动
         *
         * 转移条件 A: 消抖时间到 && 仍按下
         *   说明不是抖动, 是有效的按下操作
         *   设置 valid_flag = 1, 进入确认按下状态
         *
         * 转移条件 B: 消抖时间到 && 已松开
         *   说明之前的按下信号是抖动噪声
         *   回到空闲状态, 不触发任何回调
         *
         * 关键点: 消抖时间内即使检测到多次电平变化,
         *         也不会改变状态, 只有消抖结束时的电平才算数
         * ================================================================ */
        case KEY_STATE_PRESS_DEB:
            if (HAL_GetTick() - key->tick_ms >= key->debounce_time_ms)
            {
                if (key->isPressed() == 1)
                {
                    key->valid_flag = 1;
                    key->curr_state = KEY_STATE_PRESSED;
                }
                else
                {
                    key->curr_state = KEY_STATE_IDLE;
                }
            }
            break;

        /* ================================================================
         * [状态2] KEY_STATE_PRESSED (确认按下)
         *
         * 状态说明: 按键已被确认为有效按下, valid_flag == 1
         *          上层代码 (如 task_key) 可在此状态检测 long press
         *
         * 转移条件: isPressed() == 0
         *   检测到 GPIO 电平变为释放状态
         *   记录当前时间戳
         *   进入释放消抖状态
         * ================================================================ */
        case KEY_STATE_PRESSED:
            if (key->isPressed() == 0)
            {
                key->tick_ms = HAL_GetTick();
                key->curr_state = KEY_STATE_RELEASE_DEB;
            }
            break;

        /* ================================================================
         * [状态3] KEY_STATE_RELEASE_DEB (释放消抖)
         *
         * 状态说明: 检测到释放, 但正在等待消抖时间, 排除释放抖动
         *
         * 转移条件 A: 消抖时间到 && 仍松开
         *   说明是有效的释放操作
         *   执行 pressed_callback() — "释放时触发" 设计
         *   清 valid_flag, 回到空闲状态
         *
         * 转移条件 B: 消抖时间到 && 又按下
         *   说明之前的释放信号是抖动
         *   (实际上用户没有松开手指)
         *   回到确认按下状态
         * ================================================================ */
        case KEY_STATE_RELEASE_DEB:
            if (HAL_GetTick() - key->tick_ms >= key->debounce_time_ms)
            {
                if (key->isPressed() == 0)
                {
                    key->pressed_callback();
                    key->valid_flag = 0;
                    key->curr_state = KEY_STATE_IDLE;
                }
                else
                {
                    key->curr_state = KEY_STATE_PRESSED;
                }
            }
            break;

        default:
            /* 异常状态保护: 强制回到空闲 */
            key->curr_state = KEY_STATE_IDLE;
            break;
    }

    return KEY_OK;
}
