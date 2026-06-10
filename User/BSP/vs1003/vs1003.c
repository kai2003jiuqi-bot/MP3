/**
 * @file    vs1003.c
 * @brief   VS1003 音频编解码器驱动实现, 支持 MP3/WAV/MIDI 解码
 *
 * ========== VS1003 简介 ==========
 * VS1003 是一款单芯片 MP3/WMA/MIDI 音频解码器和 ADPCM 编码器。
 * 本驱动通过 SPI1 与 VS1003 通信, 包含:
 *   - 控制通道 (XCS): SPI 分频 64 (~650kHz), 用于读写寄存器
 *   - 数据通道 (XDCS): SPI 分频 16 (~2.6MHz), 用于发送音频数据流
 *
 * ========== SPI 总线共享 ==========
 * SPI1 被以下四个外设共享, 通过 g_spi1_mutex 互斥锁保护:
 *   - VS1003 (音频解码器)
 *   - ILI9341 (LCD 屏幕)
 *   - XPT2046 (触摸控制器, 与 ILI9341 共用 CS)
 *   - W25Q64 (外部 Flash 存储器)
 *
 * 每个外设操作 SPI 前获取互斥锁, 可配置 SPI 分频和模式, 操作后释放。
 *
 * ========== DREQ 握手协议 ==========
 * VS1003 使用 DREQ (Data Request) 引脚进行流控:
 *   - DREQ 高电平: VS1003 内部 FIFO 有空闲空间, 可以接收数据
 *   - DREQ 低电平: FIFO 已满, 需要等待
 * 每次发送前必须等待 DREQ 变为高电平。
 *
 * ========== 使用方法 ==========
 *   1. 调用 VS1003_Init() 初始化, 设置时钟和音量
 *   2. 可选 VS1003_Diagnostic() 自检
 *   3. 调用 VS1003_WriteMusicData() 发送 MP3/WAV 数据流
 *
 * @date    2025-11-12
 */
#include "vs1003.h"
#include "app.h"
#include <stdio.h>
#include <string.h>

/* ==================================================================== */
/*               底层硬件接口层                                           */
/* ==================================================================== */

/*
 * 以下 6 个静态函数封装了 VS1003 的底层硬件操作,
 * 将 GPIO 控制和 SPI 通信抽象为统一的接口,
 * 上层代码无需关心具体引脚和 SPI 配置。
 */

static void VS1003_Delay(uint32_t ms)
{
    vTaskDelay(ms);
}

static uint8_t VS1003_ReadDREQ(void)
{
    return (HAL_GPIO_ReadPin(VS1003_DREQ_GPIO_Port, VS1003_DREQ_Pin) != 0) ? 1 : 0;
}

/*
 * 函数: VS1003_WriteXCS
 * 功能: 控制 VS1003 的命令片选 (XCS) 引脚
 *
 * 操作流程:
 *   拉低 CS 前:
 *     - 获取 SPI 互斥锁 (xSemaphoreTake)
 *     - 配置 SPI 为 64 分频 (~650kHz) — 控制通道慢速, 更可靠
 *     - 重新初始化 SPI 以应用新分频
 *   拉高 CS 后:
 *     - 释放 SPI 互斥锁 (xSemaphoreGive)
 *     - 其他外设现在可以使用 SPI
 */
static void VS1003_WriteXCS(uint8_t state)
{
    if (state == 0)
    {
        xSemaphoreTake(g_spi1_mutex, portMAX_DELAY);
        hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;
        HAL_SPI_Init(&hspi1);
        HAL_GPIO_WritePin(VS1003_CS_GPIO_Port, VS1003_CS_Pin, GPIO_PIN_RESET);
    }
    else
    {
        HAL_GPIO_WritePin(VS1003_CS_GPIO_Port, VS1003_CS_Pin, GPIO_PIN_SET);
        xSemaphoreGive(g_spi1_mutex);
    }
}

/*
 * 函数: VS1003_WriteXDCS
 * 功能: 控制 VS1003 的数据片选 (XDCS) 引脚
 *
 * 操作流程:
 *   与 XCS 类似, 但 SPI 分频为 16 (~2.6MHz) — 数据通道更快, 提高传输效率
 */
static void VS1003_WriteXDCS(uint8_t state)
{
    if (state == 0)
    {
        xSemaphoreTake(g_spi1_mutex, portMAX_DELAY);
        hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
        HAL_SPI_Init(&hspi1);
        HAL_GPIO_WritePin(VS1003_DCS_GPIO_Port, VS1003_DCS_Pin, GPIO_PIN_RESET);
    }
    else
    {
        HAL_GPIO_WritePin(VS1003_DCS_GPIO_Port, VS1003_DCS_Pin, GPIO_PIN_SET);
        xSemaphoreGive(g_spi1_mutex);
    }
}

