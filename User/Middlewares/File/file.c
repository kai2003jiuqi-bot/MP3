/**
 * @file file.c
 * @brief 文件系统管理, MP3文件扫描, ID3v2标签解析(APIC封面), 播放列表保存/恢复到Flash
 *
 * 功能概述:
 *   - 初始化 SD 卡 FATFS 和外部 Flash (W25Q64) FATFS 文件系统
 *   - 扫描 SD 卡根目录下的 MP3/WAV 文件, 构建双向循环链表
 *   - 解析 ID3v2 标签中的 APIC 帧, 提取封面图片在文件中的偏移和大小
 *   - 将播放列表序列化保存到外部 Flash, 下次开机时恢复
 */
#include "file.h"
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "w25q64.h"
#include "ff_gen_drv.h"
#include "TCHAR_Sring.h"

/* 外部依赖 */
extern Disk_drvTypeDef disk;

/* ==================================================================== */
/*                   全局变量定义                                         */
/* ==================================================================== */

uint32_t          g_file_count            = 0;
File_MusicNode_t *g_file_music_head       = NULL;
File_MusicNode_t *g_file_current_music    = NULL;

FATFS g_file_fs;         /**< SD 卡文件系统对象 */
FATFS g_file_fs_flash;   /**< 外部 Flash 文件系统对象 */
FIL   g_file_font;       /**< 字库文件对象 */

/* ==================================================================== */
/*                   内部函数声明                                         */
/* ==================================================================== */

static void      File_ScanMP3(void);
static void      File_DetectCoverInfo(File_MusicNode_t *node);

/* ==================================================================== */
/*                   文件系统初始化                                       */
/* ==================================================================== */

void File_Init(void)
{
    FRESULT res;

    /* 挂载 SD 卡 */
    res = f_mount(&g_file_fs, _T(""), 1);
    if (res != FR_OK)
    {
        printf("mount sd card error: %d\r\n", res);
        printf("enter while\r\n");
        while (1) { }
    }
    else
    {
        printf("mount sd card success\r\n");
    }

    /* 挂载外部 Flash */
    res = f_mount(&g_file_fs_flash, _T("1:"), 1);
    if (res != FR_OK)
    {
        printf("mount external flash error: %d\r\n", res);
        printf("enter while\r\n");
        while (1) { }
    }
    else
    {
        printf("mount external flash success\r\n");
    }

    /* 打开字库文件 */
    res = f_open(&g_file_font, _T("1:my_font_song_14.bin"), FA_READ);
    if (res != FR_OK)
    {
        printf("open font file error: %d\r\n", res);
        printf("enter while\r\n");
        while (1) { }
    }
    else
    {
        printf("open font file success\r\n");
    }

    vTaskDelay(500);
    MX_USB_DEVICE_Init();

    File_ScanMP3();
}

/* ==================================================================== */
/*                   ID3v2 封面信息解析                                   */
/* ==================================================================== */

