/**
 * @file    task_playMusic.c
 * @brief   音乐播放 FreeRTOS 任务
 *
 * 核心播放引擎, 负责:
 *   - 从 SD 卡读取 MP3 数据, 喂给 VS1003 硬件解码器
 *   - 淡入/淡出音量过渡 (防止切歌爆音)
 *   - 接收并处理播放控制命令
 *   - 播放模式管理 (顺序/单曲循环/随机)
 *   - 封面图片解码触发
 */

/* ==================================================================== */
/*                    头文件包含                                          */
/* ==================================================================== */

#include "FreeRTOS.h"       /* FreeRTOS 内核 API: task, queue 等          */
#include "task.h"           /* 任务创建、任务控制块等                       */
#include "queue.h"          /* 消息队列 API, 用于接收播放控制命令           */
#include "ff.h"             /* FatFs 文件系统 API: FIL, f_read, f_open 等  */
#include "app.h"            /* 应用层全局变量/枚举: STATE_PLAYMUSIC,       */
                            /*   g_file_current_music, g_volume 等        */
#include "lvgl.h"           /* LVGL 图形库 (可能用于 UI 更新通知)           */
#include <stdio.h>          /* printf 调试输出                             */
#include <stdlib.h>         /* rand() 随机函数, 用于随机播放模式选歌       */
#include "file.h"           /* 文件列表管理: File_MusicNode_t,              */
                            /*   g_file_music_head, File_SavePlaylist 等  */
#include "vs1003.h"         /* VS1003 硬件解码器驱动: Init/Write/SetVolume */
#include "TCHAR_Sring.h"    /* Unicode/UTF-8 字符串辅助宏 (TCHAR 相关)     */
#include "cover_art.h"      /* 封面图片解码触发: cover_art_decode()        */

/* ==================================================================== */
/*                    内部宏定义                                          */
/* ==================================================================== */

/** @brief 淡出间隔 (毫秒) — 每 FADE_STEP_MS 毫秒步进一次音量 */
#define FADE_STEP_MS      2U
/** @brief 每次淡出步进 (音量值) — 每步增大或减小 5 个单位音量 */
#define FADE_STEP_VOL     5U
/** @brief 切歌后冲刷 VS1003 缓冲的块数 — 每块 32 字节, 共 64 块 ≈ 2KB,
 *         目的是把 VS1003 内部 FIFO 中的旧歌数据排空 */
#define FLUSH_CHUNKS      64U

/* ==================================================================== */
/*                    静态 (模块内) 变量                                  */
/* ==================================================================== */

/** @brief 当前播放控制命令
 *  通过消息队列接收外部 (如按键、UI) 发送的命令, 在任务主循环中处理 */
static STATE_PLAYMUSIC s_cmd = PLAYMUSIC_PAUSE;

/** @brief SHUFFLE (随机) 模式下, 跳过淡出后的自动 "前进到下一首" 操作
 *  因为随机模式下, NEXT 命令已通过 pick_random_song() 选好了歌,
 *  淡出结束后不需要再走默认的 next/prev 遍历逻辑 */
static uint8_t s_skip_fade_advance = 0;

/** @brief SEEK 后跳过 update_song_metadata 更新 (当前未使用, 保留备用) */
static uint8_t s_skip_metadata_update = 0;

/* ==================================================================== */
/*               全局变量定义                                             */
/* ==================================================================== */

TaskHandle_t  g_task_play_music;   /**< 本任务的句柄, 供其他任务使用 */
QueueHandle_t g_queue_play_music;  /**< 播放控制命令的消息队列句柄 */

/* ==================================================================== */
/*               内部函数声明                                             */
/* ==================================================================== */

static void skip_id3_tag(FIL *file);          /* 跳过 ID3v2 标签头 */
static void find_mpeg_sync(FIL *file);        /* 搜索 MPEG 帧同步字 0xFFE0 */
static File_MusicNode_t *pick_random_song(void);  /* 随机模式选歌 */
static void update_song_metadata(FIL *file);  /* 更新当前歌曲元信息 */
static void feed_audio_data(FIL *file, uint8_t *buff, UINT *br,
                            uint8_t *need_vol_restore, uint8_t *fade_vol,
                            uint32_t *fade_tick, STATE_PLAYMUSIC *cmd);
                                              /* 从文件读取 32 字节喂给 VS1003 */

/* ==================================================================== */
/*               ID3v2 标签跳过                                          */
/* ==================================================================== */

