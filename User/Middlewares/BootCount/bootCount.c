/**
 * @file bootCount.c
 * @brief 启动次数记录模块
 *
 * 磨损均衡策略:
 *   在 Flash 中划出一个扇区(128KB), 按半字(2字节)顺序写入递增的计数。
 *   每次启动时, 扫描找到最后一个非 0xFFFF 的半字作为上次计数值, 加1后写入下一个空位。
 *   利用 Flash 按半字编程的特性: 擦除后每个半字可独立写入一次, 无需每次擦除。
 *   一个扇区可记录 65536 次(128KB/2B), 擦除一次即可写入 65536 次, 大大延长 Flash 寿命。
 *   当写满时(所有半字均非 0xFFFF), 擦除整个扇区并从 0 重新开始。
 */
#include "bootCount.h"

uint32_t BOOTCOUNT_Get(void)
{
    const uint16_t *p = (const uint16_t *)BOOTCOUNT_FLASH_ADDR;
    uint32_t idx = 0;

    /* 扫描找到最后一个非 0xFFFF 的半字位置 */
    while ((idx < BOOTCOUNT_MAX_IDX) && (p[idx] != 0xFFFFU))
    {
        idx++;
    }

    uint16_t count = (idx == 0) ? 0 : p[idx - 1];
    count++;

    HAL_FLASH_Unlock();

    if (idx < BOOTCOUNT_MAX_IDX)
    {
        /* 扇区未写满, 直接编程下一个半字 */
        (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                BOOTCOUNT_FLASH_ADDR + idx * 2, count);
    }
    else
    {
        /* 扇区写满, 擦除后从起始位置重写 */
        FLASH_EraseInitTypeDef erase_cfg = {0};
        uint32_t erase_error = 0;

        erase_cfg.TypeErase    = FLASH_TYPEERASE_SECTORS;
        erase_cfg.Sector       = BOOTCOUNT_FLASH_SECTOR;
        erase_cfg.NbSectors    = 1;
        erase_cfg.VoltageRange = FLASH_VOLTAGE_RANGE_3;

        (void)HAL_FLASHEx_Erase(&erase_cfg, &erase_error);
        (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                BOOTCOUNT_FLASH_ADDR, count);
    }

    HAL_FLASH_Lock();
    return (uint32_t)count;
}
