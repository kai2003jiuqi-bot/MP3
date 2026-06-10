/**
 * @file    w25q64.c
 * @brief   W25Q64 SPI Flash 存储器驱动实现 (8MB)
 *
 * ========== W25Q64 简介 ==========
 * W25Q64 是一款 64Mbit (8MB) 的 SPI NOR Flash 存储器。
 * 在本项目中用于存储:
 *   - 播放列表配置文件 (playlist.cfg) — 掉电保存歌曲索引和播放状态
 *   - 播放模式/音量配置 (mode.cfg) — 掉电保存用户设置
 *   - 字库文件 (my_font_song_14.bin) — LVGL 中文字体
 *
 * ========== 存储操作总结 ==========
 *   - 读操作: 无限制, 可从任意地址读取任意长度字节
 *   - 写操作 (页编程): 必须先擦除, 再编程
 *     因为 Flash 只能将 1->0, 不能将 0->1
 *   - 擦除操作: 最小擦除单位为扇区 (4KB)
 *   - 页编程: 单次最多 256 字节
 *
 * ========== 写入流程 ==========
 *   1. 写使能 (Write Enable)
 *   2. 擦除扇区 (Sector Erase, 4KB)
 *   3. 页编程 (Page Program, 256 字节 × 16 次填满一个扇区)
 *   4. 等待忙状态结束 (Wait Busy)
 *
 * ========== 扇区结构 ==========
 *   1 扇区 = 16 页 = 4096 字节
 *   1 页   = 256 字节
 *   全片  = 2048 扇区 = 8MB
 *
 * ========== 使用方法 ==========
 *   1. 调用 W25Q64_Init() 初始化
 *   2. 调用 W25Q64_ReadData() / W25Q64_WriteSector() 等函数
 *
 * @date    2025-11-12
 */
#include "w25q64.h"
#include "app.h"

/* ==================================================================== */
/*               底层 SPI 接口层                                           */
/* ==================================================================== */

/*
 * 函数: W25Q64_CS_Control
 * 功能: 控制 W25Q64 的片选 (CS) 引脚
 *
 * SPI 分频: 16 (~2.6MHz) — Flash 支持较高速度
 *
 * 操作流程 (拉低 CS):
 *   1. 获取 SPI 互斥锁
 *   2. 配置分频为 16
 *   3. 初始化 SPI
 *   4. 拉低 CS
 *
 * 操作流程 (拉高 CS):
 *   1. 拉高 CS
 *   2. 释放 SPI 互斥锁
 */
static void W25Q64_CS_Control(uint8_t state)
{
    if (state == 0)
    {
        xSemaphoreTake(g_spi1_mutex, portMAX_DELAY);
        hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
        HAL_SPI_Init(&hspi1);
        HAL_GPIO_WritePin(W25Q64_CS_GPIO_Port, W25Q64_CS_Pin, GPIO_PIN_RESET);
    }
    else
    {
        HAL_GPIO_WritePin(W25Q64_CS_GPIO_Port, W25Q64_CS_Pin, GPIO_PIN_SET);
        xSemaphoreGive(g_spi1_mutex);
    }
}

static void W25Q64_WriteByte(uint8_t data)
{
    HAL_SPI_Transmit(&hspi1, &data, 1, 100);
}

static void W25Q64_WriteBytes(const uint8_t *data, uint32_t len)
{
    HAL_SPI_Transmit(&hspi1, (uint8_t *)data, len, 100);
}

static void W25Q64_ReadBytes(uint8_t *data, uint32_t len)
{
    HAL_SPI_Receive(&hspi1, data, len, 100);
}

/* ==================================================================== */
/*               内部辅助函数 (写使能)                                     */
/* ==================================================================== */

/*
 * 函数: W25Q64_WriteEnable
 * 功能: 发送写使能命令 (WREN, 0x06)
 *
 * W25Q64 在进行任何写/擦除操作前, 必须先发送 WREN 命令,
 * 否则后续命令将被忽略。
 * 写使能在每次 CS 拉高后自动清除, 因此每次操作前都需要重新使能。
 *
 * 操作流程:
 *   1. 拉低 CS
 *   2. 发送 0x06 (WREN)
 *   3. 拉高 CS
 */
static void W25Q64_WriteEnable(void)
{
    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_CS_Control(1);
}

/* ==================================================================== */
/*               公开 API 实现                                           */
/* ==================================================================== */

