/**
 * @file cover_jpegdec.c
 * @brief 封面图片解码: 使用 JPEGDEC 库解码 JPEG 封面 (支持 Baseline + Progressive)
 *
 * ========== 功能说明 ==========
 *
 * 本模块实现从 MP3 文件的 ID3v2 标签中提取 APIC 帧的 JPEG 封面数据,
 * 使用 JPEGDEC 库解码为 RGB565 格式, 输出到 CCM (Core Coupled Memory)
 * 中的固定缓冲区 (地址由 COVER_BUF_ADDR 定义), 供 LVGL 显示。
 *
 * ========== 解码架构 ==========
 *
 *   cover_art_decode()   ← 公开 API, 在播放任务中调用
 *        │
 *        ├── JPEGDEC 解码 (支持 Baseline + Progressive JPEG)
 *        │      ├── cover_read()    — 从 MP3 文件读数据 (FatFs I/O 回调)
 *        │      ├── cover_seek()    — 在 JPEG 数据流中定位
 *        │      ├── cover_draw()    — 接收解码后的 MCU, 写入 CCM 缓冲
 *        │      └── cover_close()   — 清理资源
 *        │
 *        └── 失败后调用 cover_art_decode_tjpgd() → TJpgDec 后备方案
 *
 * ========== 缩放策略 ==========
 *   解码时根据封面原始尺寸选择缩放比:
 *     - 原始尺寸 ≤ DECODE_W × DECODE_H: 不缩放
 *     - 1/2 缩放可满足:      使用 HALF
 *     - 1/4 缩放可满足:      使用 QUARTER
 *     - 否则:                使用 EIGHTH
 *
 *   Progressive JPEG 在 1/4 缩放可能产生黑块, 强制降到 1/8。
 *   解码失败时自动用 1/8 重试一次。
 *
 * ========== 内存管理 ==========
 *   - JPEGIMAGE 结构体 (~13KB) 使用静态变量 (SRAM BSS), 避免 FreeRTOS 堆碎片
 *   - I/O 上下文 (含 512B 扇区缓冲的 FIL) 使用 pvPortMalloc 动态分配
 *   - 解码输出直接写入 COVER_BUF_ADDR (CCM RAM, 地址和大小由 cover_art.h 定义)
 *
 * @note JPEGDEC 库通过本文件末尾 #include "jpeg.inl" 内联包含,
 *       JPEG_STATIC 在 C 模式下为空, 故 JPEGInit / DecodeJPEG 在此编译单元内均可见
 */
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "cover_art.h"
#include "file.h"
#include "ff.h"

#include "JPEGDEC.h"

/* 封面有效标志 (cover_art.h 中 extern) */
volatile uint8_t g_cover_valid = 0;     /*!< 封面数据有效标志, 1=缓冲区有有效封面 */

/* jpeg.inl 内部的静态函数前向声明 (本文件末尾 #include 实现) */
static int JPEGInit(JPEGIMAGE *pJPEG);
static int DecodeJPEG(JPEGIMAGE *pJPEG);

/* ======================== 文件 I/O 回调 ======================== */

/*
 * IO 上下文:
 *   在解码过程中传递 FatFs 文件句柄和 MP3 文件内 APIC 图片数据的偏移量
 */
typedef struct {
    FIL      fil;       /* FatFs 文件对象 */
    uint32_t offset;    /* APIC 图片数据在 MP3 文件中的偏移 */
} cover_io_t;

/*
 * 函数: cover_read
 * 功能: JPEGDEC 库的读数据回调
 *
 * 从 MP3 文件中读取指定长度的 JPEG 数据到缓冲区。
 * 自动处理文件末尾截断, 不读取超出图像数据范围。
 *
 * @param pFile JPEGFILE 结构体, 包含文件大小、当前读取位置等信息
 * @param pBuf  输出缓冲区
 * @param iLen  请求读取的字节数
 * @return      实际读取的字节数
 */