static void File_DetectCoverInfo(File_MusicNode_t *node)
{
    FIL file;
    uint8_t header[10];
    UINT br;

    node->img_type   = FILE_IMG_NONE;
    node->img_offset = 0;
    node->img_size   = 0;

    if (f_open(&file, node->unicode_name, FA_READ) != FR_OK) return;

    /* 读取 ID3v2 头 */
    if (f_read(&file, header, 10, &br) != FR_OK || br < 10)
    {
        f_close(&file);
        return;
    }

    /* 检查 ID3 签名 */
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3')
    {
        f_close(&file);
        return;
    }

    /* 解析 ID3v2 标签大小 (synchsafe integer) */
    uint32_t tag_size = ((uint32_t)header[6] << 21)
                      | ((uint32_t)header[7] << 14)
                      | ((uint32_t)header[8] << 7)
                      |  (uint32_t)header[9];

    /* 遍历帧, 查找 APIC */
    uint32_t pos = 10;
    uint8_t frame_hdr[10];

    while (pos + 10 <= 10 + tag_size)
    {
        f_lseek(&file, pos);
        if (f_read(&file, frame_hdr, 10, &br) != FR_OK || br < 10) break;

        if (frame_hdr[0] == 0x00 || frame_hdr[0] == 0xFF) break;

        uint32_t frame_size = ((uint32_t)frame_hdr[4] << 24)
                            | ((uint32_t)frame_hdr[5] << 16)
                            | ((uint32_t)frame_hdr[6] << 8)
                            |  (uint32_t)frame_hdr[7];
        if (frame_size < 2 || frame_size > tag_size) break;

        if (frame_hdr[0] == 'A' && frame_hdr[1] == 'P' &&
            frame_hdr[2] == 'I' && frame_hdr[3] == 'C')
        {
            uint32_t end = pos + 10 + frame_size;
            uint32_t off = pos + 10;

            /* 1. 文字编码 (1 byte) */
            uint8_t encoding;
            f_lseek(&file, off);
            f_read(&file, &encoding, 1, &br);
            off++;

            /* 2. MIME 类型 (null-terminated) */
            {
                uint8_t b;
                do {
                    if (off >= end) break;
                    f_lseek(&file, off);
                    f_read(&file, &b, 1, &br);
                    off++;
                } while (b != 0);
            }

            /* 检查 MIME 类型 */
            {
                uint8_t mime_buf[32];
                uint32_t mime_len = 0;
                f_lseek(&file, pos + 11);
                do {
                    if (mime_len >= sizeof(mime_buf) - 1) break;
                    f_read(&file, &mime_buf[mime_len], 1, &br);
                } while (mime_buf[mime_len++] != 0 && off < end);

                if (strstr((const char *)mime_buf, "jpeg") ||
                    strstr((const char *)mime_buf, "jpg"))
                {
                    node->img_type = FILE_IMG_JPEG;
                    printf("cover: JPEG found, mime=%s\r\n", mime_buf);
                }
                else if (strstr((const char *)mime_buf, "png"))
                {
                    node->img_type = FILE_IMG_PNG;
                    printf("cover: PNG found, mime=%s\r\n", mime_buf);
                }
                else
                {
                    printf("cover: unknown mime=%s\r\n", mime_buf);
                    f_close(&file);
                    return;
                }
            }

            /* 3. 图片类型 (1 byte) */
            if (off >= end) { f_close(&file); return; }
            off++;

            /* 4. 描述 (null-terminated, encoding dependent) */
            if (encoding == 0)
            {
                uint8_t b;
                do {
                    if (off >= end) break;
                    f_lseek(&file, off);
                    f_read(&file, &b, 1, &br);
                    off++;
                } while (b != 0);
            }
            else
            {
                while (off + 1 < end)
                {
                    uint8_t b1, b2;
                    f_lseek(&file, off);
                    f_read(&file, &b1, 1, &br);
                    f_read(&file, &b2, 1, &br);
                    off += 2;
                    if (b1 == 0 && b2 == 0) break;
                }
            }

            /* 5. 图片数据起始 */
            node->img_offset = off;
            node->img_size   = end - off;
            printf("cover: offset=%lu size=%lu\r\n", node->img_offset, node->img_size);

            f_close(&file);
            return;
        }

        pos += 10 + frame_size;
    }

    printf("cover: no APIC frame found\r\n");
    f_close(&file);
}

/* ==================================================================== */
/*                   MP3 文件扫描 (双向循环链表)                          */
/* ==================================================================== */