/*
 * 函数: W25Q64_Init
 * 功能: 初始化 W25Q64
 *
 * 实质: CS 置高, 确保 Flash 处于非选中状态 (默认状态)
 */
void W25Q64_Init(void)
{
    W25Q64_CS_Control(1);
}

/*
 * 函数: W25Q64_WaitBusy
 * 功能: 等待 Flash 内部操作完成 (轮询 BUSY 位)
 *
 * 原理:
 *   读取状态寄存器 1 (RDSR1, 0x05)
 *   检查 bit0 (BUSY 位): 1=忙碌, 0=就绪
 *
 * 擦除操作需要 50~400ms, 页编程需要 0.5~3ms,
 * 此函数在操作完成后返回。
 */
void W25Q64_WaitBusy(void)
{
    uint8_t status;

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_READ_STATUS1);

    do
    {
        W25Q64_ReadBytes(&status, 1);
    } while (status & 0x01U);

    W25Q64_CS_Control(1);
}

/*
 * 函数: W25Q64_GetID
 * 功能: 读取 W25Q64 的 3 字节设备 ID
 *
 * 命令: READ_ID (0x90) + 3 个 0x9F 填充字节
 * 返回: 3 字节 ID (制造商 ID + 设备类型 + 容量)
 *   如 W25Q64: 0xEF 0x40 0x17
 */
W25Q64_Result_t W25Q64_GetID(uint8_t *id)
{
    if (id == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_READ_ID);

    for (uint8_t i = 0; i < 3; i++)
    {
        W25Q64_WriteByte(0x9f);
    }

    W25Q64_ReadBytes(id, 3);
    W25Q64_CS_Control(1);

    return W25Q64_OK;
}

/*
 * 函数: W25Q64_EraseSector
 * 功能: 擦除指定地址所在的 4KB 扇区 (所有位变为 1)
 *
 * 命令格式 (SECTOR_ERASE, 0x20):
 *   字节 0: 0x20
 *   字节 1: 地址 A23-A16
 *   字节 2: 地址 A15-A8
 *   字节 3: 地址 A7-A0
 *
 * 操作流程:
 *   1. 写使能
 *   2. 拉低 CS
 *   3. 发送 0x20 + 3 字节地址
 *   4. 拉高 CS
 *   5. 等待擦除完成
 */
void W25Q64_EraseSector(uint32_t addr)
{
    W25Q64_WriteEnable();

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_SECTOR_ERASE);
    W25Q64_WriteByte((uint8_t)((addr >> 16) & 0xFFU));
    W25Q64_WriteByte((uint8_t)((addr >> 8)  & 0xFFU));
    W25Q64_WriteByte((uint8_t)( addr        & 0xFFU));
    W25Q64_CS_Control(1);

    W25Q64_WaitBusy();
}

/*
 * 函数: W25Q64_PageProgram
 * 功能: 向指定地址写入数据 (单页, 最多 256 字节)
 *
 * 命令格式 (PAGE_PROGRAM, 0x02):
 *   字节 0: 0x02
 *   字节 1: 地址 A23-A16
 *   字节 2: 地址 A15-A8
 *   字节 3: 地址 A7-A0
 *   字节 4+: 数据 (最多 256 字节)
 *
 * 注意:
 *   - 写入前必须先将目标扇区擦除
 *   - 单次写入不能跨越页边界 (256 字节对齐)
 *   - 如果写入数据超过页末尾, 会自动回卷到页开头
 *
 * @param addr  目标地址 (页对齐)
 * @param data  数据指针
 * @param len   数据长度 (≤ 256)
 */
W25Q64_Result_t W25Q64_PageProgram(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (data == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    W25Q64_WriteEnable();

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_PAGE_PROGRAM);
    W25Q64_WriteByte((uint8_t)((addr >> 16) & 0xFFU));
    W25Q64_WriteByte((uint8_t)((addr >> 8)  & 0xFFU));
    W25Q64_WriteByte((uint8_t)( addr        & 0xFFU));

    W25Q64_WriteBytes(data, len);
    W25Q64_CS_Control(1);

    W25Q64_WaitBusy();

    return W25Q64_OK;
}

