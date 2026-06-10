/**
 * @file cover_art.h
 * @brief MP3 封面图片解码模块
 *
 * 本模块负责从 MP3 文件的 ID3v2 APIC 帧中提取 JPEG 封面图片,
 * 解码为 RGB565 格式并存入 CCM (Core Coupled Memory) 固定缓冲区。
 *
 * 解码采用两级策略:
 *   1. 首选 JPEGDEC 库 (支持 Baseline + Progressive)
 *   2. 失败后回退到 TJpgDec 库 (仅 Baseline, 兼容性更好)
 *
 * 封面图片经过 2:1 缩放后存入缓冲区 (60×60 像素),
 * LVGL 显示时通过 zoom=512 放大到 120×120 像素。
 */
#ifndef COVER_ART_H
#define COVER_ART_H

#include <stdint.h>

/* ==================================================================== */
/*                   尺寸常量                                             */
/* ==================================================================== */

/** @brief LVGL 显示宽度 (像素) */
#define COVER_W             120U

/** @brief LVGL 显示高度 (像素) */
#define COVER_H             120U

/** @brief 解码输出宽度 (像素), 2:1 缩小以节省 CCM */
#define DECODE_W            60U

/** @brief 解码输出高度 (像素) */
#define DECODE_H            60U

/** @brief 封面缓冲区大小: RGB565 格式, 每像素 2 字节 */
#define COVER_BUF_SIZE      (DECODE_W * DECODE_H * 2U)

/**
 * @brief 封面图缓冲区地址
 * CCM RAM 布局: LVGL 堆从 0x10000000 开始占 30KB,
 * 封面缓冲区紧跟在 LVGL 堆之后。
 */
#define COVER_BUF_ADDR      ((void *)(0x10000000U + 30U * 1024U))

/* ==================================================================== */
/*                   外部变量和函数声明                                    */
/* ==================================================================== */

struct File_MusicNode;

/** @brief 1=缓冲区中有有效封面数据 */
extern volatile uint8_t g_cover_valid;

/**
 * @brief 主解码器, 使用 JPEGDEC 库解码封面
 * @param node  当前歌曲节点
 * @return 0=成功, 非0=失败
 */
uint8_t cover_art_decode(struct File_MusicNode *node);

/**
 * @brief 后备解码器, 使用 TJpgDec 库解码封面
 * @param node  当前歌曲节点
 * @return 0=成功, 非0=失败
 */
uint8_t cover_art_decode_tjpgd(struct File_MusicNode *node);

#endif /* COVER_ART_H */
