/**
 * @file rx_stateMachine.c
 * @brief 通用串行帧接收状态机, 支持自定义头尾字节和长度校验, 基于 FreeRTOS 流缓冲
 *
 * ========== 工作原理 ==========
 *
 * 本状态机用于从串行数据流中提取完整的帧, 适用于 UART/SPI 等逐字节接收场景。
 *
 *   1. 调用 RX_Init() 配置:
 *      - 帧头 (head): 帧起始标记, 如 ' ' (空格)
 *      - 帧尾 (tail): 帧结束标记, 如 '\r' (回车)
 *      - 流缓冲 (stream_buffer): FreeRTOS StreamBuffer, 中断写入, 任务读取
 *      - 最大长度 (max_len): 帧数据最大长度, 防止缓冲区溢出
 *      - 接收缓冲区 (rx_buf): 存放完整帧内容的缓冲区
 *      - 接收完成回调 (rx_done_cb): 收到完整帧后执行
 *
 *   2. RX_Run() 以 task 形式持续运行:
 *      - 从 StreamBuffer 中逐个字节读取数据
 *      - 状态机包含两个状态: HEAD → BAG
 *      - 收到帧尾后自动截断, 将帧尾替换为 '\0', 调用回调
 *
 * ========== 状态转移图 ==========
 *
 *   RX_STATE_HEAD (等待帧头)
 *       |  byte == head (匹配帧头字节)
 *       v
 *   RX_STATE_BAG (接收数据)
 *       |  byte == tail (匹配帧尾字节)
 *       v
 *   截断 + 回调 + 回到 HEAD
 *
 *   异常转移:
 *   BAG 状态下, 如果接收数据超过 max_len,
 *   强制回到 HEAD, 丢弃当前帧 (溢出保护)
 *
 * ========== 适用场景 ==========
 *   - UART 串口接收
 *   - SPI 从机接收
 *   等需要帧同步的通信协议。
 */
#include "rx_stateMachine.h"

/*
 * 函数: RX_Init
 * 功能: 初始化串行帧接收状态机
 *
 * 配置状态机的所有参数:
 *   - 帧头字节 (用于帧同步)
 *   - 帧尾字节 (用于帧结束检测)
 *   - FreeRTOS StreamBuffer (数据源)
 *   - 最大帧长度 (溢出保护)
 *   - 接收缓冲区 (存放完整帧)
 *   - 接收完成回调
 *
 * 初始状态: RX_STATE_HEAD (等待帧头)
 *
 * @param rx             状态机句柄指针
 * @param head           帧头字节
 * @param tail           帧尾字节
 * @param stream_buffer  FreeRTOS 流缓冲句柄
 * @param max_len        帧数据最大长度
 * @param rx_buf         接收缓冲区
 * @param rx_done_cb     接收完成回调函数
 * @return  RX_OK 或 RX_ERR_NULL (参数为空)
 */
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

/*
 * 函数: RX_Run
 * 功能: 运行帧接收状态机, 从流缓冲中读取并解析数据
 *
 * 使用 xStreamBufferReceive() 从 StreamBuffer 中读取一个字节,
 * timeout = portMAX_DELAY 表示阻塞直到有数据可用。
 *
 * 状态机逻辑:
 *
 *   [HEAD] 等待帧头:
 *     - 如果当前字节匹配 head → 切换到 BAG, 计数器清零
 *     - 否则忽略, 继续等待
 *
 *   [BAG] 接收数据:
 *     - 将每个字节存入 rx_buf[cnt++]
 *     - 如果 cnt >= max_len → 溢出, 回到 HEAD (丢弃此帧)
 *     - 如果字节匹配 tail → 截断 (替换为 '\0'), 调用回调, 回到 HEAD
 *
 * @param rx  状态机句柄指针
 * @return  RX_OK 或 RX_ERR_NULL
 */
RX_Result_t RX_Run(RX_StateMachine_t *rx)
{
    if (rx == NULL)
    {
        return RX_ERR_NULL;
    }

    /*
     * 从 StreamBuffer 中阻塞读取 1 个字节:
     *   portMAX_DELAY 表示无限等待, 直到有数据可用
     *   这是由 ISR (如 UART 接收中断) 写入 StreamBuffer 的数据
     */
    if (xStreamBufferReceive(rx->stream_buffer, &rx->byte, 1, portMAX_DELAY) == pdPASS)
    {
        switch (rx->state)
        {
            case RX_STATE_HEAD:
                /*
                 * [HEAD] 状态: 等待帧头
                 * 匹配则进入 BAG, 开始接收帧数据
                 */
                if (rx->byte == rx->head)
                {
                    rx->state = RX_STATE_BAG;
                    rx->cnt = 0;
                }
                break;

            case RX_STATE_BAG:
                /*
                 * [BAG] 状态: 接收帧数据
                 * 数据存入 rx_buf, 同时检测帧尾和溢出
                 */
                rx->rx_buf[rx->cnt++] = rx->byte;

                /* 溢出保护: 超过 max_len 则丢弃此帧 */
                if (rx->cnt >= rx->max_len)
                {
                    rx->state = RX_STATE_HEAD;
                }

                /* 检测到帧尾: 截断, 调用回调, 回到 HEAD */
                if (rx->byte == rx->tail)
                {
                    rx->state = RX_STATE_HEAD;
                    rx->rx_buf[rx->cnt - 1] = '\0';  /* 将帧尾替换为字符串结束符 */
                    rx->rx_done_cb();                 /* 调用命令处理回调 */
                }
                break;

            default:
                /* 异常状态保护: 强制回到 HEAD */
                rx->state = RX_STATE_HEAD;
                rx->cnt = 0;
                break;
        }
    }

    return RX_OK;
}
