/**
 * @file    w25q64.h
 * @brief   W25Q64 SPI Flash 存储器驱动头文件 (8MB)
 * @details W25Q64 是一款 64Mbit (8MB) 串行 Flash 存储器,
 *          通过 SPI 接口与 STM32F407 通信。
 *
 *          存储结构:
 *          - 总容量: 8,388,608 字节 (8MB)
 *          - 页 (Page):   256 字节
 *          - 扇区 (Sector): 4,096 字节 (16 页)
 *          - 块 (Block):  64KB (256 页 / 16 扇区)
 *          - 总扇区数: 2048 个
 *
 *          操作注意事项:
 *          - 写入前必须先擦除 (Flash 特性: 只能将 1 写为 0, 不能将 0 写为 1)
 *          - 最小擦除单位: 扇区 (4KB)
 *          - 最小写入单位: 页 (256 字节), 但需先擦除所在扇区
 *          - 读取无限制, 可从任意地址读取任意长度
 *
 * @date    2025-11-12
 */
#ifndef W25Q64_H
#define W25Q64_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "spi.h"

/* ==================================================================== */
/*                   容量定义和存储结构                                    */
/* ==================================================================== */

/** @brief W25Q64 总容量: 8,388,608 字节 (8MB) */
#define W25Q64_TOTAL_SIZE       (8UL * 1024UL * 1024UL)

/** @brief 页大小: 256 字节 */
#define W25Q64_PAGE_SIZE        256UL

/** @brief 扇区大小: 4096 字节 (16 页) */
#define W25Q64_SECTOR_SIZE      4096UL

/** @brief 块大小: 64KB */
#define W25Q64_BLOCK_SIZE       65536UL

/** @brief 总扇区数: 2048 */
#define W25Q64_TOTAL_SECTORS    (W25Q64_TOTAL_SIZE / W25Q64_SECTOR_SIZE)

/* ==================================================================== */
/*                   SPI 指令定义                                        */
/* ==================================================================== */

#define W25Q64_CMD_WRITE_ENABLE             0x06U   /**< 写使能 */
#define W25Q64_CMD_READ_ID                  0x90U   /**< 读设备 ID */
#define W25Q64_CMD_PAGE_PROGRAM             0x02U   /**< 页编程 */
#define W25Q64_CMD_READ_DATA                0x03U   /**< 读数据 */
#define W25Q64_CMD_READ_STATUS1             0x05U   /**< 读状态寄存器1 (bit0=BSY) */
#define W25Q64_CMD_SECTOR_ERASE             0x20U   /**< 扇区擦除 */
#define W25Q64_CMD_BLOCK_ERASE              0xD8U   /**< 块擦除 */
#define W25Q64_CMD_CHIP_ERASE               0xC7U   /**< 整片擦除 */

/* ==================================================================== */
/*                   驱动返回值类型                                       */
/* ==================================================================== */

typedef enum
{
    W25Q64_OK       = 0,  /*!< 操作成功 */
    W25Q64_ERR_NULL = -1, /*!< 空指针参数 */
    W25Q64_ERR_BUSY = -2, /*!< Flash 忙 */
} W25Q64_Result_t;

/* ==================================================================== */
/*                   函数声明                                             */
/* ==================================================================== */

/**
 * @brief   初始化 W25Q64, 将 CS 置为高电平 (不选中)
 */
void W25Q64_Init(void);

/**
 * @brief   等待 W25Q64 内部操作完成
 * @note    轮询状态寄存器 BSY 位, 每次写/擦除后必须调用
 */
void W25Q64_WaitBusy(void);

/**
 * @brief   获取 W25Q64 设备 ID
 * @param[out] id  输出缓冲区 (至少 3 字节):
 *                  id[0]=制造商ID, id[1]=设备ID高, id[2]=设备ID低
 * @return ::W25Q64_OK        读取成功
 * @return ::W25Q64_ERR_NULL  id 为空指针
 */
W25Q64_Result_t W25Q64_GetID(uint8_t *id);

/**
 * @brief   擦除指定地址所在的扇区 (4KB)
 * @param   addr  目标地址 (自动对齐到扇区起始)
 * @note    擦除操作约 45~400ms, 函数内部阻塞等待完成
 */
void W25Q64_EraseSector(uint32_t addr);

/**
 * @brief   页编程: 向指定页写入最多 256 字节
 * @param   addr  目标地址
 * @param   data  待写入数据缓冲区 [非空]
 * @param   len   写入长度 (1~256 字节)
 * @note    写入前必须先擦除所在扇区!
 */
W25Q64_Result_t W25Q64_PageProgram(uint32_t addr, const uint8_t *data, uint32_t len);

/**
 * @brief   从指定地址读取任意长度数据
 * @param   addr  读取起始地址
 * @param[out] data  输出缓冲区 [非空]
 * @param   len   读取长度 (字节)
 * @return ::W25Q64_OK        读取成功
 * @return ::W25Q64_ERR_NULL  data 为空指针
 */
W25Q64_Result_t W25Q64_ReadData(uint32_t addr, uint8_t *data, uint32_t len);

/**
 * @brief   整片擦除 (8MB)
 * @note    擦除时间约 40~200 秒, 使用前确认不需要保存的数据已导出
 */
void W25Q64_ChipErase(void);

/**
 * @brief   写入整个扇区 (自动擦除 + 16 次页编程)
 * @param   buff    待写入数据 (必须 >= 4096 字节) [非空]
 * @param   sector  扇区编号 (0~2047)
 * @return ::W25Q64_OK        写入成功
 * @return ::W25Q64_ERR_NULL  buff 为空指针
 */
W25Q64_Result_t W25Q64_WriteSector(const uint8_t *buff, uint32_t sector);

/**
 * @brief   读取连续多个扇区
 * @param[out] buff    输出缓冲区 (必须 >= count * 4096 字节) [非空]
 * @param   sector  起始扇区编号
 * @param   count   要读取的扇区数量
 * @return ::W25Q64_OK        读取成功
 * @return ::W25Q64_ERR_NULL  buff 为空指针
 */
W25Q64_Result_t W25Q64_ReadSectors(uint8_t *buff, uint32_t sector, uint32_t count);

/**
 * @brief   写入连续多个扇区 (自动擦除 + 编程)
 * @param   buff    待写入数据 (必须 >= count * 4096 字节) [非空]
 * @param   sector  起始扇区编号
 * @param   count   要写入的扇区数量
 * @return ::W25Q64_OK        写入成功
 * @return ::W25Q64_ERR_NULL  buff 为空指针
 * @note    总耗时 ≈ count * 500ms, 建议在后台任务中执行
 */
W25Q64_Result_t W25Q64_WriteSectors(const uint8_t *buff, uint32_t sector, uint32_t count);

#endif /* W25Q64_H */