/**
 * @brief 跳过 ID3v2 标签头
 *
 * ID3v2 是 MP3 文件中嵌入元数据 (歌手、专辑、封面等) 的标准格式,
 * 它位于音频数据之前, 不能作为 MPEG 帧喂给 VS1003, 必须跳过。
 *
 * ID3v2 头部固定 10 字节:
 *   字节 0-2:  "ID3" 标识
 *   字节 3-4:  版本号
 *   字节 5:    标志位
 *   字节 6-9:  标签总大小 (使用 7 位编码, 共 28 位, 大端序)
 *
 * @param file  已打开的 FATFS 文件对象指针
 */
static void skip_id3_tag(FIL *file)
{
    uint8_t header[10];      /* ID3v2 头缓冲区 */
    UINT    br;              /* f_read 实际读取字节数 */
    FSIZE_t fpos = f_tell(file);  /* 保存当前文件位置, 用于回退 */

    /* 尝试读取 10 字节 ID3 头部 */
    if (f_read(file, header, 10, &br) != FR_OK || br < 10)
    {
        /* 读取失败或文件不足 10 字节 → 回退到原位置 */
        f_lseek(file, fpos);
        return;
    }

    /* 检查是否为 ID3v2 标签: 前三个字节必须是 "ID3" */
    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3')
    {
        /*
         * 解析 ID3v2 标签大小 (非标准 32 位整数, 而是 4 个 7 位值,
         * 每个字节的最高位恒为 0, 有效数据只在低 7 位):
         *   size = (b6<<21) | (b7<<14) | (b8<<7) | b9
         * 跳到 10 字节头部 + 标签数据之后, 即 MP3 音频起始位置
         */
        uint32_t tag_size = ((uint32_t)header[6] << 21)
                          | ((uint32_t)header[7] << 14)
                          | ((uint32_t)header[8] << 7)
                          |  (uint32_t)header[9];
        f_lseek(file, fpos + 10 + tag_size);
        printf("skip ID3v2, %lu bytes\r\n", (unsigned long)tag_size);
    }
    else
    {
        /* 不是 ID3v2 标签, 回退到原位置, 当作普通 MPEG 数据开始 */
        f_lseek(file, fpos);
    }
}

/* ==================================================================== */
/*               MPEG 帧同步字搜索                                       */
/* ==================================================================== */

/**
 * @brief 在文件中向后搜索 MPEG 帧同步字
 *
 * MPEG 音频帧的第一字节固定为 0xFF, 第二字节高 5 位为 11111 (0xE0)。
 * 合起来就是 0xFFE0 开头 (掩码后 0xFFE0)。
 *
 * 在 SEEK (拖动进度条) 后使用, 因为根据码率估算的偏移位置
 * 可能落在某个帧的中间, 需要重新同步到有效帧起始。
 *
 * @param file  已打开的 FATFS 文件对象指针
 */
static void find_mpeg_sync(FIL *file)
{
    uint8_t byte;    /* 当前读取的字节 */
    uint8_t prev = 0; /* 上一个字节 */
    UINT    br;      /* 实际读取字节数 */

    /* 逐字节读取, 寻找 0xFF 0xE? 的帧同步模式 */
    while (f_read(file, &byte, 1, &br) == FR_OK && br == 1)
    {
        /*
         * 检测条件:
         *   - prev == 0xFF          → 上一字节是全 1
         *   - (byte & 0xE0) == 0xE0 → 当前字节高 3 位是 111
         * 满足则后退 2 字节到帧头起始位置
         */
        if (prev == 0xFF && (byte & 0xE0) == 0xE0)
        {
            f_lseek(file, f_tell(file) - 2);
            return;
        }
        prev = byte;
    }
}

/* ==================================================================== */
/*               随机选歌                                                */
/* ==================================================================== */

/**
 * @brief 在随机播放模式下, 从播放列表中随机选取一首可播放的歌曲
 *
 * 算法:
 *   1. 遍历循环链表, 统计 if_play == 1 的歌曲总数
 *   2. 用 rand() 取模得到随机序号
 *   3. 再次遍历找到该序号对应的节点
 *   4. 如果列表不止一首且选中的恰好是当前播放歌曲, 则跳到下一首可播放的
 *
 * @return 选中的歌曲节点指针, 若无可播放歌曲则返回 NULL
 */