static void File_ScanMP3(void)
{
    DIR dir;
    FILINFO fno;
    File_MusicNode_t *node_tail = NULL;

    if (f_opendir(&dir, _T("0:")) != FR_OK)
    {
        printf("open dir failed\r\n");
        return;
    }

    g_file_count = 0;
    g_file_music_head = NULL;

    while (g_file_count < FILE_COUNT_MAX)
    {
        FRESULT res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        /* 跳过 . 和 .. */
        if (TCHAR_Cmp(fno.fname, _T(".")) == 0 ||
            TCHAR_Cmp(fno.fname, _T("..")) == 0)
        {
            continue;
        }

        /* 跳过目录/隐藏/系统文件 */
        if ((fno.fattrib & AM_DIR) || (fno.fattrib & (AM_HID | AM_SYS)))
            continue;

        if (fno.fname[0] == '\0') continue;

        /* 匹配音频后缀 */
        TCHAR *ext = TCHAR_Strrchr(fno.fname, _T('.'));
        if (ext == NULL) continue;

        if (!(TCHAR_Cmp(ext, _T(".mp3")) == 0 ||
              TCHAR_Cmp(ext, _T(".MP3")) == 0 ||
              TCHAR_Cmp(ext, _T(".wav")) == 0 ||
              TCHAR_Cmp(ext, _T(".WAV")) == 0))
        {
            continue;
        }

        /* 分配节点内存 */
        File_MusicNode_t *new_node = (File_MusicNode_t *)pvPortMalloc(sizeof(File_MusicNode_t));
        if (new_node == NULL)
        {
            printf("malloc failed\r\n");
            continue;
        }

        memset(new_node, 0, sizeof(File_MusicNode_t));
        new_node->if_play = 1;

        /* 复制文件名 */
        TCHAR_Strncpy(new_node->unicode_name, fno.fname, FILE_NAME_MAX_LEN - 1);
        new_node->unicode_name[FILE_NAME_MAX_LEN - 1] = '\0';

        /* 转 UTF-8 */
        TCHAR_ToUtf8(new_node->unicode_name, new_node->utf8_name, FILE_NAME_MAX_LEN);

        /* 检测封面 */
        File_DetectCoverInfo(new_node);

        /* 挂载到双向循环链表 */
        if (g_file_music_head == NULL)
        {
            g_file_music_head = new_node;
            new_node->prev = new_node;
            new_node->next = new_node;
        }
        else
        {
            new_node->prev = node_tail;
            new_node->next = g_file_music_head;

            node_tail->next = new_node;
            g_file_music_head->prev = new_node;
        }

        node_tail = new_node;
        g_file_count++;
    }

    f_closedir(&dir);

    /* 打印扫描结果 */
    printf("\r\nScanning MP3 finished\r\nTotal: %lu songs\r\n", g_file_count);

    File_MusicNode_t *p = g_file_music_head;
    uint32_t idx = 1;
    if (p != NULL)
    {
        static const char *img_names[] = {"NONE", "JPEG", "PNG"};
        do {
            printf("song %lu: %s  if_play=%d  cover=%s\r\n",
                   idx++, p->utf8_name, p->if_play, img_names[p->img_type]);
            p = p->next;
        } while (p != g_file_music_head);
    }

    g_file_current_music = g_file_music_head;
    if (g_file_current_music != NULL)
    {
        printf("\r\ncurrent music: %s\r\n", g_file_current_music->utf8_name);
    }
}

/* ==================================================================== */
/*                   链表清理                                             */
/* ==================================================================== */

void File_FreeList(void)
{
    if (g_file_music_head == NULL) return;

    File_MusicNode_t *p = g_file_music_head;
    File_MusicNode_t *start = p;

    do {
        File_MusicNode_t *next = p->next;
        vPortFree(p);
        p = next;
    } while (p != start);

    g_file_music_head    = NULL;
    g_file_count         = 0;
    g_file_current_music = NULL;
}

/* ==================================================================== */
/*                   USB 拔出后重初始化                                   */
/* ==================================================================== */

void File_ReinitAfterUSB(void)
{
    File_MusicNode_t *old_list = g_file_music_head;
    uint32_t old_cnt = g_file_count;

    /* 关闭字库文件 */
    f_close(&g_file_font);

    /* 卸载两个卷 */
    f_mount(NULL, _T(""), 0);
    f_mount(NULL, _T("1:"), 0);

    /* 重新挂载 SD 卡 */
    disk.is_initialized[0] = 0;
    if (f_mount(&g_file_fs, _T(""), 1) != FR_OK)
    {
        printf("re-mount sd fail, keep old list\r\n");
        goto restore;
    }

    /* 重新挂载外部 Flash */
    if (f_mount(&g_file_fs_flash, _T("1:"), 1) != FR_OK)
    {
        printf("re-mount flash fail\r\n");
        goto restore;
    }

    /* 重新打开字库文件 */
    if (f_open(&g_file_font, _T("1:my_font_song_14.bin"), FA_READ) != FR_OK)
    {
        printf("re-open font fail\r\n");
        goto restore;
    }

    /* 重新扫描歌曲 */
    File_ScanMP3();

    /* 全部成功, 释放旧链表 */
    if (old_list)
    {
        File_MusicNode_t *p = old_list;
        File_MusicNode_t *start = p;
        do {
            File_MusicNode_t *next = p->next;
            vPortFree(p);
            p = next;
        } while (p != start);
    }

    g_file_current_music = g_file_music_head;
    return;

restore:
    g_file_music_head    = old_list;
    g_file_count         = old_cnt;
    g_file_current_music = old_list;

    f_mount(&g_file_fs, _T(""), 1);
    f_mount(&g_file_fs_flash, _T("1:"), 1);
    f_open(&g_file_font, _T("1:my_font_song_14.bin"), FA_READ);
}

