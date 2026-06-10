/**
 * @file bootCount.c
 * @brief 启动次数记录模块 (基于 STM32 内部 Flash)
 *
 * ========== 设计意图 ==========
 *
 * 记录设备累计启动次数, 用于调试和生产测试验证:
 *   - 验证 Flash 读写功能是否正常
 *   - 提供设备使用次数统计
 *
 * ========== 磨损均衡策略 ==========
 *
 * Flash 的擦除寿命有限 (~10 万次), 如果每次启动都擦除重写同一个位置,
 * 很快就会耗尽寿命。本模块采用"顺序写入"策略:
 *
 *   在 Flash 中划出一个扇区 (128KB), 按半字 (2 字节) 顺序写入递增的计数值。
 *   每次启动时:
 *     1. 扫描找到最后一个非 0xFFFF 的半字位置
 *     2. 该值即为上次记录的开机次数
 *     3. 加 1 后写入下一个空位
 *
 *   利用 Flash 的特性: 擦除后每个半字只能写入一次, 但不需要每次都擦除。
 *   一个扇区可记录:
 *     128KB / 2B = 65536 次
 *   按每天开关机 10 次计算, 一个扇区可用约 18 年。
 *
 *   当扇区写满时 (所有半字均非 0xFFFF), 擦除整个扇区并从 0 重新开始。
 *
 * ========== 存储布局 ==========
 *
 *   Flash 地址: BOOTCOUNT_FLASH_ADDR
 *   扇区大小:  128KB
 *   存储格式:  word[0], word[1], word[2], ..., word[65535]
 *             每个 word 是 2 字节的非递减小整数
 *
 *   空扇区状态: 全是 0xFFFF
 *   第 N 次写入: word[N-1] = N (第一次写入 word[0]=1, 第二次 word[1]=2, ...)
 */
#include "bootCount.h"

/*
 * 函数: BOOTCOUNT_Get
 * 功能: 记录一次启动并返回累计启动次数
 *
 * 工作流程:
 *   1. 扫描 Flash 扇区, 找到最后一个非 0xFFFF 的半字 → 上次计数值
 *   2. 计数值 +1
 *   3. 如果扇区还有空位, 直接编程下一个半字
 *   4. 如果扇区已写满, 擦除整个扇区后从起始位置重写
 *   5. 返回当前计数值
 *
 * @return 累计启动次数
 */
uint32_t BOOTCOUNT_Get(void)
{
    const uint16_t *p = (const uint16_t *)BOOTCOUNT_FLASH_ADDR;
    uint32_t idx = 0;

    /* 扫描找到最后一个非 0xFFFF 的半字位置 */
    while ((idx < BOOTCOUNT_MAX_IDX) && (p[idx] != 0xFFFFU))
    {
        idx++;
    }

    /*
     * 计算新的计数值:
     *   如果 idx == 0: 扇区全空, 从未记录过 → count = 1
     *   如果 idx > 0:  上次计数值 = p[idx-1], 加 1
     */
    uint16_t count = (idx == 0) ? 0 : p[idx - 1];
    count++;

    HAL_FLASH_Unlock();

    if (idx < BOOTCOUNT_MAX_IDX)
    {
        /* 扇区未写满: 直接编程下一个半字 */
        (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                BOOTCOUNT_FLASH_ADDR + idx * 2, count);
    }
    else
    {
        /*
         * 扇区已写满: 擦除整个扇区后重写
         * 擦除后 Flash 所有位变 1 (0xFFFF), 然后写入当前计数值到第一个位置
         */
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
