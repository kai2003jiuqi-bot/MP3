/**
 * @file    vs1003.c
 * @brief   VS1003 音频编解码器驱动实现, 支持 MP3/WAV/MIDI 解码
 * @details VS1003 驱动核心功能:
 *
 *          1. SPI通信:
 *             - 使用 hspi1 (SPI1), 与 ILI9341/XPT2046/W25Q64 共享总线
 *             - 通过互斥量 g_spi1_mutex 保证 SPI 总线独占访问
 *             - 控制通道 (XCS): SPI 分频 64 (~650kHz)
 *             - 数据通道 (XDCS): SPI 分频 16 (~2.6MHz)
 *
 *          2. DREQ 握手:
 *             - DREQ 高电平: VS1003 内部缓冲区有空闲, 可以接收数据
 *             - DREQ 低电平: 缓冲区满, 需等待
 *
 *          使用方法:
 *          1. 调用 VS1003_Init() 初始化, 设置时钟和音量
 *          2. 可选 VS1003_Diagnostic() 自检
 *          3. 调用 VS1003_WriteMusicData() 发送 MP3/WAV 数据流
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

static void VS1003_Delay(uint32_t ms)
{
    vTaskDelay(ms);
}

static uint8_t VS1003_ReadDREQ(void)
{
    return (HAL_GPIO_ReadPin(VS1003_DREQ_GPIO_Port, VS1003_DREQ_Pin) != 0) ? 1 : 0;
}

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

void VS1003_Init(uint16_t vol)
{
    VS1003_PinInit();
    VS1003_WriteRST(1);
    VS1003_Delay(10);

    VS1003_WriteReg(VS1003_REG_CLOCKF, 0x9800U);
    VS1003_Delay(50);

    VS1003_SetVolume((uint8_t)vol);

    if (VS1003_ReadReg(VS1003_REG_CLOCKF) == 0x9800U)
    {
        printf("vs1003 init success\r\n");
    }
    else
    {
        printf("vs1003 init fail\r\n");
    }
}

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

    const uint8_t melody[] = {
        0x22, 0x22, 0x38, 0x38, 0x44, 0x44, 0x38,
        0x2C, 0x2C, 0x22, 0x22, 0x18, 0x18, 0x22
    };
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

        if (i < 13)
        {
            uint8_t stop_data[8] = {0x45, 0x78, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00};
            VS1003_WriteXDCS(0);
            VS1003_SPI_Write(stop_data, 8);
            VS1003_WriteXDCS(1);
            VS1003_Delay(50);
        }
    }

    uint8_t stop_data[8] = {0x45, 0x78, 0x69, 0x74, 0x00, 0x00, 0x00, 0x00};
    VS1003_WriteXDCS(0);
    VS1003_SPI_Write(stop_data, 8);
    VS1003_WriteXDCS(1);

    VS1003_WriteReg(VS1003_REG_MODE, 0x0800U);
    printf("旋律测试结束\r\n");
}

uint16_t VS1003_GetDecodeTime(void)
{
    return VS1003_ReadReg(VS1003_REG_DECODE_TIME);
}
