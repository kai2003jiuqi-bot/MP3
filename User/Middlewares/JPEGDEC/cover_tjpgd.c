/**
 * @file cover_tjpgd.c
 * @brief 封面图片解码后备方案 — TJpgDec 实现
 *
 * ========== 设计意图 ==========
 *
 * 当 JPEGDEC（cover_jpegdec.c）解码失败时调用本模块。
 * TJpgDec 是 ChaN 开发的微型 JPEG 解码器，代码量小、RAM 占用低，
 * 但仅支持 Baseline JPEG（不支持 Progressive JPEG）。
 *
 * 调用链:
 *   cover_art_decode()
 *       ├── JPEGDEC 解码 (支持 Baseline + Progressive)
 *       └── 失败 → cover_art_decode_tjpgd() (仅 Baseline)
 *
 * ========== 内存管理 ==========
 *   TJpgDec 需要一个工作池 (pool), 最坏情况需要约 12.3KB
 *   (480×480 图像: 480×16 + 4096 + 512)。
 *   本实现分配 22KB 静态缓冲区, 留出充足余量。
 *   I/O 上下文使用静态变量 (而非动态分配), 简化内存管理。
 *
 * ========== 回调机制 ==========
 *   tjd_input()  — 从 MP3 文件读取 JPEG 数据
 *   tjd_output() — 将解码后的 MCU 写入 CCM 封面缓冲, 带边界裁剪
 *
 * TJpgDec 源码在 User/Middlewares/tjpgd/ 目录下。
 */
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "cover_art.h"
#include "file.h"
#include "ff.h"
#include "../tjpgd/tjpgd.h"

/* ======================== I/O 上下文 ======================== */

typedef struct {
    FIL      fil;        /* FatFs 文件对象 */
    uint32_t offset;     /* APIC 图片数据在 MP3 文件中的偏移 */
} tjpgd_io_t;

/*
 * TJpgDec 工作池:
 *   最坏情况 480×480: 480×16 + 4096 + 512 ≈ 12.3KB
 *   取 22KB 留足够余量应对更大尺寸的封面图
 */
#define TJPGD_POOL_SIZE (22U * 1024U)   /*!< TJpgDec 工作池大小 (约 22KB) */
static uint8_t s_pool[TJPGD_POOL_SIZE]; /*!< TJpgDec 工作池缓冲区 */
static tjpgd_io_t s_ctx;   /* 静态 I/O 上下文 (内含 FIL 的 512B 扇区缓冲) */

/* ======================== 回调函数 ======================== */

/*
 * 函数: tjd_input
 * 功能: TJpgDec 的输入数据回调
 *
 * 从 MP3 文件的 APIC 封面数据区读取/跳过指定长度的数据。
 *
 * @param jd  TJpgDec 解码器对象
 * @param buf 输出缓冲区 (NULL 表示跳过数据而非读取)
 * @param len 请求读取/跳过的字节数
 * @return    实际读取/跳过的字节数
 */
static unsigned int tjd_input(JDEC *jd, uint8_t *buf, unsigned int len)
{
    tjpgd_io_t *ctx = (tjpgd_io_t *)jd->device; /* 获取 I/O 上下文 */

    if (buf == NULL) {
        /* buf==NULL: 跳过 len 字节 (不读取到内存) */
        f_lseek(&ctx->fil, f_tell(&ctx->fil) + len);
        return len;
    }

    UINT br;                                    /* FatFs 读取返回字节数 */
    if (f_read(&ctx->fil, buf, len, &br) != FR_OK)
        return 0;
    return br;
}

/*
 * 函数: tjd_output
 * 功能: TJpgDec 的输出回调
 *
 * TJpgDec 解码完一个 MCU 后调用此回调,
 * 将解码后的 RGB565 像素数据写入 CCM 封面缓冲区的对应位置。
 *
 * 带边界裁剪: 超出 DECODE_W × DECODE_H 范围的像素被丢弃。
 *
 * @param jd      TJpgDec 解码器对象
 * @param bitmap  解码后的像素数据
 * @param rect    当前 MCU 的矩形区域范围
 * @return        1 = 继续解码, 0 = 中断解码
 */
