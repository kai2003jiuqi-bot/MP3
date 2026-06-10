/**
 * @file bootCount.h
 * @brief 启动次数记录模块头文件
 *
 * 基于 STM32F4 内部 Flash 扇区 7(地址 0x08060000, 128KB),
 * 按半字顺序写入实现磨损均衡, 最多记录 65536 次启动。
 */
#ifndef BOOTCOUNT_H
#define BOOTCOUNT_H

#include <stdint.h>
#include "stm32f4xx_hal.h"

/** @brief Flash 中存储启动计数的基地址 (扇区7) */
#define BOOTCOUNT_FLASH_ADDR    0x08060000UL

/** @brief 使用的 Flash 扇区编号 */
#define BOOTCOUNT_FLASH_SECTOR  FLASH_SECTOR_7

/** @brief 扇区可容纳的半字数量: 128KB / 2 = 65536 */
#define BOOTCOUNT_MAX_IDX       65536UL

/**
 * @brief 获取系统启动次数 (内部 Flash 磨损均衡存储)
 * @return 当前启动计数值 (从 1 开始递增)
 * @note   每次调用自动递增计数值并写入 Flash。
 *         磨损均衡算法: 在扇区内顺序写入半字, 写满后擦除整个扇区重新开始。
 */
uint32_t BOOTCOUNT_Get(void);

#endif /* BOOTCOUNT_H */
