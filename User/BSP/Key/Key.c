/**
 * @file    Key.c
 * @brief   通用按键状态机驱动实现 (四状态硬件消抖)
 * @details 按键消抖原理:
 *          机械按键在按下/释放时会产生 5~20ms 的抖动,
 *          本驱动通过四状态状态机, 在每个状态切换时插入消抖延时,
 *          只有在消抖时间到后再次确认电平状态一致, 才认为是有效操作。
 *
 *          状态转移图:
 *          KEY_STATE_IDLE(空闲)
 *              |  isPressed()==1 (检测到按下)
 *              v
 *          KEY_STATE_PRESS_DEB(按下消抖)
 *              |  debounce_time_ms 到, 再次确认 isPressed()==1
 *              v
 *          KEY_STATE_PRESSED(确认按下)
 *              |  isPressed()==0 (检测到释放)
 *              v
 *          KEY_STATE_RELEASE_DEB(释放消抖)
 *              |  debounce_time_ms 到, 再次确认 isPressed()==0
 *              v
 *          KEY_STATE_IDLE(回到空闲, 完成一次按键周期)
 *
 *          使用方法:
 *          1. 编写 isPressed() 函数, 检测按键 GPIO 电平
 *          2. 调用 KEY_Init() 初始化状态机
 *          3. 在定时中断或主循环中周期调用 KEY_Run()
 *          4. 通过 key->valid_flag 判断是否按下
 *          5. 按键回调在释放消抖确认后执行, 实现"释放时触发"
 *
 * @note    状态机必须周期性调用, 建议周期 1~10ms
 *          消抖时间建议 20~50ms (取决于具体按键特性)
 */
#include "Key.h"
#include "FreeRTOS.h"   /* HAL_GetTick */

/**
 * @brief   按键状态机初始化
 * @details 初始化按键句柄的所有字段:
 *          - valid_flag 清零
 *          - 状态置为 IDLE
 *          - 记录当前 tick 作为计时基准
 *          - 绑定用户提供的电平检测函数和回调函数
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
 * @details 必须在主循环或定时器中周期调用 (建议 1~10ms)。
 *          四状态消抖流程:
 *          [IDLE] 检测到按下 -> [PRESS_DEB] 等待消抖时间
 *          -> 确认按下 -> [PRESSED] 等待释放
 *          -> [RELEASE_DEB] 等待消抖时间 -> 确认释放, 回调, 回到 IDLE
 *
 *          任一消抖阶段检测到电平抖动, 则回退到上一稳定状态,
 *          有效避免机械抖动引起的误触发。
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
        /* ====================================================================
         * [状态0] KEY_STATE_IDLE (空闲)
         * 功能: 等待按键按下, 持续检测电平
         * 转移: isPressed()==1 -> PRESS_DEB (记录当前 tick, 开始消抖计时)
         * ==================================================================== */
        case KEY_STATE_IDLE:
            if (key->isPressed() == 1)
            {
                key->tick_ms = HAL_GetTick();
                key->curr_state = KEY_STATE_PRESS_DEB;
            }
            break;

        /* ====================================================================
         * [状态1] KEY_STATE_PRESS_DEB (按下消抖)
         * 功能: 等待 debounce_time_ms 后再次确认电平状态
         * 转移:
         *   - 消抖时间到 && 仍按下 -> PRESSED (有效按下, 置 valid_flag=1)
         *   - 消抖时间到 && 已松开 -> IDLE (抖动噪声, 回到空闲)
         * 说明: 消抖时间内任何抖动都会被忽略, 只有消抖结束后确认的状态才有效
         * ==================================================================== */
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

        /* ====================================================================
         * [状态2] KEY_STATE_PRESSED (确认按下)
         * 功能: 按键已被确认按下, 等待用户释放
         * 转移: isPressed()==0 -> RELEASE_DEB (记录 tick, 开始释放消抖)
         * ==================================================================== */
        case KEY_STATE_PRESSED:
            if (key->isPressed() == 0)
            {
                key->tick_ms = HAL_GetTick();
                key->curr_state = KEY_STATE_RELEASE_DEB;
            }
            break;

        /* ====================================================================
         * [状态3] KEY_STATE_RELEASE_DEB (释放消抖)
         * 功能: 等待 debounce_time_ms 后再次确认电平状态
         * 转移:
         *   - 消抖时间到 && 仍松开 -> IDLE (有效释放, 执行回调, 清 valid_flag)
         *   - 消抖时间到 && 又按下 -> PRESSED (抖动, 回到按下状态)
         * 说明: 该状态退出时调用 pressed_callback(), 实现"释放时触发"
         * ==================================================================== */
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