static void VS1003_WriteRST(uint8_t state)
{
    HAL_GPIO_WritePin(VS1003_RST_GPIO_Port, VS1003_RST_Pin,
                      (state) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* 初始化引脚: 确保两个片选都处于非选中状态 */
static void VS1003_PinInit(void)
{
    HAL_GPIO_WritePin(VS1003_CS_GPIO_Port,  VS1003_CS_Pin,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(VS1003_DCS_GPIO_Port, VS1003_DCS_Pin, GPIO_PIN_SET);
}

static void VS1003_SPI_ReadWrite(uint8_t *tx, uint8_t *rx, uint16_t size)
{
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, size, 100);
}

static void VS1003_SPI_Write(uint8_t *data, uint16_t size)
{
    HAL_SPI_Transmit(&hspi1, data, size, 100);
}

/* ==================================================================== */
/*               寄存器访问接口                                           */
/* ==================================================================== */

/*
 * 函数: VS1003_WriteReg
 * 功能: 写入 VS1003 寄存器 (控制通道)
 *
 * SPI 命令格式:
 *   字节 0: 0x02 (写命令)
 *   字节 1: 寄存器地址
 *   字节 2: 数据高字节
 *   字节 3: 数据低字节
 *
 * 操作流程:
 *   1. 等待 DREQ 高电平 (VS1003 就绪)
 *   2. 拉低 XCS (选中控制通道)
 *   3. 发送 4 字节命令
 *   4. 拉高 XCS (释放控制通道)
 */
void VS1003_WriteReg(uint8_t address, uint16_t data)
{
    uint8_t tx_buf[4];

    while (VS1003_ReadDREQ() == 0)
    {
        VS1003_Delay(1);
    }

    VS1003_WriteXCS(0);

    tx_buf[0] = 0x02U;
    tx_buf[1] = address;
    tx_buf[2] = (uint8_t)((data >> 8) & 0xFFU);
    tx_buf[3] = (uint8_t)( data       & 0xFFU);

    VS1003_SPI_Write(tx_buf, 4);
    VS1003_WriteXCS(1);
}

/*
 * 函数: VS1003_ReadReg
 * 功能: 读取 VS1003 寄存器 (控制通道)
 *
 * SPI 命令格式:
 *   字节 0: 0x03 (读命令)
 *   字节 1: 寄存器地址
 *   字节 2-3: 填充字节 (全 0), 同时从 MISO 接收寄存器值
 *
 * 返回: 16 位寄存器值 (rx_buf[2] 高字节 + rx_buf[3] 低字节)
 */
uint16_t VS1003_ReadReg(uint8_t address)
{
    uint8_t tx_buf[4] = {0};
    uint8_t rx_buf[4] = {0};

    while (VS1003_ReadDREQ() == 0) { }

    VS1003_WriteXCS(0);

    tx_buf[0] = 0x03U;
    tx_buf[1] = address;

    VS1003_SPI_ReadWrite(tx_buf, rx_buf, 4);
    VS1003_WriteXCS(1);

    return (uint16_t)(((uint16_t)rx_buf[2] << 8) | rx_buf[3]);
}

/* ==================================================================== */
/*               初始化与复位                                            */
/* ==================================================================== */

/*
 * 函数: VS1003_Init
 * 功能: 初始化 VS1003 硬件解码器
 *
 * 初始化序列:
 *   1. 引脚初始化 (CS/DCS 置高)
 *   2. 硬件复位 (RST 高电平)
 *   3. 设置时钟寄存器: 0x9800
 *      - bit15: 倍频选择 (1=3.5x 内部倍频)
 *      - bit14-13: 时钟倍率 (10=3.5x)
 *      - bit12-0: 基准时钟分频
 *   4. 设置初始音量
 *   5. 回读 CLOCKF 寄存器验证通信是否正常
 */
void VS1003_Init(uint16_t vol)
{
    VS1003_PinInit();
    VS1003_WriteRST(1);
    VS1003_Delay(10);

    VS1003_WriteReg(VS1003_REG_CLOCKF, 0x9800U);
    VS1003_Delay(50);

    VS1003_SetVolume((uint8_t)vol);

    /* 回读验证: 如果 CLOCKF 写入正确, 说明 SPI 通信正常 */
    if (VS1003_ReadReg(VS1003_REG_CLOCKF) == 0x9800U)
    {
        printf("vs1003 init success\r\n");
    }
    else
    {
        printf("vs1003 init fail\r\n");
    }
}

/*
 * 函数: VS1003_HardwareReset
 * 功能: 硬件复位 VS1003 — 拉低 RST 引脚再释放
 *
 * 复位后 VS1003 需要等待 DREQ 变高才能继续操作,
 * 超时保护: 0xFFFF 次轮询 (~65 秒), 防止死锁。
 * 成功后执行软复位确保完整启动。
 */
void VS1003_HardwareReset(void)
{
    printf("执行硬件复位序列...\r\n");

    VS1003_WriteRST(0);
    VS1003_Delay(100);

    VS1003_WriteRST(1);
    VS1003_Delay(100);

    printf("等待DREQ变高...\r\n");

    uint32_t timeout = 0xFFFFU;
    while (VS1003_ReadDREQ() == 0)
    {
        if (timeout-- == 0)
        {
            printf("错误: 硬复位后DREQ超时!\r\n");
            break;
        }
        VS1003_Delay(1);
    }

    if (VS1003_ReadDREQ() == 1)
    {
        printf("DREQ已变高，执行软复位...\r\n");
        VS1003_SoftReset();
    }

    VS1003_Delay(10);
}

/*
 * 函数: VS1003_SoftReset
 * 功能: 软件复位 VS1003 — 设置 SM_RESET 和 SM_NEWMODE 位
 *
 * 写入 MODE 寄存器 0x0804:
 *   bit10 (SM_RESET):   软件复位
 *   bit2  (SM_NEWMODE): 新模式 (允许低位地址)
 *
 * 复位后等待 DREQ 变高, 带超时保护
 */
void VS1003_SoftReset(void)
{
    printf("执行软件复位...\r\n");

    VS1003_WriteReg(VS1003_REG_MODE, 0x0804U);
    VS1003_Delay(100);

    uint32_t timeout = 0xFFFFU;
    while (VS1003_ReadDREQ() == 0)
    {
        if (timeout-- == 0)
        {
            printf("错误: 软复位后DREQ超时!\r\n");
            break;
        }
        VS1003_Delay(1);
    }

    if (VS1003_ReadDREQ() == 1)
    {
        printf("软复位完成\r\n");
    }
}

/* ==================================================================== */
/*               诊断接口                                                */
/* ==================================================================== */

/*
 * 函数: VS1003_Diagnostic
 * 功能: 读取 VS1003 关键寄存器的值并打印, 用于调试和验证硬件连接
 *
 * 读取的寄存器:
 *   MODE        — 模式配置 (复位后应为 0x0800)
 *   STATUS      — 状态寄存器
 *   BASS        — 低音/高音控制
 *   CLOCKF      — 时钟倍频配置
 *   DECODE_TIME — 当前解码时间 (秒/2)
 *   AUDATA      — 音频数据格式 (采样率等)
 *   VOLUME      — 左右声道音量
 */
void VS1003_Diagnostic(void)
{
    uint8_t i;

    printf("==== VS1003诊断信息 ====\r\n");

    const char *reg_names[] = {
        "MODE", "STATUS", "BASS", "CLOCKF",
        "DECODE_TIME", "AUDATA", "VOLUME"
    };
    const uint8_t reg_addrs[] = {
        VS1003_REG_MODE, VS1003_REG_STATUS, VS1003_REG_BASS,
        VS1003_REG_CLOCKF, VS1003_REG_DECODE_TIME,
        VS1003_REG_AUDATA, VS1003_REG_VOLUME
    };

    for (i = 0; i < 7; i++)
    {
        uint16_t val = VS1003_ReadReg(reg_addrs[i]);
        printf("%s: 0x%04X\r\n", reg_names[i], val);
    }

    printf("DREQ引脚: ");
    if (VS1003_ReadDREQ() == 1)
    {
        printf("高电平 [就绪]\r\n");
    }
    else
    {
        printf("低电平 [繁忙]\r\n");
    }

    printf("==== 诊断完成 ====\r\n\r\n");
}

/* ==================================================================== */
/*               数据流接口                                              */
/* ==================================================================== */

/*
 * 函数: VS1003_WriteMusicData
 * 功能: 将音频数据写入 VS1003 数据 FIFO (数据通道)
 *
 * 操作流程:
 *   1. 检查数据指针不为空
 *   2. 等待 DREQ 高电平 (FIFO 有空闲空间)
 *   3. 拉低 XDCS (选中数据通道)
 *   4. 发送数据
 *   5. 拉高 XDCS (释放数据通道)
 *
 * 注意: 每次发送的数据量应在 VS1003 FIFO 容量限制内 (32 字节)
 */
VS1003_Result_t VS1003_WriteMusicData(const uint8_t *data, uint16_t len)
{
    if (data == NULL)
    {
        return VS1003_ERR_NULL;
    }

    while (VS1003_ReadDREQ() == 0)
    {
        VS1003_Delay(1);
    }

    VS1003_WriteXDCS(0);
    VS1003_SPI_Write((uint8_t *)data, len);
    VS1003_WriteXDCS(1);

    return VS1003_OK;
}

/*
 * 函数: VS1003_SetVolume
 * 功能: 设置 VS1003 左右声道音量
 *
 * VS1003 音量寄存器说明:
 *   - VOLUME 寄存器为 16 位: 高字节 = 左声道, 低字节 = 右声道
 *   - 值越大, 输出越小 (0 = 最大音量, 254 = 静音, 255 有特殊含义)
 *   - 此处左右声道设置相同的音量值
 *
 * 输入 vol 范围: 0~254
 *   当 vol >= 80 时映射到 254 (接近静音, 限制最大衰减)
 *   实际调用时传入的 g_volume 范围通常在 0~20 之间
 */
void VS1003_SetVolume(uint8_t vol)
{
    if (vol >= 80U)
    {
        vol = 254;
    }

    uint16_t vol_reg = ((uint16_t)vol << 8) | vol;
    VS1003_WriteReg(VS1003_REG_VOLUME, vol_reg);
}

void VS1003_SetClock(uint16_t clock_reg)
{
    VS1003_WriteReg(VS1003_REG_CLOCKF, clock_reg);
    VS1003_Delay(50);
}

/*
 * 函数: VS1003_MelodyTest
 * 功能: 播放一段内置 MIDI 旋律, 用于测试 VS1003 硬件连接和音频输出
 *
 * 原理:
 *   向 VS1003 发送 MIDI 格式的测试数据:
 *     - 模式寄存器设为 0x0820 (MIDI 测试模式)
 *     - 每个音符发送 8 字节 MIDI 事件
 *     - "Exit" 命令结束测试
 *     - 恢复为正常模式 0x0800
 *
 * 测试数据格式:
 *   0x53, 0xEF, 0x6E, note, 0x00, 0x00, 0x00, 0x00 — 开始播放音符
 *   0x45, 0x78, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00 — 停止 (Exit)
 */
void VS1003_MelodyTest(void)
{
    printf("开始旋律测试...\r\n");

    VS1003_WriteReg(VS1003_REG_MODE, 0x0820U);
    while (VS1003_ReadDREQ() == 0)
    {
        printf("等待DREQ变高...\r\n");
        VS1003_Delay(1);
    }
    printf("DREQ已变高，继续...\r\n");

    /* MIDI 音符序列 (8 字节事件: 音高值在第 4 字节) */
    const uint8_t melody[] = {
        0x22, 0x22, 0x38, 0x38, 0x44, 0x44, 0x38,
        0x2C, 0x2C, 0x22, 0x22, 0x18, 0x18, 0x22
    };
    /* 每个音符的持续时间 (ms) */
    const int durations[] = {
        400, 400, 400, 400, 400, 400, 800,
        400, 400, 400, 400, 400, 400, 800
    };

    for (int i = 0; i < 14; i++)
    {
        uint8_t test_data[8] = {0x53, 0xEF, 0x6E, melody[i], 0x00, 0x00, 0x00, 0x00};

        VS1003_WriteXDCS(0);
        VS1003_SPI_Write(test_data, 8);
        VS1003_WriteXDCS(1);

        VS1003_Delay(durations[i]);

        /* 发送音符停止命令 (除最后一个音符外) */
        if (i < 13)
        {
            uint8_t stop_data[8] = {0x45, 0x78, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00};
            VS1003_WriteXDCS(0);
            VS1003_SPI_Write(stop_data, 8);
            VS1003_WriteXDCS(1);
            VS1003_Delay(50);
        }
    }

    /* 停止所有音符 */
    uint8_t stop_data[8] = {0x45, 0x78, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00};
    VS1003_WriteXDCS(0);
    VS1003_SPI_Write(stop_data, 8);
    VS1003_WriteXDCS(1);

    /* 恢复为正常解码模式 */
    VS1003_WriteReg(VS1003_REG_MODE, 0x0800U);
    printf("旋律测试结束\r\n");
}

/*
 * 函数: VS1003_GetDecodeTime
 * 功能: 读取 VS1003 硬件解码时间寄存器
 *
 * DECODE_TIME 寄存器每秒钟递增 2 次 (半秒为单位),
 * 配合 g_song_decode_base 可计算当前歌曲已播放的秒数。
 *
 * 返回: 解码时间计数值 (单位: 秒/2)
 */
uint16_t VS1003_GetDecodeTime(void)
{
    return VS1003_ReadReg(VS1003_REG_DECODE_TIME);
}
