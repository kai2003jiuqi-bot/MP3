/**
 * 封面图片解码：用 JPEGDEC 库解码 JPEG 封面（支持 Baseline + Progressive），
 * 输出 RGB565 到 CCM 固定缓冲区。
 *
 * JPEGDEC 库本文件内联包含（不单独编译 JPEGDEC.cpp），
 * JPEG_STATIC 在 C 模式下为空，故 JPEGInit / DecodeJPEG 在此编译单元内均可见。
 */
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "cover_art.h"
#include "file.h"
#include "ff.h"

#include "JPEGDEC.h"

/* 封面有效标志（cover_art.h 中 extern） */
volatile uint8_t g_cover_valid = 0;     /*!< 封面数据有效标志，1=缓冲区有有效封面 */

/* jpeg.inl 内部的静态函数前向声明（本文件末尾 #include 实现） */
static int JPEGInit(JPEGIMAGE *pJPEG);
static int DecodeJPEG(JPEGIMAGE *pJPEG);

/* ======================== 文件 I/O 回调 ======================== */

/* IO 上下文：传递 FatFs 句柄和 MP3 文件内偏移 */
typedef struct {
    FIL      fil;       /* FatFs 文件对象 */
    uint32_t offset;    /* APIC 图片数据在 MP3 文件中的偏移 */
} cover_io_t;

/* 读回调：从当前 iPos 读取数据 */
static int32_t cover_read(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    cover_io_t *ctx = (cover_io_t *)pFile->fHandle; /* I/O 上下文指针 */
    int32_t iBytesRead = iLen;                      /* 实际可读字节数 */
    if ((pFile->iSize - pFile->iPos) < iLen)
        iBytesRead = pFile->iSize - pFile->iPos;
    if (iBytesRead <= 0) return 0;
    UINT br;                                        /* FatFs 读取返回字节数 */
    f_read(&ctx->fil, pBuf, (UINT)iBytesRead, &br);
    pFile->iPos += (int32_t)br;
    return (int32_t)br;
}

/* 寻址回调：定位到 JPEG 数据流中绝对位置（相对于封面数据起始） */
static int32_t cover_seek(JPEGFILE *pFile, int32_t iPosition)
{
    cover_io_t *ctx = (cover_io_t *)pFile->fHandle; /* I/O 上下文指针 */
    if (iPosition < 0) iPosition = 0;
    else if (iPosition >= pFile->iSize) iPosition = pFile->iSize - 1;
    pFile->iPos = iPosition;
    f_lseek(&ctx->fil, ctx->offset + (uint32_t)iPosition);
    return iPosition;
}

/* 关闭回调 */
static void cover_close(void *pHandle)
{
    cover_io_t *ctx = (cover_io_t *)pHandle; /* I/O 上下文指针 */
    f_close(&ctx->fil);
    vPortFree(ctx);
}

/* ======================== 绘制回调 ======================== */

/* 输出回调：将解码后的 MCU 块写入 CCM 封面缓冲，带边界裁剪 */
static int cover_draw(JPEGDRAW *pDraw)
{
    uint16_t *dst = (uint16_t *)COVER_BUF_ADDR;     /* 封面缓冲区目标地址 */
    uint16_t *src = pDraw->pPixels;                 /* 解码像素数据源指针 */
    int copy_w = (pDraw->iWidthUsed > 0 && pDraw->iWidthUsed < pDraw->iWidth)
                     ? pDraw->iWidthUsed : pDraw->iWidth;  /* 本行有效像素宽度 */

    for (int y = 0; y < pDraw->iHeight; y++) {
        int dst_y = pDraw->y + y;                           /* 目标行 Y 坐标 */
        if (dst_y >= DECODE_H || dst_y < 0) continue;
        int dst_x = pDraw->x;                               /* 目标起始 X 坐标 */
        if (dst_x >= DECODE_W) continue;
        int draw_w = copy_w;                                /* 实际绘制宽度（含边界裁剪） */
        if (dst_x + draw_w > DECODE_W)
            draw_w = DECODE_W - dst_x;
        if (draw_w <= 0) continue;
        memcpy(dst + dst_y * DECODE_W + dst_x, src, (unsigned int)draw_w * 2);
        src += pDraw->iWidth;
    }
    return 1; /* 继续解码 */
}

/* ======================== 公开 API ======================== */

