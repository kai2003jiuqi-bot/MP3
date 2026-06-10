/**
 * @file    vs1003.h
 * @brief   VS1003 音频编解码器驱动头文件, 支持 MP3/WAV/MIDI 解码
 * @details VS1003 是一款单芯片 MP3/WMA/MIDI 音频解码器和 ADPCM 编码器,
 *          通过 SPI 接口与 STM32F407 通信。
 *
 *          硬件连接:
 *          - SPI1: SCK/MOSI/MISO 复用 SPI 总线
 *          - XCS:  控制寄存器片选 (SPI 命令/寄存器访问)
 *          - XDCS: 数据片选 (SPI 音乐数据流访问)
 *          - DREQ: 数据请求引脚 (高电平表示缓冲区有空闲)
 *          - RST:  硬件复位 (低电平复位)
 *
 * @date    2025-11-12
 */
#ifndef VS1003_H
#define VS1003_H

#include <stdint.h>
#include "main.h"
#include "spi.h"

/* ==================================================================== */
/*                   VS1003 寄存器地址定义                                  */
/* ==================================================================== */

#define VS1003_REG_MODE        0x00U   /**< MODE 寄存器 */
#define VS1003_REG_STATUS      0x01U   /**< STATUS 寄存器 */
#define VS1003_REG_BASS        0x02U   /**< BASS 寄存器 */
#define VS1003_REG_CLOCKF      0x03U   /**< CLOCKF 时钟倍频寄存器 */
#define VS1003_REG_DECODE_TIME 0x04U   /**< DECODE_TIME 解码时间寄存器 */
#define VS1003_REG_AUDATA      0x05U   /**< AUDATA 音频数据寄存器 */
#define VS1003_REG_WRAM        0x06U   /**< WRAM 寄存器 */
#define VS1003_REG_WRAMADDR    0x07U   /**< WRAMADDR 寄存器 */
#define VS1003_REG_HDAT0       0x08U   /**< HDAT0 头信息寄存器 */
#define VS1003_REG_HDAT1       0x09U   /**< HDAT1 头信息寄存器 */
#define VS1003_REG_VOLUME      0x0BU   /**< VOLUME 音量寄存器 */

/* ==================================================================== */
/*                   驱动返回值类型                                       */
/* ==================================================================== */

typedef enum
{
    VS1003_OK       = 0,  /*!< 操作成功 */
    VS1003_ERR_NULL = -1, /*!< 空指针参数 */
} VS1003_Result_t;

/* ==================================================================== */
/*                   函数声明                                             */
/* ==================================================================== */

/**
 * @brief   写 VS1003 寄存器
 * @param   address  寄存器地址 (VS1003_REG_xxx)
 * @param   data     16位寄存器值
 */
void VS1003_WriteReg(uint8_t address, uint16_t data);

/**
 * @brief   读 VS1003 寄存器
 * @param   address  寄存器地址 (VS1003_REG_xxx)
 * @return  寄存器的 16位 当前值
 */
uint16_t VS1003_ReadReg(uint8_t address);

/**
 * @brief   初始化 VS1003
 * @param   vol  初始音量 (0x00=最大声 ~ 0xFE=最小声, 0xFF=静音)
 */
void VS1003_Init(uint16_t vol);

/**
 * @brief   硬件复位 VS1003
 */
void VS1003_HardwareReset(void);

/**
 * @brief   软件复位 VS1003
 */
void VS1003_SoftReset(void);

/**
 * @brief   自检诊断 (通过串口打印寄存器信息)
 */
void VS1003_Diagnostic(void);

/**
 * @brief   向 VS1003 发送音乐数据流
 * @param   data  MP3/WAV 数据缓冲区 [非空]
 * @param   len   数据长度 (字节数)
 * @return ::VS1003_OK        发送成功
 * @return ::VS1003_ERR_NULL  data 为空指针
 */
VS1003_Result_t VS1003_WriteMusicData(const uint8_t *data, uint16_t len);

/**
 * @brief   设置音量 (双声道同步)
 * @param   vol  音量值 (0x00=最大声 ~ 0xFE=最小声)
 */
void VS1003_SetVolume(uint8_t vol);

/**
 * @brief   旋律测试 (播放"小星星")
 * @note    仅用于硬件功能验证, 正常播放 MP3 时不使用
 */
void VS1003_MelodyTest(void);

/**
 * @brief   获取当前解码时间
 * @return  解码时间 (秒)
 */
uint16_t VS1003_GetDecodeTime(void);

/**
 * @brief   设置 VS1003 时钟倍频
 * @param   clock_reg  CLOCKF 寄存器值 (默认 0x9800)
 */
void VS1003_SetClock(uint16_t clock_reg);

#endif /* VS1003_H */