static File_MusicNode_t *pick_random_song(void)
{
    int total = 0;                       /* 可播放歌曲总数 */
    File_MusicNode_t *p = g_file_music_head;  /* 从链表头开始遍历 */
    if (p == NULL) return NULL;          /* 空列表, 直接返回 */

    /* 第一遍: 统计可播放 (if_play == 1) 的歌曲总数 */
    do {
        if (p->if_play) total++;
        p = p->next;
    } while (p != g_file_music_head);   /* 循环链表, 回到头则停止 */

    if (total == 0) return NULL;        /* 没有可播放歌曲 */

    /* 第二遍: 随机选择第 r 首可播放歌曲 */
    int r = rand() % total;             /* 0 ~ total-1 的随机序号 */
    p = g_file_music_head;
    while (1)
    {
        if (p->if_play) { if (r == 0) break; r--; }
        p = p->next;
    }

    /*
     * 防止连续两次播放同一首歌:
     * 如果列表不止一首歌, 且选中的正是当前正在播放的,
     * 则跳到下一首可播放的歌曲
     */
    if (total > 1 && p == g_file_current_music)
    {
        do { p = p->next; } while (p->if_play == 0);
    }
    return p;
}

/* ==================================================================== */
/*               歌曲元数据更新                                          */
/* ==================================================================== */

/**
 * @brief 更新当前播放歌曲的全局元数据信息
 *
 * 记录的元数据包含:
 *   - g_song_file_size:     文件总大小
 *   - g_song_audio_start:   音频数据起始偏移 (跳过 ID3v2 后的位置)
 *   - g_song_bitrate:       从 MPEG 帧头解析出的码率 (kbps)
 *   - g_song_decode_base:   VS1003 当前的解码时间基值, 用于计算播放进度
 *
 * @param file  已打开并跳过 ID3v2 标签的文件对象指针
 */
static void update_song_metadata(FIL *file)
{
    /* 记录文件总大小和音频数据起始位置 (即当前文件指针位置) */
    g_song_file_size   = f_size(file);
    g_song_audio_start = f_tell(file);

    /* 尝试从第一个 MPEG 帧头提取码率信息 */
    uint8_t   fh[4];      /* 帧头缓冲区 (4 字节) */
    UINT      br;         /* 实际读取字节数 */
    FSIZE_t   pos = f_tell(file);   /* 保存当前位置用于回退 */
    g_song_bitrate = 0;   /* 初始化为 0, 随后可能被覆盖 */

    /* 读取 4 字节帧头 */
    if (f_read(file, fh, 4, &br) == FR_OK && br == 4)
    {
        f_lseek(file, pos);    /* 读完后立即回退, 不破坏文件指针 */

        /* 验证是否为有效的 MPEG 帧同步字 */
        if (fh[0] == 0xFF && (fh[1] & 0xE0) == 0xE0)
        {
            /*
             * MPEG1 Layer3 码率查找表 (单位: kbps)
             * 索引取自帧头第 3 字节的 bit4-bit7 (bitrate index)
             */
            static const uint16_t bitrates[] = {
                0,   32,  40,  48,  56,  64,  80,  96,
                112, 128, 160, 192, 224, 256, 320, 0
            };
            uint8_t idx = (fh[2] >> 4) & 0x0F;  /* 提取 bitrate index */
            if (idx < 16) g_song_bitrate = bitrates[idx];
        }
    }

    /*
     * 如果解析失败 (非 MPEG 帧或读取错误), 则默认使用 128 kbps
     * 这在 SEEK 时用于估算字节偏移: offset = seek_time * bitrate / 8
     */
    if (g_song_bitrate == 0) g_song_bitrate = 128;

    /* 记录 VS1003 硬件解码器的当前解码时间, 用于计算已经播放的时间 */
    g_song_decode_base = VS1003_GetDecodeTime();
}

/* ==================================================================== */
/*               音频数据喂送                                            */
/* ==================================================================== */