uint8_t cover_art_decode(MusicNode_t *node)
{
    g_cover_valid = 0;

    if (!node || node->img_type != IMG_JPEG || node->img_size == 0) {
        printf("cover: skip (type=%u size=%lu)\r\n",
               node ? (unsigned)node->img_type : 0xff,
               node ? node->img_size : 0);
        return 0;
    }

    /* 使用静态 JPEGIMAGE（≈13KB，放在 SRAM BSS 而非 FreeRTOS 堆，
       避免堆空间不足导致分配失败） */
    static JPEGIMAGE s_jpeg;
    JPEGIMAGE *jpeg = &s_jpeg;                     /* JPEG 解码器指针 */
    memset(jpeg, 0, sizeof(JPEGIMAGE));

    /* 分配 IO 上下文（内置 FIL，含 512B 扇区缓冲） */
    cover_io_t *ctx = (cover_io_t *)pvPortMalloc(sizeof(cover_io_t)); /* I/O 上下文指针 */
    if (!ctx) {
        printf("cover: pvPortMalloc ctx fail\r\n");
        return 0;
    }
    ctx->offset = node->img_offset;

    /* 打开 MP3 文件并定位到封面数据 */
    if (f_open(&ctx->fil, node->unicode_name, FA_READ) != FR_OK) {
        printf("cover: f_open fail\r\n");
        vPortFree(ctx);
        return 0;
    }
    f_lseek(&ctx->fil, node->img_offset);
    printf("cover: opened, img_offset=%lu img_size=%lu\r\n",
           node->img_offset, node->img_size);

    /* 校验 JPEG SOI 标记 */
    {
        uint8_t soi[2];             /* JPEG SOI 标记 (0xFF 0xD8) */
        UINT br;                    /* FatFs 读取字节数 */
        f_read(&ctx->fil, soi, 2, &br);
        printf("cover: first 2 bytes = 0x%02X 0x%02X\r\n", soi[0], soi[1]);
        f_lseek(&ctx->fil, node->img_offset); /* 倒回 */
    }

    /* 配置 JPEGIMAGE 结构体 = 相当于 C++ 版 open(void*, ...) */
    jpeg->pfnRead            = cover_read;
    jpeg->pfnSeek            = cover_seek;
    jpeg->pfnDraw            = cover_draw;
    jpeg->pfnClose           = cover_close;
    jpeg->iMaxMCUs           = 1000;
    jpeg->JPEGFile.iSize     = (int32_t)node->img_size;
    jpeg->JPEGFile.fHandle   = ctx;
    jpeg->ucPixelType        = RGB565_LITTLE_ENDIAN;

    /* 初始化解码器（解析 JPEG 头信息） */
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

    /* 选择缩放比：输出 ≤ DECODE_W × DECODE_H */
    int options = 0;                            /* 缩放选项 (0=原始/1=1/2/2=1/4/3=1/8) */
    uint16_t w = (uint16_t)jpeg->iWidth;        /* 图片原始宽度 */
    uint16_t h = (uint16_t)jpeg->iHeight;       /* 图片原始高度 */

    if (w > DECODE_W || h > DECODE_H) {
        if ((w >> 1) <= DECODE_W && (h >> 1) <= DECODE_H)
            options = JPEG_SCALE_HALF;
        else if ((w >> 2) <= DECODE_W && (h >> 2) <= DECODE_H)
            options = JPEG_SCALE_QUARTER;
        else
            options = JPEG_SCALE_EIGHTH;
    }

    /* JPEGDEC progressive 在 1/4 缩放会产生黑格，强制降到 1/8 */
    if ((jpeg->ucMode == 0xC2) && options > JPEG_SCALE_EIGHTH) {
        printf("cover: progressive JPEG, forcing EIGHTH\r\n");
        options = JPEG_SCALE_EIGHTH;
    }

    /* 解码直接输出到 0,0，LVGL 用 zoom=512 放大到 120×120 */
    memset((void *)COVER_BUF_ADDR, 0, COVER_BUF_SIZE);
    jpeg->iXOffset = 0;
    jpeg->iYOffset = 0;
    jpeg->iOptions = options;
    int res = DecodeJPEG(jpeg);                 /* 解码结果 (1=成功/0=失败) */

    g_cover_valid = (res == 1);
    printf("cover: decode %s (res=%d)\r\n", g_cover_valid ? "OK" : "FAIL", res);

    /* 失败时重试：强制 1/8 缩放再解一次 */
    if (!g_cover_valid && options != JPEG_SCALE_EIGHTH) {
        printf("cover: retry with EIGHTH...\r\n");
        /* 注意：ctx 和 ctx->fil 此时仍然有效，只需倒回文件指针即可重来 */
        f_lseek(&ctx->fil, node->img_offset);
        jpeg->JPEGFile.iPos = 0;
        memset((void *)COVER_BUF_ADDR, 0, COVER_BUF_SIZE);
        jpeg->iXOffset = 0; jpeg->iYOffset = 0;
        jpeg->iOptions = JPEG_SCALE_EIGHTH;
        int res8 = DecodeJPEG(jpeg);            /* 1/8 缩放重试解码结果 */
        g_cover_valid = (res8 == 1);
        printf("cover: EIGHTH retry %s (res=%d)\r\n",
               g_cover_valid ? "OK" : "FAIL", res8);
    }

    /* 清理：关闭文件并释放 IO 上下文 */
    f_close(&ctx->fil);
    vPortFree(ctx);

    /* JPEGDEC 全部失败时，用 TJpgDec 做最终尝试 */
    if (!g_cover_valid) {
        printf("cover: JPEGDEC failed, trying TJpgDec...\r\n");
        g_cover_valid = cover_art_decode_tjpgd(node);
    }

    return g_cover_valid;
}

/* 包含 JPEGDEC 库完整实现（JPEG_STATIC 在 C 模式下为空，故链接正常） */
#include "jpeg.inl"
