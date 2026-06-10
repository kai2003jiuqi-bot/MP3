/**
 * @file file.c
 * @brief 文件系统管理, MP3文件扫描, ID3v2标签解析(APIC封面), 播放列表保存/恢复到Flash
 *
 * ========== 功能概述 ==========
 * 本模块是整个播放器的数据管理层, 核心功能如下:
 *
 * 1. 文件系统初始化
 *    挂载 SD 卡 FATFS 和外部 Flash (W25Q64) FATFS 文件系统,
 *    打开字库文件供 LVGL 显示中文。
 *
 * 2. MP3/封面扫描
 *    扫描 SD 卡根目录下的 MP3/WAV 文件,
 *    解析每个文件中的 ID3v2 标签, 提取 APIC 帧中的封面图片信息
 *    (图片类型/偏移/大小), 构建双向循环链表。
 *
 * 3. 播放列表序列化
 *    将播放列表 (if_play 状态、当前歌曲索引) 保存到外部 Flash,
 *    下次开机时恢复, 实现"掉电记忆"功能。
 *
 * 4. 配置保存/恢复
 *    保存音量、播放模式到外部 Flash, 掉电后恢复。
 *
 * 5. USB 拔出后重建
 *    当 USB 拔出后重新挂载文件系统并扫描歌曲,
 *    旧链表在成功切换后释放, 失败时回退到旧链表。
 *
 * ========== 数据结构 ==========
 *   歌曲列表使用双向循环链表:
 *     g_file_music_head → [node1] ↔ [node2] ↔ [node3] ↔ ...
 *                           ↑                                 ↓
 *                           +---------------head--------------+
 *     g_file_current_music 指向当前播放的歌曲节点。
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

uint32_t          g_file_count            = 0;       /* 歌曲总数 */
File_MusicNode_t *g_file_music_head       = NULL;    /* 链表头指针 */
File_MusicNode_t *g_file_current_music    = NULL;    /* 当前播放歌曲指针 */

FATFS g_file_fs;         /**< SD 卡文件系统对象 */
FATFS g_file_fs_flash;   /**< 外部 Flash 文件系统对象 */
FIL   g_file_font;       /**< 字库文件对象 (外部 Flash 中的中文字体) */

/* ==================================================================== */
/*                   内部函数声明                                         */
/* ==================================================================== */

static void      File_ScanMP3(void);
static void      File_DetectCoverInfo(File_MusicNode_t *node);

/* ==================================================================== */
/*                   文件系统初始化                                       */
/* ==================================================================== */

/*
 * 函数: File_Init
 * 功能: 初始化文件系统, 挂载 SD 卡和外部 Flash, 扫描歌曲
 *
 * 初始化顺序:
 *   1. 挂载 SD 卡 (卷标 "") — 存放 MP3 文件
 *   2. 挂载外部 Flash (卷标 "1:") — 存放配置文件和字库
 *   3. 打开中文字库文件 my_font_song_14.bin
 *   4. 延时等待 USB 设备初始化完成
 *   5. 初始化 USB 设备 (MSC 大容量存储)
 *   6. 扫描 SD 卡根目录下的 MP3/WAV 文件
 */
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

/*
 * 函数: File_DetectCoverInfo
 * 功能: 解析 MP3 文件中的 ID3v2 标签, 提取 APIC (封面图片) 帧信息
 *
 * ========== ID3v2 标签结构 ==========
 *
 *   ID3v2 头部 (10 字节):
 *     字节 0-2:  "ID3" 签名
 *     字节 3:    版本号主版本
 *     字节 4:    版本号副版本
 *     字节 5:    标志位
 *     字节 6-9:  标签总大小 (synchsafe integer, 28 位)
 *
 *   帧结构 (每个帧 10 字节头部 + 数据):
 *     字节 0-3:  帧 ID (如 "APIC" 表示封面图片)
 *     字节 4-7:  帧大小 (标准 32 位大端)
 *     字节 8-9:  帧标志
 *
 * ========== APIC 帧格式 ==========
 *   1. 文字编码 (1 byte): 0=ISO-8859-1, 1=UTF-16
 *   2. MIME 类型 (null-terminated): "image/jpeg", "image/png" 等
 *   3. 图片类型 (1 byte): 0x03 表示封面图 (Cover (front))
 *   4. 描述 (null-terminated, 编码取决于文字编码字段)
 *   5. 图片二进制数据
 *
 * @param node  指向要解析的歌曲节点
 */