static int tjd_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    uint16_t *dst = (uint16_t *)COVER_BUF_ADDR; /* 封面缓冲区目标地址 */
    uint16_t *src = (uint16_t *)bitmap;         /* 解码像素数据源指针 */

    int block_w = rect->right  - rect->left + 1;   /* 当前 MCU 块宽度 */
    int block_h = rect->bottom - rect->top  + 1;   /* 当前 MCU 块高度 */

    /*
     * 逐行逐像素复制, 带边界裁剪:
     *   计算目标坐标, 超出有效范围的像素直接跳过
     */
    for (int y = 0; y < block_h; y++) {
        int dst_y = rect->top + y;                  /* 目标行 Y 坐标 */
        if (dst_y < 0 || dst_y >= DECODE_H)
            continue;

        for (int x = 0; x < block_w; x++) {
            int dst_x = rect->left + x;             /* 目标列 X 坐标 */
            if (dst_x < 0 || dst_x >= DECODE_W)
                continue;

            dst[dst_y * DECODE_W + dst_x] = src[y * block_w + x];
        }
    }
    return 1;   /* 返回 1 继续解码 */
}

/* ======================== 公开 API ======================== */

/*
 * 函数: cover_art_decode_tjpgd
 * 功能: 使用 TJpgDec 解码 JPEG 封面图片
 *
 * 解码流程:
 *   1. 检查节点是否为 JPEG 类型
 *   2. 打开 MP3 文件, 定位到封面数据偏移
 *   3. 校验 JPEG SOI 标记 (0xFF 0xD8)
 *   4. 调用 jd_prepare() 准备解码器
 *   5. 选择合适的缩放比
 *   6. 调用 jd_decomp() 解码到 CCM 缓冲区
 *
 * @param node  歌曲节点
 * @return      1 = 解码成功, 0 = 失败
 */
uint8_t cover_art_decode_tjpgd(MusicNode_t *node)
{
    if (!node || node->img_type != IMG_JPEG || node->img_size == 0) {
        printf("tjpgd: skip (type=%u size=%lu)\r\n",
               node ? (unsigned)node->img_type : 0xff,
               node ? node->img_size : 0);
        return 0;
    }

    /* 打开 MP3 文件并定位到封面数据 */
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.offset = node->img_offset;

    if (f_open(&s_ctx.fil, node->unicode_name, FA_READ) != FR_OK) {
        printf("tjpgd: f_open fail\r\n");
        return 0;
    }
    f_lseek(&s_ctx.fil, node->img_offset);

    /* 校验 JPEG SOI (Start Of Image) 标记: 0xFF 0xD8 */
    {
        uint8_t soi[2];             /* JPEG SOI 标记 (0xFF 0xD8) */
        UINT br;                    /* FatFs 读取字节数 */
        f_read(&s_ctx.fil, soi, 2, &br);
        f_lseek(&s_ctx.fil, node->img_offset);
        if (soi[0] != 0xFF || soi[1] != 0xD8) {
            printf("tjpgd: no JPEG SOI\r\n");
            f_close(&s_ctx.fil);
            return 0;
        }
    }

    /* 准备 TJpgDec 解码器: jd_prepare 解析 JPEG 头, 分配工作区 */
    JDEC jdec;                                                              /* TJpgDec 解码器对象 */
    JRESULT res = jd_prepare(&jdec, tjd_input, s_pool, TJPGD_POOL_SIZE, &s_ctx); /* 初始化解码器 */
    if (res != JDR_OK) {
        printf("tjpgd: jd_prepare fail (%d)\r\n", res);
        f_close(&s_ctx.fil);
        return 0;
    }

    printf("tjpgd: JPEG %ux%u\r\n", jdec.width, jdec.height);

    /* 选择缩放比: 使解码输出 ≤ DECODE_W × DECODE_H */
    uint16_t w = jdec.width;
    uint16_t h = jdec.height;
    uint8_t scale = 0;              /* 缩放比 (0=原始/1=1/2/2=1/4/3=1/8) */

    if (w > DECODE_W || h > DECODE_H) {
        if ((w >> 1) <= DECODE_W && (h >> 1) <= DECODE_H)
            scale = 1;   /* 1:2 */
        else if ((w >> 2) <= DECODE_W && (h >> 2) <= DECODE_H)
            scale = 2;   /* 1:4 */
        else
            scale = 3;   /* 1:8 */
    }

    /* 清空缓冲区, 解码输出 */
    memset((void *)COVER_BUF_ADDR, 0, COVER_BUF_SIZE);

    /* 执行解码: jd_decomp 逐 MCU 解码并调用 tjd_output 输出 */
    res = jd_decomp(&jdec, tjd_output, scale);

    f_close(&s_ctx.fil);

    printf("tjpgd: decode %s (res=%d)\r\n",
           (res == JDR_OK) ? "OK" : "FAIL", res);

    return (res == JDR_OK) ? 1 : 0;
}
