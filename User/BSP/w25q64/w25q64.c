/**
 * @file    w25q64.c
 * @brief   W25Q64 SPI Flash 存储器驱动实现 (8MB)
 * @details 本文件实现 W25Q64 的所有读写擦除操作。
 *
 *          存储操作总结:
 *          - 读操作: 无限制, 可从任意地址读取任意长度字节
 *          - 写操作: 必须先擦除, 再编程 (Flash 只能将 1->0 不能 0->1)
 *          - 擦除操作: 最小擦除单位为扇区 (4KB)
 *          - 页编程: 单次最多 256 字节
 *
 *          使用方法:
 *          1. 调用 W25Q64_Init() 初始化
 *          2. 调用 W25Q64_ReadData() / W25Q64_WriteSector() 等函数
 *
 * @date    2025-11-12
 */
#include "w25q64.h"
#include "app.h"

/* ==================================================================== */
/*               底层 SPI 接口层                                           */
/* ==================================================================== */

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

static void W25Q64_WriteEnable(void)
{
    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_WRITE_ENABLE);
    W25Q64_CS_Control(1);
}

/* ==================================================================== */
/*               公开 API 实现                                           */
/* ==================================================================== */

void W25Q64_Init(void)
{
    W25Q64_CS_Control(1);
}

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

void W25Q64_ChipErase(void)
{
    W25Q64_WriteEnable();

    W25Q64_CS_Control(0);
    W25Q64_WriteByte(W25Q64_CMD_CHIP_ERASE);
    W25Q64_CS_Control(1);

    W25Q64_WaitBusy();
}

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
