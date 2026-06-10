/**
 * @file rx_stateMachine.c
 * @brief 通用串行帧接收状态机, 支持自定义头尾字节和长度校验, 基于 FreeRTOS 流缓冲
 *
 * 工作原理:
 *   1. 用户通过 RX_Init() 配置帧头(head)、帧尾(tail)、最大长度(max_len)、
 *      接收缓冲区(rx_buf)和接收完成回调(rx_done_cb)
 *   2. RX_Run() 从 FreeRTOS StreamBuffer 中逐个字节读取数据
 *   3. 状态机包含两个状态:
 *      - RX_STATE_HEAD: 等待帧头字节, 匹配后切换到 RX_STATE_BAG
 *      - RX_STATE_BAG:  接收有效数据, 每收到一个字节就存入 rx_buf
 *                       当收到帧尾字节时, 自动截断并将状态切回 RX_STATE_HEAD
 *   4. 状态机以 task 形式持续运行, 使用 portMAX_DELAY 阻塞等待流缓冲数据
 *
 * 适用场景: UART 串口接收、SPI 从机接收等需要帧同步的通信协议。
 */
#include "rx_stateMachine.h"

RX_Result_t RX_Init(RX_StateMachine_t *rx,
                    uint8_t head,
                    uint8_t tail,
                    StreamBufferHandle_t stream_buffer,
                    uint16_t max_len,
                    uint8_t *rx_buf,
                    void (*rx_done_cb)(void))
{
    if ((rx == NULL) || (rx_buf == NULL) || (rx_done_cb == NULL))
    {
        return RX_ERR_NULL;
    }

    rx->head          = head;
    rx->tail          = tail;
    rx->stream_buffer = stream_buffer;
    rx->max_len       = max_len;
    rx->rx_buf        = rx_buf;
    rx->rx_done_cb    = rx_done_cb;
    rx->state         = RX_STATE_HEAD;
    rx->cnt           = 0;

    return RX_OK;
}

RX_Result_t RX_Run(RX_StateMachine_t *rx)
{
    if (rx == NULL)
    {
        return RX_ERR_NULL;
    }

    if (xStreamBufferReceive(rx->stream_buffer, &rx->byte, 1, portMAX_DELAY) == pdPASS)
    {
        switch (rx->state)
        {
            case RX_STATE_HEAD:
                if (rx->byte == rx->head)
                {
                    rx->state = RX_STATE_BAG;
                    rx->cnt = 0;
                }
                break;

            case RX_STATE_BAG:
                rx->rx_buf[rx->cnt++] = rx->byte;

                /* 接收数据溢出保护 */
                if (rx->cnt >= rx->max_len)
                {
                    rx->state = RX_STATE_HEAD;
                }

                /* 检测到帧尾, 帧接收完成 */
                if (rx->byte == rx->tail)
                {
                    rx->state = RX_STATE_HEAD;
                    rx->rx_buf[rx->cnt - 1] = '\0';
                    rx->rx_done_cb();
                }
                break;

            default:
                rx->state = RX_STATE_HEAD;
                rx->cnt = 0;
                break;
        }
    }

    return RX_OK;
}