/**
 * @brief 从文件读取 32 字节音频数据并喂给 VS1003 硬件解码器
 *
 * 本函数是播放循环的最低层 I/O 操作:
 *   1. 从 SD 卡读取 32 字节到缓冲区
 *   2. 如果文件读取完毕 (EOF) 或出错, 根据播放模式决定:
 *        - 单曲循环 (PLAY_MODE_ONE): 跳回音频起始位置重播
 *        - 其他模式: 触发切歌流程 (标记需要淡出到下一首)
 *   3. 正常读取后, 将数据写入 VS1003 数据端口
 *   4. 如果需要恢复音量 (切歌后冲刷 FIFO 阶段), 递减计数器;
 *      计数器归零后恢复用户设定的正常音量
 *
 * @param file              已打开的 FATFS 文件对象指针
 * @param buff              32 字节读取缓冲区
 * @param br                输出参数, 实际读取的字节数
 * @param need_vol_restore  音量恢复计数器, 切歌后初始化为 FLUSH_CHUNKS
 * @param fade_vol          当前淡出音量值
 * @param fade_tick         淡出计时时间戳
 * @param cmd               播放命令, 文件读完时设置为 FADE_NEXT
 */
static void feed_audio_data(FIL *file, uint8_t *buff, UINT *br,
                            uint8_t *need_vol_restore, uint8_t *fade_vol,
                            uint32_t *fade_tick, STATE_PLAYMUSIC *cmd)
{
    /* 从 FATFS 读取 32 字节, 存入 buff */
    if (f_read(file, buff, 32, br) != FR_OK || *br == 0)
    {
        /*
         * 文件读取失败或已到末尾 (EOF):
         *   - 单曲循环模式 → 跳回音频起始位置, 开始重新播放
         *   - 其他模式 → 标记进度重置, 设置淡出命令准备切歌
         */
        if (g_play_mode == PLAY_MODE_ONE)
        {
            /* 单曲循环: 直接跳回音频数据起始位置 */
            f_lseek(file, g_song_audio_start);
        }
        else
        {
            /* 非单曲模式: 触发淡出 → 下一首流程 */
            g_reset_progress = 1;     /* 通知 UI 进度条复位 */
            *fade_vol = g_volume;     /* 从当前音量开始淡出 */
            *fade_tick = HAL_GetTick(); /* 记录淡出开始时间 */
            *cmd = PLAYMUSIC_FADE_NEXT; /* 切换命令为 "淡出后下一首" */
        }
        return;
    }

    /* 正常读取: 将数据写入 VS1003 硬件解码器 */
    VS1003_WriteMusicData(buff, *br);

    /*
     * 音量恢复机制:
     * 切歌后, 前 FLUSH_CHUNKS 次数据写入时,
     * VS1003 的音量被临时设置为 VOL_MAX (静音),
     * 目的是让旧歌曲在 VS1003 FIFO 中的残留数据被冲走,
     * 之后才恢复正常的用户音量, 避免新旧音频混杂产生爆音。
     */
    if (*need_vol_restore > 0)
    {
        (*need_vol_restore)--;
        if (*need_vol_restore == 0)
        {
            /* 冲刷完毕, 恢复用户设定的正常音量 */
            VS1003_SetVolume(g_volume);
        }
    }
}

/* ==================================================================== */
/*               播放任务主函数                                          */
/* ==================================================================== */

/**
 * @brief 音乐播放 FreeRTOS 任务入口
 *
 * 任务工作流程:
 *   1. 初始化 VS1003 解码器、文件列表、播放记录
 *   2. 如果有默认播放歌曲, 打开文件并准备播放
 *   3. 进入无限循环:
 *      a. 从消息队列接收播放控制命令 (播放/暂停/上下曲/SEEK等)
 *      b. 根据命令执行相应的状态机操作
 *      c. 如果需要喂数据, 则从文件读取 32 字节写入 VS1003
 *
 * @param pvParameters  FreeRTOS 任务参数 (未使用)
 */