static void File_DetectCoverInfo(File_MusicNode_t *node)
{
    FIL file;
    uint8_t header[10];
    UINT br;

    /* 初始化封面信息为无图片 */
    node->img_type   = FILE_IMG_NONE;
    node->img_offset = 0;
    node->img_size   = 0;

    if (f_open(&file, node->unicode_name, FA_READ) != FR_OK) return;

    /* 读取 ID3v2 头 (10 字节) */
    if (f_read(&file, header, 10, &br) != FR_OK || br < 10)
    {
        f_close(&file);
        return;
    }

    /* 检查 ID3 签名: 前 3 字节是否为 "ID3" */
    if (header[0] != 'I' || header[1] != 'D' || header[2] != '3')
    {
        f_close(&file);
        return;
    }

    /* 解析 ID3v2 标签总大小 (synchsafe integer 格式) */
    uint32_t tag_size = ((uint32_t)header[6] << 21)
                      | ((uint32_t)header[7] << 14)
                      | ((uint32_t)header[8] << 7)
                      |  (uint32_t)header[9];

    /* 遍历所有帧, 查找 APIC (封面图片) 帧 */
    uint32_t pos = 10;       /* 从 ID3v2 头后的第一个帧开始 */
    uint8_t frame_hdr[10];   /* 帧头部缓冲区 */

    while (pos + 10 <= 10 + tag_size)
    {
        f_lseek(&file, pos);
        if (f_read(&file, frame_hdr, 10, &br) != FR_OK || br < 10) break;

        /* 帧 ID 全零或全 FF 表示结束 */
        if (frame_hdr[0] == 0x00 || frame_hdr[0] == 0xFF) break;

        /* 解析帧大小 (标准 32 位大端) */
        uint32_t frame_size = ((uint32_t)frame_hdr[4] << 24)
                            | ((uint32_t)frame_hdr[5] << 16)
                            | ((uint32_t)frame_hdr[6] << 8)
                            |  (uint32_t)frame_hdr[7];
        if (frame_size < 2 || frame_size > tag_size) break;

        /* 检查是否为 APIC 帧 (ID = "APIC") */
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

            /* 2. MIME 类型 (null-terminated 字符串) */
            {
                uint8_t b;
                do {
                    if (off >= end) break;
                    f_lseek(&file, off);
                    f_read(&file, &b, 1, &br);
                    off++;
                } while (b != 0);
            }

            /* 检查 MIME 类型, 确定图片格式 */
            {
                uint8_t mime_buf[32];
                uint32_t mime_len = 0;
                f_lseek(&file, pos + 11);   /* 回到 MIME 起始 */
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

            /* 3. 图片类型 (1 byte) — 0x03 表示封面 */
            if (off >= end) { f_close(&file); return; }
            off++;

            /* 4. 描述 (null-terminated, 编码取决于 encoding 字段) */
            if (encoding == 0)
            {
                /* ISO-8859-1: 单字节 null terminated */
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
                /* UTF-16: 双字节 null terminated (00 00) */
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

            /* 5. 图片数据起始: 当前 off 即为图片数据偏移 */
            node->img_offset = off;
            node->img_size   = end - off;   /* 图片数据大小 */
            printf("cover: offset=%lu size=%lu\r\n", node->img_offset, node->img_size);

            f_close(&file);
            return;
        }

        pos += 10 + frame_size;  /* 移到下一个帧 */
    }

    printf("cover: no APIC frame found\r\n");
    f_close(&file);
}

/* ==================================================================== */
/*                   MP3 文件扫描 (双向循环链表)                          */
/* ==================================================================== */

/*
 * 函数: File_ScanMP3
 * 功能: 扫描 SD 卡根目录, 构建 MP3/WAV 文件的双向循环链表
 *
 * ========== 扫描流程 ==========
 *   1. 打开根目录 "0:"
 *   2. 循环调用 f_readdir() 读取目录项
 *   3. 过滤:
 *      - 排除 "." 和 ".."
 *      - 排除目录、隐藏文件、系统文件
 *      - 只保留 .mp3/.MP3/.wav/.WAV 后缀的文件
 *   4. 为每个匹配文件:
 *      a. 动态分配 File_MusicNode_t 节点
 *      b. 复制文件名 (UTF-16 和 UTF-8 两份)
 *      c. 解析 ID3v2 封面信息
 *      d. 挂载到双向循环链表
 *   5. 设置 g_file_current_music 为链表第一首歌
 *
 * ========== 链表构建算法 ==========
 *
 *   首次插入 (head == NULL):
 *     head = new_node
 *     new_node->prev = new_node
 *     new_node->next = new_node
 *     (自己指向自己, 形成单节点环)
 *
 *   后续插入 (head != NULL):
 *     new_node->prev = node_tail
 *     new_node->next = g_file_music_head
 *     node_tail->next = new_node
 *     head->prev = new_node
 *     (新节点插入在 tail 和 head 之间)
 */
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

    /*
     * 循环读取目录项, 最多 FILE_COUNT_MAX 个
     * 每次读取一个文件, 判断是否匹配音频格式
     */
    while (g_file_count < FILE_COUNT_MAX)
    {
        FRESULT res = f_readdir(&dir, &fno);
        if (res != FR_OK || fno.fname[0] == 0) break;

        /* 跳过 "." 和 ".." */
        if (TCHAR_Cmp(fno.fname, _T(".")) == 0 ||
            TCHAR_Cmp(fno.fname, _T("..")) == 0)
        {
            continue;
        }

        /* 跳过目录/隐藏/系统文件 */
        if ((fno.fattrib & AM_DIR) || (fno.fattrib & (AM_HID | AM_SYS)))
            continue;

        if (fno.fname[0] == '\0') continue;

        /* 检查文件扩展名, 只保留音频文件 */
        TCHAR *ext = TCHAR_Strrchr(fno.fname, _T('.'));
        if (ext == NULL) continue;

        if (!(TCHAR_Cmp(ext, _T(".mp3")) == 0 ||
              TCHAR_Cmp(ext, _T(".MP3")) == 0 ||
              TCHAR_Cmp(ext, _T(".wav")) == 0 ||
              TCHAR_Cmp(ext, _T(".WAV")) == 0))
        {
            continue;
        }

        /* 分配节点内存 (FreeRTOS 堆) */
        File_MusicNode_t *new_node = (File_MusicNode_t *)pvPortMalloc(sizeof(File_MusicNode_t));
        if (new_node == NULL)
        {
            printf("malloc failed\r\n");
            continue;
        }

        memset(new_node, 0, sizeof(File_MusicNode_t));
        new_node->if_play = 1;   /* 默认标记为可播放 */

        /* 复制文件名 (UTF-16 格式, 用于 FATFS) */
        TCHAR_Strncpy(new_node->unicode_name, fno.fname, FILE_NAME_MAX_LEN - 1);
        new_node->unicode_name[FILE_NAME_MAX_LEN - 1] = '\0';

        /* 转换为 UTF-8 (用于 LVGL 显示和 printf) */
        TCHAR_ToUtf8(new_node->unicode_name, new_node->utf8_name, FILE_NAME_MAX_LEN);

        /* 检测封面图片信息 */
        File_DetectCoverInfo(new_node);

        /* 挂载到双向循环链表 */
        if (g_file_music_head == NULL)
        {
            /* 第一个节点: 自环 */
            g_file_music_head = new_node;
            new_node->prev = new_node;
            new_node->next = new_node;
        }
        else
        {
            /* 后续节点: 插入到 tail 与 head 之间 */
            new_node->prev = node_tail;
            new_node->next = g_file_music_head;

            node_tail->next = new_node;
            g_file_music_head->prev = new_node;
        }

        node_tail = new_node;
        g_file_count++;
    }

    f_closedir(&dir);

    /* 打印扫描结果摘要 */
    printf("\r\nScanning MP3 finished\r\nTotal: %lu songs\r\n", g_file_count);

    /* 打印每首歌的详细信息 (调试用) */
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

    /* 默认从第一首歌开始播放 */
    g_file_current_music = g_file_music_head;
    if (g_file_current_music != NULL)
    {
        printf("\r\ncurrent music: %s\r\n", g_file_current_music->utf8_name);
    }
}