static int32_t cover_read(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    cover_io_t *ctx = (cover_io_t *)pFile->fHandle; /* 获取 I/O 上下文 */
    int32_t iBytesRead = iLen;                      /* 预期读取长度 */
    /* 如果请求位置超出文件大小, 截断到文件末尾 */
    if ((pFile->iSize - pFile->iPos) < iLen)
        iBytesRead = pFile->iSize - pFile->iPos;
    if (iBytesRead <= 0) return 0;
    UINT br;                                        /* FatFs 返回的已读字节数 */
    f_read(&ctx->fil, pBuf, (UINT)iBytesRead, &br);
    pFile->iPos += (int32_t)br;                     /* 更新 JPEGFILE 中的读取位置 */
    return (int32_t)br;
}

/*
 * 函数: cover_seek
 * 功能: JPEGDEC 库的定位回调
 *
 * 定位到 JPEG 数据流中的指定位置 (相对于封面数据的起始处)。
 * 同时移动 FatFs 文件指针到 MP3 文件中的对应绝对值位置。
 *
 * @param pFile     JPEGFILE 结构体
 * @param iPosition 目标位置 (相对封面数据起始, 0 = 第一个字节)
 * @return          定位后的位置
 */
static int32_t cover_seek(JPEGFILE *pFile, int32_t iPosition)
{
    cover_io_t *ctx = (cover_io_t *)pFile->fHandle;
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize - 1;
    pFile->iPos = iPosition;
    f_lseek(&ctx->fil, ctx->offset + (uint32_t)iPosition);
    return iPosition;
}

/* 关闭回调: 关闭文件并释放 I/O 上下文 */
static void cover_close(void *pHandle)
{
    cover_io_t *ctx = (cover_io_t *)pHandle;
    f_close(&ctx->fil);
    vPortFree(ctx);
}

/* ======================== 绘制回调 ======================== */

/*
 * 函数: cover_draw
 * 功能: JPEGDEC 库的输出回调
 *
 * JPEGDEC 解码完一个 MCU (最小编码单元) 后调用此回调,
 * 将解码后的 RGB565 像素数据写入 CCM 封面缓冲区的对应位置。
 *
 * 边界裁剪: 确保像素只写入 DECODE_W × DECODE_H 的有效范围内。
 *
 * @param pDraw  绘制信息, 包含解码后的像素数据、矩形区域坐标等
 * @return       1 = 继续解码, 0 = 中断解码
 */
static int cover_draw(JPEGDRAW *pDraw)
{
    uint16_t *dst = (uint16_t *)COVER_BUF_ADDR;     /* 封面缓冲区目标地址 */
    uint16_t *src = pDraw->pPixels;                 /* 解码像素数据源指针 */
    /*
     * 有效宽度:
     *   如果 JPEG 宽度因缩放不能整除, iWidthUsed 表示实际有效像素宽度
     *   否则使用全宽度
     */
    int copy_w = (pDraw->iWidthUsed > 0 && pDraw->iWidthUsed < pDraw->iWidth)
                     ? pDraw->iWidthUsed : pDraw->iWidth;

    /*
     * 逐行复制, 带边界裁剪:
     *   - 超出 DECODE_H 的行跳过
     *   - 超出 DECODE_W 的列截断
     */
    for (int y = 0; y < pDraw->iHeight; y++) {
        int dst_y = pDraw->y + y;                   /* 目标行坐标 */
        if (dst_y >= DECODE_H || dst_y < 0) continue;
        int dst_x = pDraw->x;                       /* 目标起始列坐标 */
        if (dst_x >= DECODE_W) continue;
        int draw_w = copy_w;                        /* 实际绘制宽度 */
        if (dst_x + draw_w > DECODE_W)
            draw_w = DECODE_W - dst_x;
        if (draw_w <= 0) continue;
        memcpy(dst + dst_y * DECODE_W + dst_x, src, (unsigned int)draw_w * 2);
        src += pDraw->iWidth;   /* 源数据每行按完整宽度对齐, 跳过填充空白 */
    }
    return 1; /* 返回 1 继续解码 */
}