/* ==================================================================== */
/*                   播放列表保存/恢复                                    */
/* ==================================================================== */

/** @brief 共用 FIL 对象 (4KB 扇区缓冲, 避免每个函数各占一份) */
static FIL s_file;

void File_SavePlaylist(void)
{
    UINT bw;
    File_PlaylistSave_t data;
    File_MusicNode_t *p;

    if ((g_file_music_head == NULL) || (g_file_count == 0)) return;

    memset(&data, 0, sizeof(data));
    data.magic            = FILE_PLAYLIST_MAGIC;
    data.version          = FILE_PLAYLIST_VERSION;
    data.saved_file_count = g_file_count;

    p = g_file_music_head;
    for (uint32_t i = 0; i < g_file_count && i < FILE_COUNT_MAX; i++)
    {
        data.if_play[i] = p->if_play;
        if (p == g_file_current_music) data.current_index = i;
        p = p->next;
    }

    if (f_open(&s_file, _T("1:playlist.cfg"), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return;
    f_write(&s_file, &data, sizeof(data), &bw);
    f_close(&s_file);

    printf("playlist saved (%lu songs, cur=%lu)\r\n", g_file_count, data.current_index);
}

void File_RestorePlaylist(void)
{
    UINT br;
    File_PlaylistSave_t data;
    File_MusicNode_t *p;

    if ((g_file_music_head == NULL) || (g_file_count == 0)) return;

    if (f_open(&s_file, _T("1:playlist.cfg"), FA_READ) != FR_OK)
    {
        printf("no saved playlist\r\n");
        return;
    }

    FRESULT res = f_read(&s_file, &data, sizeof(data), &br);
    f_close(&s_file);

    if (res != FR_OK || br != sizeof(data)) { printf("playlist bad read\r\n"); return; }
    if (data.magic != FILE_PLAYLIST_MAGIC || data.version != FILE_PLAYLIST_VERSION)
    {
        printf("playlist bad magic\r\n");
        return;
    }
    if (data.saved_file_count != g_file_count)
    {
        printf("playlist cnt chg (%lu!=%lu)\r\n", data.saved_file_count, g_file_count);
        return;
    }

    /* 恢复 if_play */
    p = g_file_music_head;
    for (uint32_t i = 0; i < g_file_count && i < FILE_COUNT_MAX; i++)
    {
        p->if_play = data.if_play[i] ? 1 : 0;
        p = p->next;
    }

    /* 恢复当前歌曲指针 */
    if (data.current_index < g_file_count)
    {
        p = g_file_music_head;
        for (uint32_t i = 0; i < data.current_index; i++)
            p = p->next;
        g_file_current_music = p;
    }

    printf("playlist restored (%lu songs, cur=%lu)\r\n", g_file_count, data.current_index);
}

void File_SaveConfig(void)
{
    UINT bw;
    File_ConfigSave_t data;

    memset(&data, 0, sizeof(data));
    data.magic     = FILE_CONFIG_MAGIC;
    data.version   = FILE_CONFIG_VERSION;

    extern volatile uint8_t g_play_mode;
    extern volatile uint8_t g_volume;
    data.play_mode = g_play_mode;
    data.volume    = g_volume;

    if (f_open(&s_file, _T("1:mode.cfg"), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return;
    f_write(&s_file, &data, sizeof(data), &bw);
    f_close(&s_file);

    printf("config saved (mode=%lu, vol=%lu)\r\n", data.play_mode, data.volume);
}

void File_RestoreConfig(void)
{
    UINT br;
    File_ConfigSave_t data;

    if (f_open(&s_file, _T("1:mode.cfg"), FA_READ) != FR_OK)
    {
        printf("no saved config\r\n");
        return;
    }

    FRESULT res = f_read(&s_file, &data, sizeof(data), &br);
    f_close(&s_file);

    if (res != FR_OK || br != sizeof(data)) { printf("config bad read\r\n"); return; }
    if (data.magic != FILE_CONFIG_MAGIC || data.version != FILE_CONFIG_VERSION)
    {
        printf("config bad magic\r\n");
        return;
    }

    extern volatile uint8_t g_play_mode;
    extern volatile uint8_t g_volume;
    g_play_mode = (uint8_t)data.play_mode;
    g_volume    = (uint8_t)data.volume;

    printf("config restored (mode=%lu, vol=%lu)\r\n", data.play_mode, data.volume);
}