/* ==================================================================== */
/*                   链表清理                                             */
/* ==================================================================== */

/*
 * 函数: File_FreeList
 * 功能: 释放整个歌曲链表中所有节点的动态内存
 *
 * 遍历双向循环链表, 使用 vPortFree() 释放每个节点,
 * 最后将全局指针置空。
 *
 * 注意: 此操作不可逆, 调用前确保不再需要播放列表
 */
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

/*
 * 函数: File_ReinitAfterUSB
 * 功能: USB 拔出后重新初始化文件系统, 重建播放列表
 *
 * 当 USB 连接到电脑时, STM32 作为 MSC 设备,
 * SD 卡被电脑独占, 停止播放。
 * USB 拔出后, 需要:
 *   1. 关闭所有打开的文件
 *   2. 卸载文件系统
 *   3. 重新挂载 SD 卡和外部 Flash
 *   4. 重新打开字库文件
 *   5. 重新扫描歌曲, 构建新链表
 *   6. 如果上述任何步骤失败, 回退到旧链表
 *
 * 回退机制:
 *   先保留旧链表的指针 (old_list), 如果重新扫描失败,
 *   恢复到旧链表, 保证系统不丢失播放列表。
 */
void File_ReinitAfterUSB(void)
{
    File_MusicNode_t *old_list = g_file_music_head;
    uint32_t old_cnt = g_file_count;

    /* 关闭字库文件 */
    f_close(&g_file_font);

    /* 卸载两个卷 */
    f_mount(NULL, _T(""), 0);
    f_mount(NULL, _T("1:"), 0);

    /* 重新挂载 SD 卡 (需要重置初始化标志) */
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

    /* 重新扫描歌曲, 构建新链表 */
    File_ScanMP3();

    /* 全部成功: 释放旧链表内存 */
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

    /*
     * 恢复点: 如果重新初始化失败, 回退到旧链表
     * 尝试重新挂载文件系统以维持基本功能
     */
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

/*
 * 函数: File_SavePlaylist
 * 功能: 将当前播放列表序列化保存到外部 Flash
 *
 * 保存的内容 (File_PlaylistSave_t 结构体):
 *   - magic:       校验魔数 (FILE_PLAYLIST_MAGIC)
 *   - version:     版本号
 *   - saved_file_count: 保存时的歌曲总数
 *   - if_play[]:   每首歌是否在播放列表中的标记数组
 *   - current_index: 当前播放歌曲在列表中的索引
 *
 * 文件路径: 1:playlist.cfg (外部 Flash 的 FATFS 卷)
 *
 * 保存时机:
 *   - 每次切歌时 (保持当前歌曲索引)
 *   - 用户修改播放列表 (删除/添加歌曲)
 *
 * 恢复时通过 magic + version + count 三重重校验,
 * 防止读取到格式不匹配或损坏的数据。
 */
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

    /*
     * 遍历链表, 记录每首歌的 if_play 状态
     * 同时找到当前歌曲的索引
     */
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

/*
 * 函数: File_RestorePlaylist
 * 功能: 从外部 Flash 恢复上次保存的播放列表
 *
 * 恢复流程:
 *   1. 打开 playlist.cfg 文件
 *   2. 读取 File_PlaylistSave_t 结构体
 *   3. 校验: magic + version + count 三重验证
 *   4. 恢复每首歌的 if_play 状态
 *   5. 恢复当前歌曲指针
 *
 * 校验失败的情况:
 *   - 文件不存在 (首次开机) — 使用默认全部可播放
 *   - magic 不匹配 — 文件损坏或格式不同
 *   - version 不匹配 — 版本不兼容
 *   - count 不匹配 — 歌曲列表已变化 (增删文件), 放弃恢复
 */
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

    /* 恢复 if_play: 逐节点恢复 */
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

/*
 * 函数: File_SaveConfig
 * 功能: 保存播放器配置到外部 Flash
 *
 * 保存的内容 (File_ConfigSave_t):
 *   - magic:   校验魔数 (FILE_CONFIG_MAGIC)
 *   - version: 版本号
 *   - play_mode: 播放模式 (ALL/ONE/SHUFFLE)
 *   - volume:   音量值
 *
 * 文件路径: 1:mode.cfg
 *
 * 保存时机:
 *   每次音量变化 (音量±键短按) 时保存
 *   播放模式变化时保存 (被注释, 未启用)
 */
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

/*
 * 函数: File_RestoreConfig
 * 功能: 从外部 Flash 恢复上次保存的配置
 *
 * 恢复流程:
 *   1. 打开 mode.cfg 文件
 *   2. 验证 magic 和 version
 *   3. 恢复播放模式和音量
 */
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