void task_play_music(void *pvParameters)
{
    (void)pvParameters;   /* 消除编译器 "未使用参数" 警告 */

    printf("here\r\n");   /* 调试输出: 标记任务已启动 */

    /* ============================== */
    /*   局部变量声明                  */
    /* ============================== */
    uint8_t  streamBuff[32];      /* 音频数据读取缓冲区, 固定 32 字节 */
    uint32_t bytes_count = 0;     /* 本首歌已发送的字节数 (当前未使用) */
    FIL      file;                /* FATFS 文件对象, 每次只打开一首歌 */
    UINT     br;                  /* f_read 实际读取字节数 */
    uint32_t lastTime = 0;        /* 备用计时变量 (当前未使用) */

    uint8_t  fade_vol;            /* 淡入/淡出过程中的当前音量值 */
    uint8_t  vol_faded = 0;       /* 标记淡出是否已完成 */
    uint8_t  need_vol_restore = 0;/* 切歌后冲刷 VS1003 FIFO 计数器 */
    uint32_t fade_tick = 0;       /* 淡入/淡出步进计时基准 (HAL Tick) */

    /* ============================== */
    /*   硬件和文件系统初始化          */
    /* ============================== */

    /* 初始化 VS1003 硬件解码器, 设置初始音量 */
    VS1003_Init(g_volume);

    /* 初始化文件列表管理 (扫描 SD 卡建立播放链表) */
    File_Init();

    /* 从 SD 卡恢复上次保存的播放列表和配置 (音量、播放模式等) */
    File_RestorePlaylist();
    File_RestoreConfig();

    /* 恢复配置后, 将音量写入 VS1003 */
    VS1003_SetVolume(g_volume);

    /* ============================== */
    /*   打开默认播放歌曲              */
    /* ============================== */

    File_MusicNode_t *opened_node = NULL;  /* 记录当前打开的文件节点 */

    /*
     * 检查是否有默认播放歌曲:
     *   - g_file_current_music == NULL : 没有歌曲
     *   - if_play == 0                : 歌曲被标记为不可播放
     */
    if (g_file_current_music == NULL || g_file_current_music->if_play == 0)
    {
        printf("no song to play, entering PAUSE\r\n");
        s_cmd = PLAYMUSIC_PAUSE;   /* 无歌可播, 进入暂停状态 */
        opened_node = NULL;
    }
    else
    {
        /* 有默认歌曲: 触发封面图片解码 (LVGL 图像) */
        cover_art_decode(g_file_current_music);

        /* 以只读方式打开 MP3 文件 (unicode_name 是长文件名 UTF-16) */
        if (f_open(&file, g_file_current_music->unicode_name, FA_READ) != FR_OK)
        {
            /* 打开失败 → 打印错误信息并死锁 (调试阶段容错) */
            printf("open %s error\r\n", g_file_current_music->utf8_name);
            while (1) { }
        }
        printf("open %s success\r\n", g_file_current_music->utf8_name);

        /* 跳过 ID3v2 标签, 找到真正的音频起始位置 */
        skip_id3_tag(&file);

        /* 更新歌曲元数据 (文件大小、码率等) */
        update_song_metadata(&file);

        /* 记录当前打开的是哪首歌 */
        opened_node = g_file_current_music;
    }

    /* ============================== */
    /*   任务主循环                    */
    /* ============================== */

    while (1)
    {
        uint8_t feed_data = 0;   /* 标志: 本轮循环是否需要喂数据给 VS1003 */

        /*
         * 从消息队列接收播放控制命令 (非阻塞方式, timeout=0):
         *   如果队列中有新命令, 读取到 s_cmd;
         *   如果没有, s_cmd 保持原值
         */
        STATE_PLAYMUSIC prev_cmd = s_cmd;         /* 保存上一轮的命令, 用于状态变化判断 */
        xQueueReceive(g_queue_play_music, &s_cmd, 0);  /* 接收命令 */

        /* ============================================================ */
        /*   命令状态机处理                                              */
        /* ============================================================ */

        switch (s_cmd)
        {

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_PLAY:
            /* 开始播放 / 恢复播放 / 切歌播放 */

            /*
             * 检查当前歌曲是否标记为不可播放:
             * 如果不可播放, 直接淡出跳转到下一首
             */
            if (g_file_current_music->if_play == 0)
            {
                fade_vol = g_volume;           /* 从当前音量开始淡出 */
                fade_tick = HAL_GetTick();     /* 记录淡出起始时间 */
                s_cmd = PLAYMUSIC_FADE_NEXT;   /* 设置命令为淡出后切歌 */
                feed_data = 1;                 /* 淡出过程中仍需喂数据 */
                break;
            }

            /*
             * 检查当前文件是否与打开的文件不同 (外部已切换歌曲节点):
             * 例如通过 UI 切换歌曲时, g_file_current_music 已被修改,
             * 但文件仍指向旧歌, 需要重新打开文件
             */
            if (g_file_current_music != opened_node)
            {
                /* 触发新歌的封面图片解码 */
                cover_art_decode(g_file_current_music);

                /* 关闭旧文件 */
                f_close(&file);

                /* 打开新歌文件 */
                if (f_open(&file, g_file_current_music->unicode_name, FA_READ) != FR_OK)
                {
                    printf("open %s error\r\n", g_file_current_music->utf8_name);
                    while (1) { }
                }

                /* 跳过新歌的 ID3v2 标签, 更新元数据 */
                skip_id3_tag(&file);
                update_song_metadata(&file);

                /* 更新记录: 已打开的文件节点 */
                opened_node = g_file_current_music;

                /* 重置字节计数, 设置音量恢复计数器 (冲刷旧歌 FIFO) */
                bytes_count = 0;
                need_vol_restore = FLUSH_CHUNKS;
            }

            /*
             * 如果不需要音量恢复 (已冲刷完毕), 确保音量回到用户设定值。
             * 这覆盖了从暂停恢复时的场景: 暂停期间用户可能调节了音量
             */
            if (need_vol_restore == 0)
            {
                VS1003_SetVolume(g_volume);
            }

            vol_faded = 0;       /* 清除淡出完成标记 */
            feed_data = 1;       /* 本轮需要喂数据给 VS1003 */
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_FADE_IN:
            /*
             * 淡入恢复播放: 从暂停状态逐步增大音量到正常值
             * 用于从暂停恢复时, 防止突然的响声
             */
            feed_data = 1;       /* 淡入过程中持续喂数据, 保证音频连续 */

            /*
             * 每 FADE_STEP_MS 毫秒步进一次:
             * 从当前淡入音量 (fade_vol) 逐步降低到用户设定的正常音量 (g_volume)
             * 注意: 这里的 fade_vol 初始值大于 g_volume (在 PAUSE 命令中设置),
             * 所以是递减, 实际是 "静音 → 正常音量" 的过渡
             */
            if (HAL_GetTick() - fade_tick >= FADE_STEP_MS)
            {
                fade_tick = HAL_GetTick();

                /*
                 * 如果当前音量比目标音量高出一个步进以上, 则减少一个步进;
                 * 否则直接设为目标音量
                 */
                if (fade_vol > g_volume + FADE_STEP_VOL)
                {
                    fade_vol -= FADE_STEP_VOL;
                }
                else
                {
                    fade_vol = g_volume;
                }
                VS1003_SetVolume(fade_vol);   /* 写入硬件 */

                /* 达到目标音量 → 切换到正常 PLAY 状态 */
                if (fade_vol <= g_volume)
                {
                    s_cmd = PLAYMUSIC_PLAY;
                }
            }
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_FADE_PAUSE:
        case PLAYMUSIC_FADE_NEXT:
        case PLAYMUSIC_FADE_PREV:
            /*
             * 淡出处理 (三种场景共用同一逻辑):
             *   - FADE_PAUSE: 淡出后暂停
             *   - FADE_NEXT:  淡出后切到下一首
             *   - FADE_PREV:  淡出后切到上一首
             *
             * 淡出 = 逐步增大音量到 VOL_MAX (VS1003 中值越大越安静)
             * 直到完全静音后执行后续操作
             */
            feed_data = 1;       /* 淡出过程中持续喂数据, 防止 VS1003 FIFO 干涸 */

            if (HAL_GetTick() - fade_tick >= FADE_STEP_MS)
            {
                fade_tick = HAL_GetTick();
                fade_vol += FADE_STEP_VOL;             /* 音量值递增 → 实际输出减小 */
                if (fade_vol > VOL_MAX) fade_vol = VOL_MAX;  /* 限幅 */
                VS1003_SetVolume(fade_vol);

                /* 淡出完成: fade_vol >= VOL_MAX 表示已经静音 */
                if (fade_vol >= VOL_MAX)
                {
                    if (s_cmd == PLAYMUSIC_FADE_PAUSE)
                    {
                        /* ===== 淡出后暂停 ===== */
                        s_cmd = PLAYMUSIC_PAUSE;
                        vol_faded = 1;     /* 标记淡出已完成 */
                        feed_data = 0;     /* 暂停后不需要再喂数据 */
                    }
                    else
                    {
                        /* ===== 淡出后切歌 (上一首/下一首) ===== */

                        /* 关闭当前歌曲文件 */
                        if (f_close(&file) != FR_OK)
                        {
                            printf("close %s error\r\n",
                                   g_file_current_music->utf8_name);
                            while (1) { }
                        }

                        /*
                         * 如果是随机模式且已由 pick_random_song() 选好歌,
                         * 跳过默认的链表遍历 (next/prev) 逻辑
                         */
                        if (s_skip_fade_advance)
                        {
                            s_skip_fade_advance = 0;   /* 清除跳过标记 */
                        }
                        else
                        {
                            /*
                             * 默认切歌逻辑: 遍历循环链表
                             * 找到下一首 (next) 或上一首 (prev) 可播放的歌曲
                             */
                            File_MusicNode_t *start = g_file_current_music;
                            if (s_cmd == PLAYMUSIC_FADE_NEXT)
                            {
                                /* 向后 (next) 遍历, 跳过不可播放的歌曲 */
                                do {
                                    g_file_current_music = g_file_current_music->next;
                                } while (g_file_current_music->if_play == 0
                                      && g_file_current_music != start);
                            }
                            else /* PLAYMUSIC_FADE_PREV */
                            {
                                /* 向前 (prev) 遍历 */
                                do {
                                    g_file_current_music = g_file_current_music->prev;
                                } while (g_file_current_music->if_play == 0
                                      && g_file_current_music != start);
                            }
                        }

                        /* 将更新后的当前歌曲索引保存到 SD 卡 (掉电恢复用) */
                        File_SavePlaylist();

                        /*
                         * 检查新当前歌曲是否可播放:
                         * 如果整张列表都没有可播放的歌曲 → 进入暂停
                         */
                        if (g_file_current_music->if_play == 0)
                        {
                            s_cmd = PLAYMUSIC_PAUSE;
                            vol_faded = 1;
                            feed_data = 0;
                        }
                        else
                        {
                            /* 有可播放歌曲 → 打开新歌文件开始播放 */

                            /* 触发新歌封面图片解码 */
                            cover_art_decode(g_file_current_music);

                            /* 打开新歌文件 */
                            if (f_open(&file,
                                       g_file_current_music->unicode_name,
                                       FA_READ) != FR_OK)
                            {
                                printf("open %s error\r\n",
                                       g_file_current_music->utf8_name);
                                while (1) { }
                            }

                            /* 跳过 ID3v2, 更新元数据 */
                            skip_id3_tag(&file);
                            update_song_metadata(&file);

                            /* 更新打开记录, 设置音量恢复计数器 */
                            opened_node = g_file_current_music;
                            need_vol_restore = FLUSH_CHUNKS;
                            vol_faded = 0;
                            bytes_count = 0;

                            /* 切到 PLAY 状态开始播放新歌 */
                            s_cmd = PLAYMUSIC_PLAY;
                        }
                    }
                }
            }
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_PAUSE:
            /*
             * 暂停命令处理:
             *   第一次进入时, 启动淡出流程 (静音后暂停)
             *   如果已经是暂停状态, 忽略重复命令
             */
            need_vol_restore = 0;    /* 暂停时不需要音量恢复 */
            g_seek_active = 0;       /* 清除 SEEK 活动标记 */

            /*
             * 仅当从非暂停状态进入时才启动淡出:
             * 防止连续收到 PAUSE 命令时重复触发淡出
             */
            if (prev_cmd != PLAYMUSIC_PAUSE
             && prev_cmd != PLAYMUSIC_FADE_PAUSE)
            {
                fade_vol = g_volume;           /* 从当前音量开始淡出 */
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_PAUSE;  /* 切换到淡出后暂停 */
                feed_data = 1;                 /* 淡出过程中继续喂数据 */
            }
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_NEXT:
            /* 下一首命令: 直接触发挥发淡出 → 切歌流程 */

            need_vol_restore = 0;    /* 清除音量恢复状态 */
            g_seek_active = 0;       /* 清除 SEEK 活动标记 */
            g_song_bitrate = 0;      /* 复位码率信息 */
            g_reset_progress = 1;    /* 通知 UI 进度条复位 */

            /*
             * 随机模式: 用 pick_random_song() 预选一首随机歌曲,
             * 并设置 s_skip_fade_advance 标志, 让淡出结束后跳过
             * 默认的 next 遍历, 直接使用预选的结果
             */
            if (g_play_mode == PLAY_MODE_SHUFFLE)
            {
                File_MusicNode_t *r = pick_random_song();
                if (r != NULL)
                {
                    g_file_current_music = r;          /* 直接设置随机选中的歌曲 */
                    s_skip_fade_advance = 1;            /* 标记: 跳过淡出后的默认切换 */
                    File_SavePlaylist();                 /* 保存播放列表 */
                }
            }

            /* 启动淡出流程 */
            fade_vol = g_volume;
            fade_tick = HAL_GetTick();
            s_cmd = PLAYMUSIC_FADE_NEXT;   /* 淡出后自动切换到下一首 */
            feed_data = 1;
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_PREV:
            /* 上一首命令 */
            need_vol_restore = 0;
            g_seek_active = 0;
            g_song_bitrate = 0;
            g_reset_progress = 1;

            if (g_play_mode == PLAY_MODE_SHUFFLE)
            {
                /*
                 * 随机模式下, "上一首" 也随机选取一首:
                 * 因为随机模式没有 "上一首" 的概念, 统一随机
                 */
                File_MusicNode_t *r = pick_random_song();
                if (r != NULL)
                {
                    g_file_current_music = r;
                    s_skip_fade_advance = 1;    /* 跳过默认遍历 */
                }
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_NEXT;    /* 随机模式下 prev 等同于 next */
            }
            else
            {
                /*
                 * 顺序/循环模式下, 正常向前遍历:
                 * 淡出后进入 FADE_PREV 逻辑, 走 prev 指针
                 */
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_PREV;    /* 淡出后切到上一首 */
            }
            feed_data = 1;
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_SEEK:
            /*
             * 拖动进度条 (SEEK) 命令:
             *   1. 将 VS1003 音量置为最大 (静音)
             *   2. 根据目标时间和码率计算文件偏移
             *   3. 跳转到估算位置后, 搜索 MPEG 帧同步字对齐
             *   4. 设置音量恢复计数器, 切换回 PLAY 状态
             */

            /* 先将音量调到最大 (静音), 防止跳转过程中的噪音 */
            VS1003_SetVolume(VOL_MAX);

            {
                /*
                 * 计算文件偏移: target_byte = seek_time × bitrate / 8
                 *   g_song_seek_target: 用户选择的播放时间 (秒)
                 *   bps: 码率转换为字节/秒 (kbps × 1000 / 8)
                 */
                uint32_t bps = (uint32_t)g_song_bitrate * 1000 / 8;
                if (bps == 0) bps = 16000;   /* 防御: 如果码率为 0, 使用 128kbps */

                /* 从音频数据起始位置 + 时间偏移 = 新文件位置 */
                f_lseek(&file, g_song_audio_start
                              + (FSIZE_t)g_song_seek_target * bps);
                /* 搜索 MPEG 帧同步字, 对齐到有效帧头 */
                find_mpeg_sync(&file);
            }

            bytes_count = 0;                    /* 复位字节计数 */
            s_skip_metadata_update = 1;          /* 标记跳过元数据更新 */
            need_vol_restore = FLUSH_CHUNKS;     /* 置音量恢复计数器 */

            s_cmd = PLAYMUSIC_PLAY;              /* 跳转完成后切换回播放状态 */
            feed_data = 1;
            break;

        /* ------------------------------------------------------------ */
        case PLAYMUSIC_REINIT:
            /*
             * 重新初始化命令:
             * 用于 USB 拔出后重新扫描 SD 卡, 重建播放列表
             */
            f_close(&file);                 /* 关闭当前打开的文件 */
            opened_node = NULL;             /* 清空打开记录 */
            File_ReinitAfterUSB();          /* USB 拔出后重新初始化文件系统 */
            s_cmd = PLAYMUSIC_PAUSE;        /* 进入暂停等待用户操作 */
            feed_data = 0;                  /* 不需要喂数据 */
            break;

        /* ------------------------------------------------------------ */
        default:
            /* 未知命令, 忽略 */
            break;
        }

        /* ============================================================ */
        /*   数据喂送阶段: 如果需要, 则从文件读取数据并写入 VS1003      */
        /* ============================================================ */

        if (feed_data)
        {
            /*
             * 防御检查: 若当前歌曲为空或不可播放,
             * 触发淡出 → 下一首流程
             */
            if (g_file_current_music == NULL
             || g_file_current_music->if_play == 0)
            {
                g_song_bitrate = 0;         /* 复位码率 */
                g_reset_progress = 1;       /* 通知 UI 复位进度条 */
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_NEXT; /* 启动淡出切歌 */
                continue;                    /* 跳过本轮的喂数据 */
            }

            /*
             * 核心喂数据操作:
             * 从当前打开的文件读取 32 字节, 写入 VS1003 数据端口,
             * 同时处理音量恢复和文件结束等逻辑
             */
            feed_audio_data(&file, streamBuff, &br,
                            &need_vol_restore, &fade_vol,
                            &fade_tick, &s_cmd);
        }
    }
}
