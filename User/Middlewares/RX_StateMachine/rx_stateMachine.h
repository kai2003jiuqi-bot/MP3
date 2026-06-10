/**
 * @file rx_stateMachine.h
 * @brief 通用串行帧接收状态机头文件
 *
 * 定义接收状态枚举和状态机结构体, 提供初始化与运行接口。
 * 适用于基于 FreeRTOS StreamBuffer 的串行数据帧接收。
 */
#ifndef RX_STATEMACHINE_H
#define RX_STATEMACHINE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "stream_buffer.h"

/**
 * @brief 串行帧接收状态机返回值类型
 */
typedef enum
{
    RX_OK       = 0,  /*!< 操作成功 */
    RX_ERR_NULL = -1, /*!< 空指针参数 */
} RX_Result_t;

/**
 * @brief 帧接收状态机状态枚举
 */
typedef enum
{
    RX_STATE_HEAD,   /*!< 等待帧头状态 */
    RX_STATE_BAG,    /*!< 接收数据状态 */
    RX_STATE_FINISH  /*!< 帧接收完成状态 */
} RX_State_t;

/**
 * @brief 串行帧接收状态机结构体
 */
typedef struct
{
    uint8_t head;                       /*!< 帧头字节 */
    uint8_t tail;                       /*!< 帧尾字节 */
    StreamBufferHandle_t stream_buffer;  /*!< FreeRTOS 流缓冲句柄 */
    uint8_t byte;                       /*!< 当前接收的字节 */
    RX_State_t state;                   /*!< 当前状态机状态 */
    uint16_t cnt;                       /*!< 已接收数据字节数 */
    uint16_t max_len;                   /*!< 最大帧长度 */
    uint8_t *rx_buf;                    /*!< 接收数据缓冲区指针 */
    void (*rx_done_cb)(void);           /*!< 帧接收完成回调函数 */
} RX_StateMachine_t;

/**
 * @brief 初始化串行帧接收状态机
 * @param rx              状态机实例指针 [非空]
 * @param head            帧头字节
 * @param tail            帧尾字节
 * @param stream_buffer   数据源流缓冲句柄
 * @param max_len         最大帧长
 * @param rx_buf          接收缓冲区 [非空]
 * @param rx_done_cb      帧完成回调 [非空]
 * @return ::RX_OK        初始化成功
 * @return ::RX_ERR_NULL  参数为空指针
 */
RX_Result_t RX_Init(RX_StateMachine_t *rx,
                    uint8_t head,
                    uint8_t tail,
                    StreamBufferHandle_t stream_buffer,
                    uint16_t max_len,
                    uint8_t *rx_buf,
                    void (*rx_done_cb)(void));

/**
 * @brief 驱动帧接收状态机运行
 * @param rx  状态机实例指针 [非空]
 * @return ::RX_OK        运行成功
 * @return ::RX_ERR_NULL  参数为空指针
 * @note 阻塞等待流缓冲数据 (portMAX_DELAY)
 */
RX_Result_t RX_Run(RX_StateMachine_t *rx);

#endif /* RX_STATEMACHINE_H */