/* ======================== 公开 API ======================== */

/*
 * 函数: cover_art_decode
 * 功能: 解码指定歌曲节点的封面图片
 *
 * 解码流程:
 *   1. 检查节点是否为 JPEG 封面类型
 *   2. 打开 MP3 文件, 定位到 APIC 图片数据偏移
 *   3. 初始化 JPEGDEC 解码器
 *   4. 选择合适的缩放比
 *   5. 解码输出到 CCM 缓冲区
 *   6. 失败则重试 (强制 1/8 缩放)
 *   7. 如果 JPEGDEC 全部失败, 调用 TJpgDec 后备方案
 *   8. 设置 g_cover_valid 标志
 *
 * @param node  歌曲节点 (需包含 img_type/img_offset/img_size)
 * @return      1 = 解码成功, 0 = 失败
 */
uint8_t cover_art_decode(MusicNode_t *node)
{
    g_cover_valid = 0;

    /* 仅处理 JPEG 封面 (暂不支持 PNG) */
    if (!node || node->img_type != IMG_JPEG || node->img_size == 0) {
        printf("cover: skip (type=%u size=%lu)\r\n",
               node ? (unsigned)node->img_type : 0xff,
               node ? node->img_size : 0);
        return 0;
    }

    /*
     * 使用静态 JPEGIMAGE (≈13KB，放在 SRAM BSS 而非 FreeRTOS 堆，
     * 避免堆空间不足导致分配失败)
     */
    static JPEGIMAGE s_jpeg;
    JPEGIMAGE *jpeg = &s_jpeg;                     /* JPEG 解码器指针 */
    memset(jpeg, 0, sizeof(JPEGIMAGE));

    /*
     * 分配 I/O 上下文 (内含 FIL 对象, 含 512B 扇区缓冲)。
     * 使用 pvPortMalloc 动态分配, 解码完成后释放。
     */
    cover_io_t *ctx = (cover_io_t *)pvPortMalloc(sizeof(cover_io_t));
    if (!ctx) {
        printf("cover: pvPortMalloc ctx fail\r\n");
        return 0;
    }
    ctx->offset = node->img_offset;

    /* 打开 MP3 文件并定位到封面数据偏移处 */
    if (f_open(&ctx->fil, node->unicode_name, FA_READ) != FR_OK) {
        printf("cover: f_open fail\r\n");
        vPortFree(ctx);
        return 0;
    }
    f_lseek(&ctx->fil, node->img_offset);
    printf("cover: opened, img_offset=%lu img_size=%lu\r\n",
           node->img_offset, node->img_size);

    /* 校验 JPEG SOI (Start Of Image) 标记: 0xFF 0xD8 */
    {
        uint8_t soi[2];             /* JPEG SOI 标记 (0xFF 0xD8) */
        UINT br;                    /* FatFs 读取字节数 */
        f_read(&ctx->fil, soi, 2, &br);
        printf("cover: first 2 bytes = 0x%02X 0x%02X\r\n", soi[0], soi[1]);
        f_lseek(&ctx->fil, node->img_offset); /* 读完后倒回 */
    }

    /*
     * 配置 JPEGIMAGE 结构体:
     *   这相当于 C++ 版的 open(void*, ...) 调用
     */
    jpeg->pfnRead            = cover_read;
    jpeg->pfnSeek            = cover_seek;
    jpeg->pfnDraw            = cover_draw;
    jpeg->pfnClose           = cover_close;
    jpeg->iMaxMCUs           = 1000;      /* 最大 MCU 数限制 */
    jpeg->JPEGFile.iSize     = (int32_t)node->img_size;  /* JPEG 数据总大小 */
    jpeg->JPEGFile.fHandle   = ctx;       /* I/O 上下文 */
    jpeg->ucPixelType        = RGB565_LITTLE_ENDIAN;  /* 输出格式: RGB565 小端 */

    /* 初始化解码器 (解析 JPEG 头信息) */
    printf("cover: calling JPEGInit...\r\n");
    int jinit_ok = JPEGInit(jpeg);
    printf("cover: JPEGInit returned %d\r\n", jinit_ok);
    if (!jinit_ok) {
        printf("cover: JPEGInit fail, iError=%d\r\n", jpeg->iError);
        f_close(&ctx->fil);
        vPortFree(ctx);
        return 0;
    }

    printf("cover: JPEG %ux%u mode=%s\r\n",
           jpeg->iWidth, jpeg->iHeight,
           (jpeg->ucMode == 0xC2) ? "progressive" : "baseline");

    /*
     * 选择缩放比: 使解码输出 ≤ DECODE_W × DECODE_H
     *   0 = 原始, 1 = 1/2, 2 = 1/4, 3 = 1/8
     */
    int options = 0;
    uint16_t w = (uint16_t)jpeg->iWidth;
    uint16_t h = (uint16_t)jpeg->iHeight;

    if (w > DECODE_W || h > DECODE_H) {
        if ((w >> 1) <= DECODE_W && (h >> 1) <= DECODE_H)
            options = JPEG_SCALE_HALF;
        else if ((w >> 2) <= DECODE_W && (h >> 2) <= DECODE_H)
            options = JPEG_SCALE_QUARTER;
        else
            options = JPEG_SCALE_EIGHTH;
    }

    /*
     * JPEGDEC progressive JPEG 在 1/4 缩放时会产生黑块,
     * 强制降到 1/8 以避免解码异常
     */
    if ((jpeg->ucMode == 0xC2) && options > JPEG_SCALE_EIGHTH) {
        printf("cover: progressive JPEG, forcing EIGHTH\r\n");
        options = JPEG_SCALE_EIGHTH;
    }

    /* 清空缓冲区, 解码输出到 (0,0) 位置 */
    memset((void *)COVER_BUF_ADDR, 0, COVER_BUF_SIZE);
    jpeg->iXOffset = 0;
    jpeg->iYOffset = 0;
    jpeg->iOptions = options;
    int res = DecodeJPEG(jpeg);                 /* 解码结果: 1=成功, 0=失败 */

    g_cover_valid = (res == 1);
    printf("cover: decode %s (res=%d)\r\n", g_cover_valid ? "OK" : "FAIL", res);

    /* 解码失败时: 强制 1/8 缩放重试一次 */
    if (!g_cover_valid && options != JPEG_SCALE_EIGHTH) {
        printf("cover: retry with EIGHTH...\r\n");
        f_lseek(&ctx->fil, node->img_offset);  /* 倒回文件指针 */
        jpeg->JPEGFile.iPos = 0;
        memset((void *)COVER_BUF_ADDR, 0, COVER_BUF_SIZE);
        jpeg->iXOffset = 0; jpeg->iYOffset = 0;
        jpeg->iOptions = JPEG_SCALE_EIGHTH;
        int res8 = DecodeJPEG(jpeg);            /* 1/8 重试解码结果 */
        g_cover_valid = (res8 == 1);
        printf("cover: EIGHTH retry %s (res=%d)\r\n",
               g_cover_valid ? "OK" : "FAIL", res8);
    }

    /* 清理: 关闭文件并释放 I/O 上下文 */
    f_close(&ctx->fil);
    vPortFree(ctx);

    /*
     * JPEGDEC 全部失败时, 调用 TJpgDec 做最终尝试:
     * TJpgDec 是 ChaN 开发的微型解码器, 仅支持 Baseline JPEG
     */
    if (!g_cover_valid) {
        printf("cover: JPEGDEC failed, trying TJpgDec...\r\n");
        g_cover_valid = cover_art_decode_tjpgd(node);
    }

    return g_cover_valid;
}

/*
 * 包含 JPEGDEC 库完整实现:
 *   jpeg.inl 是 JPEGDEC 库的所有源文件内联体,
 *   JPEG_STATIC 在 C 模式下为空, 故链接正常。
 */
#include "jpeg.inl"
