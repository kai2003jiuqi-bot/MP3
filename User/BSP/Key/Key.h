/**
 * @file    Key.h
 * @brief   通用按键状态机驱动 (四状态硬件消抖)
 * @details 支持四状态消抖: 空闲(IDLE) -> 按下消抖(PRESS_DEB) -> 按下确认(PRESSED) -> 释放消抖(RELEASE_DEB)
 *          通过状态机轮询实现按键消抖, 消除机械按键抖动带来的误触发。
 *          用户只需提供 isPressed() 电平检测函数和 pressed_callback() 回调函数。
 *          依赖 FreeRTOS 的 HAL_GetTick() 提供毫秒级计时。
 * @note    状态机需周期调用 (建议 1~10ms), 消抖时间建议 20~50ms
 */
#ifndef KEY_H
#define KEY_H

#include <stdint.h>
#include "gpio.h"

/**
 * @brief 按键驱动模块返回值类型
 */
typedef enum
{
    KEY_OK       = 0,  /*!< 操作成功 */
    KEY_ERR_NULL = -1, /*!< 空指针参数 */
} KEY_Result_t;

/**
 * @brief   按键状态枚举
 * @note    四状态消抖状态机流程:
 *          IDLE -> PRESS_DEB -> PRESSED -> RELEASE_DEB -> IDLE
 *          |          |            |             |
 *          等待按下   消抖计时    等待释放     释放确认
 *                     (抖动返回   (抖动返回     (完成一次
 *                      IDLE)      PRESSED)     按键周期)
 */
typedef enum
{
    KEY_STATE_IDLE,        /**< [状态0] 空闲：等待按键按下 */
    KEY_STATE_PRESS_DEB,   /**< [状态1] 按下消抖：检测到按下后等待消抖时间, 确认是否真实按下 */
    KEY_STATE_PRESSED,     /**< [状态2] 确认按下：按键已稳定按下, 等待释放 */
    KEY_STATE_RELEASE_DEB, /**< [状态3] 释放消抖：检测到释放后等待消抖时间, 确认释放后触发回调 */
} KEY_State_t;

/**
 * @brief   按键句柄结构体
 * @details 每个按键实例对应一个 KEY_Handle_t 结构体,
 *          包含电平检测函数指针、回调函数指针、消抖计时和状态机状态。
 *          支持多个按键实例, 每个实例独立运行状态机。
 */
typedef struct {
    uint8_t (*isPressed)(void);             /**< 按键电平检测函数指针, 返回 1=按下 0=松开 */
    void (*pressed_callback)(void);         /**< 按键按下回调函数指针, 在释放消抖确认后调用 */
    uint8_t  valid_flag;                    /**< 有效按下标志: 0=未按下, 1=已确认按下 */
    uint32_t debounce_time_ms;              /**< 消抖时间(单位:ms), 建议 20~50ms */
    uint32_t tick_ms;                       /**< 状态机计时戳, 记录进入消抖状态的时刻 */
    KEY_State_t curr_state;                 /**< 状态机当前状态 */
    KEY_State_t last_state;                 /**< 状态机上一状态 (可用于调试或状态变化检测) */
} KEY_Handle_t;

/**
 * @brief   按键状态机初始化接口
 * @param   key             按键句柄指针 [非空]
 * @param   debounce_time_ms 消抖时间 (ms)
 * @param   isPressed       按键电平检测函数 (用户实现)
 * @param   pressed_callback 按键按下回调函数 (用户实现, 释放消抖后调用)
 * @return  ::KEY_OK  初始化成功
 * @return  ::KEY_ERR_NULL  key 参数为空指针
 */
KEY_Result_t KEY_Init(KEY_Handle_t *key,
                      uint32_t debounce_time_ms,
                      uint8_t (*isPressed)(void),
                      void (*pressed_callback)(void));

/**
 * @brief   按键状态机运行函数 (需周期性调用)
 * @param   key  按键句柄指针 [非空]
 * @return  ::KEY_OK  运行成功
 * @return  ::KEY_ERR_NULL  key 参数为空指针
 * @note    必须在定时器中断或主循环中以固定周期(1~10ms)调用,
 *          周期越短, 消抖响应越快。
 */
KEY_Result_t KEY_Run(KEY_Handle_t *key);

#endif /* KEY_H */