/*
 * 函数: W25Q64_ReadData
 * 功能: 从指定地址读取数据 (无长度限制)
 *
 * 命令格式 (READ_DATA, 0x03):
 *   字节 0: 0x03
 *   字节 1: 地址 A23-A16
 *   字节 2: 地址 A15-A8
 *   字节 3: 地址 A7-A0
 *   后续: SPI 时钟继续, Flash 在 MOSI=0xFF 下将数据从 MISO 送出
 *
 * 读操作不需要写使能, 不限制对齐。
 *
 * @param addr  起始地址
 * @param data  输出缓冲区
 * @param len   读取长度
 */
W25Q64_Result_t W25Q64_ReadData(uint32_t addr, uint8_t *data, uint32_t len)
{
    if (data == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_READ_DATA);
    W25Q64_WriteByte((uint8_t)((addr >> 16) & 0xFFU));
    W25Q64_WriteByte((uint8_t)((addr >> 8)  & 0xFFU));
    W25Q64_WriteByte((uint8_t)( addr        & 0xFFU));

    W25Q64_ReadBytes(data, len);
    W25Q64_CS_Control(1);

    return W25Q64_OK;
}

/*
 * 函数: W25Q64_ChipErase
 * 功能: 擦除整个 W25Q64 芯片 (8MB 所有位变为 1)
 *
 * 警告: 此操作耗时约 30~60 秒!
 * 操作流程: 写使能 → 发送 CHIP_ERASE 命令 → 等待完成
 */
void W25Q64_ChipErase(void)
{
    W25Q64_WriteEnable();

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_CHIP_ERASE);
    W25Q64_CS_Control(1);

    W25Q64_WaitBusy();
}

/*
 * 函数: W25Q64_WriteSector
 * 功能: 写一个完整扇区 (4096 字节)
 *
 * 操作流程:
 *   1. 擦除目标扇区
 *   2. 分 16 次页编程写入 (每次 256 字节)
 *
 * @param buff  待写入的 4096 字节数据
 * @param sector 扇区号 (0 ~ 2047)
 */
W25Q64_Result_t W25Q64_WriteSector(const uint8_t *buff, uint32_t sector)
{
    if (buff == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    uint32_t addr = sector * W25Q64_SECTOR_SIZE;
    W25Q64_EraseSector(addr);

    for (uint32_t j = 0; j < 16; j++)
    {
        W25Q64_PageProgram(addr + j * W25Q64_PAGE_SIZE,
                           buff + j * W25Q64_PAGE_SIZE,
                           W25Q64_PAGE_SIZE);
    }

    return W25Q64_OK;
}

/*
 * 函数: W25Q64_ReadSectors
 * 功能: 从指定扇区开始读取连续多个扇区数据
 *
 * @param buff   输出缓冲区
 * @param sector 起始扇区号
 * @param count  扇区数量
 */
W25Q64_Result_t W25Q64_ReadSectors(uint8_t *buff, uint32_t sector, uint32_t count)
{
    if (buff == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    uint32_t addr = sector * W25Q64_SECTOR_SIZE;
    for (uint32_t j = 0; j < count; j++)
    {
        W25Q64_ReadData(addr + j * W25Q64_SECTOR_SIZE,
                        buff + j * W25Q64_SECTOR_SIZE,
                        W25Q64_SECTOR_SIZE);
    }

    return W25Q64_OK;
}

/*
 * 函数: W25Q64_WriteSectors
 * 功能: 写入连续多个扇区 (每个扇区先擦除再写入)
 *
 * 这是最高层的写入接口:
 *   自动处理扇区选择、擦除、分页编程
 *
 * @param buff   待写入数据 (count × 4096 字节)
 * @param sector 起始扇区号
 * @param count  扇区数量
 */
W25Q64_Result_t W25Q64_WriteSectors(const uint8_t *buff, uint32_t sector, uint32_t count)
{
    if (buff == NULL)
    {
        return W25Q64_ERR_NULL;
    }

    uint32_t addr = sector * W25Q64_SECTOR_SIZE;
    for (uint32_t j = 0; j < count; j++)
    {
        uint32_t sector_addr = addr + j * W25Q64_SECTOR_SIZE;

        W25Q64_EraseSector(sector_addr);

        for (uint32_t i = 0; i < 16; i++)
        {
            W25Q64_PageProgram(sector_addr + i * W25Q64_PAGE_SIZE,
                               buff + j * W25Q64_SECTOR_SIZE + i * W25Q64_PAGE_SIZE,
                               W25Q64_PAGE_SIZE);
        }
    }

    return W25Q64_OK;
}
