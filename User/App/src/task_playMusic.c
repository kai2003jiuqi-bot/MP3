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

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "ff.h"
#include "app.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include "file.h"
#include "vs1003.h"
#include "TCHAR_Sring.h"
#include "cover_art.h"

/** @brief 淡出间隔 (毫秒) */
#define FADE_STEP_MS      2U
/** @brief 每次淡出步进 (音量值) */
#define FADE_STEP_VOL     5U
/** @brief 切歌后冲刷 VS1003 缓冲的块数 (每块 32 字节) */
#define FLUSH_CHUNKS      64U

/** @brief 当前播放控制命令 */
static STATE_PLAYMUSIC s_cmd = PLAYMUSIC_PAUSE;

/** @brief SHUFFLE 模式下跳过 FADE 的 g_file_current_music 前进 */
static uint8_t s_skip_fade_advance = 0;

/** @brief SEEK 后跳过 update_song_metadata (当前未使用, 保留) */
static uint8_t s_skip_metadata_update = 0;

/* ==================================================================== */
/*               全局变量定义                                             */
/* ==================================================================== */

TaskHandle_t  g_task_play_music;
QueueHandle_t g_queue_play_music;

/* ==================================================================== */
/*               内部函数声明                                             */
/* ==================================================================== */

static void skip_id3_tag(FIL *file);
static void find_mpeg_sync(FIL *file);
static File_MusicNode_t *pick_random_song(void);
static void update_song_metadata(FIL *file);
static void feed_audio_data(FIL *file, uint8_t *buff, UINT *br,
                            uint8_t *need_vol_restore, uint8_t *fade_vol,
                            uint32_t *fade_tick, STATE_PLAYMUSIC *cmd);

/* ==================================================================== */
/*               ID3v2 标签跳过                                          */
/* ==================================================================== */

static void skip_id3_tag(FIL *file)
{
    uint8_t header[10];
    UINT    br;
    FSIZE_t fpos = f_tell(file);

    if (f_read(file, header, 10, &br) != FR_OK || br < 10)
    {
        f_lseek(file, fpos);
        return;
    }

    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3')
    {
        uint32_t tag_size = ((uint32_t)header[6] << 21)
                          | ((uint32_t)header[7] << 14)
                          | ((uint32_t)header[8] << 7)
                          |  (uint32_t)header[9];
        f_lseek(file, fpos + 10 + tag_size);
        printf("skip ID3v2, %lu bytes\r\n", (unsigned long)tag_size);
    }
    else
    {
        f_lseek(file, fpos);
    }
}

/* ==================================================================== */
/*               MPEG 帧同步字搜索                                       */
/* ==================================================================== */

static void find_mpeg_sync(FIL *file)
{
    uint8_t byte;
    uint8_t prev = 0;
    UINT    br;

    while (f_read(file, &byte, 1, &br) == FR_OK && br == 1)
    {
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

static File_MusicNode_t *pick_random_song(void)
{
    int total = 0;
    File_MusicNode_t *p = g_file_music_head;
    if (p == NULL) return NULL;

    do {
        if (p->if_play) total++;
        p = p->next;
    } while (p != g_file_music_head);

    if (total == 0) return NULL;

    int r = rand() % total;
    p = g_file_music_head;
    while (1)
    {
        if (p->if_play) { if (r == 0) break; r--; }
        p = p->next;
    }

    if (total > 1 && p == g_file_current_music)
    {
        do { p = p->next; } while (p->if_play == 0);
    }
    return p;
}

/* ==================================================================== */
/*               歌曲元数据更新                                          */
/* ==================================================================== */

static void update_song_metadata(FIL *file)
{
    g_song_file_size   = f_size(file);
    g_song_audio_start = f_tell(file);

    uint8_t   fh[4];
    UINT      br;
    FSIZE_t   pos = f_tell(file);
    g_song_bitrate = 0;

    if (f_read(file, fh, 4, &br) == FR_OK && br == 4)
    {
        f_lseek(file, pos);
        if (fh[0] == 0xFF && (fh[1] & 0xE0) == 0xE0)
        {
            static const uint16_t bitrates[] = {
                0, 32, 40, 48, 56, 64, 80, 96, 112,
                128, 160, 192, 224, 256, 320, 0
            };
            uint8_t idx = (fh[2] >> 4) & 0x0F;
            if (idx < 16) g_song_bitrate = bitrates[idx];
        }
    }

    if (g_song_bitrate == 0) g_song_bitrate = 128;

    g_song_decode_base = VS1003_GetDecodeTime();
}

/* ==================================================================== */
/*               音频数据喂送                                            */
/* ==================================================================== */

static void feed_audio_data(FIL *file, uint8_t *buff, UINT *br,
                            uint8_t *need_vol_restore, uint8_t *fade_vol,
                            uint32_t *fade_tick, STATE_PLAYMUSIC *cmd)
{
    if (f_read(file, buff, 32, br) != FR_OK || *br == 0)
    {
        if (g_play_mode == PLAY_MODE_ONE)
        {
            f_lseek(file, g_song_audio_start);
        }
        else
        {
            g_reset_progress = 1;
            *fade_vol = g_volume;
            *fade_tick = HAL_GetTick();
            *cmd = PLAYMUSIC_FADE_NEXT;
        }
        return;
    }

    VS1003_WriteMusicData(buff, *br);

    if (*need_vol_restore > 0)
    {
        (*need_vol_restore)--;
        if (*need_vol_restore == 0)
        {
            VS1003_SetVolume(g_volume);
        }
    }
}

/* ==================================================================== */
/*               播放任务主函数                                          */
/* ==================================================================== */

void task_play_music(void *pvParameters)
{
    (void)pvParameters;

    printf("here\r\n");

    uint8_t  streamBuff[32];
    uint32_t bytes_count = 0;
    FIL      file;
    UINT     br;
    uint32_t lastTime = 0;

    uint8_t  fade_vol;
    uint8_t  vol_faded = 0;
    uint8_t  need_vol_restore = 0;
    uint32_t fade_tick = 0;

    VS1003_Init(g_volume);
    File_Init();

    File_RestorePlaylist();
    File_RestoreConfig();
    VS1003_SetVolume(g_volume);

    File_MusicNode_t *opened_node = NULL;

    if (g_file_current_music == NULL || g_file_current_music->if_play == 0)
    {
        printf("no song to play, entering PAUSE\r\n");
        s_cmd = PLAYMUSIC_PAUSE;
        opened_node = NULL;
    }
    else
    {
        cover_art_decode(g_file_current_music);

        if (f_open(&file, g_file_current_music->unicode_name, FA_READ) != FR_OK)
        {
            printf("open %s error\r\n", g_file_current_music->utf8_name);
            while (1) { }
        }
        printf("open %s success\r\n", g_file_current_music->utf8_name);
        skip_id3_tag(&file);
        update_song_metadata(&file);
        opened_node = g_file_current_music;
    }

    while (1)
    {
        uint8_t feed_data = 0;

        STATE_PLAYMUSIC prev_cmd = s_cmd;
        xQueueReceive(g_queue_play_music, &s_cmd, 0);

        switch (s_cmd)
        {
        /* ============================================================ */
        case PLAYMUSIC_PLAY:
            if (g_file_current_music->if_play == 0)
            {
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_NEXT;
                feed_data = 1;
                break;
            }

            if (g_file_current_music != opened_node)
            {
                cover_art_decode(g_file_current_music);
                f_close(&file);
                if (f_open(&file, g_file_current_music->unicode_name, FA_READ) != FR_OK)
                {
                    printf("open %s error\r\n", g_file_current_music->utf8_name);
                    while (1) { }
                }
                skip_id3_tag(&file);
                update_song_metadata(&file);
                opened_node = g_file_current_music;
                bytes_count = 0;
                need_vol_restore = FLUSH_CHUNKS;
            }

            if (need_vol_restore == 0)
            {
                VS1003_SetVolume(g_volume);
            }
            vol_faded = 0;
            feed_data = 1;
            break;

        /* ============================================================ */
        case PLAYMUSIC_FADE_IN:
            feed_data = 1;
            if (HAL_GetTick() - fade_tick >= FADE_STEP_MS)
            {
                fade_tick = HAL_GetTick();
                if (fade_vol > g_volume + FADE_STEP_VOL)
                {
                    fade_vol -= FADE_STEP_VOL;
                }
                else
                {
                    fade_vol = g_volume;
                }
                VS1003_SetVolume(fade_vol);
                if (fade_vol <= g_volume)
                {
                    s_cmd = PLAYMUSIC_PLAY;
                }
            }
            break;

        /* ============================================================ */
        case PLAYMUSIC_FADE_PAUSE:
        case PLAYMUSIC_FADE_NEXT:
        case PLAYMUSIC_FADE_PREV:
            feed_data = 1;
            if (HAL_GetTick() - fade_tick >= FADE_STEP_MS)
            {
                fade_tick = HAL_GetTick();
                fade_vol += FADE_STEP_VOL;
                if (fade_vol > VOL_MAX) fade_vol = VOL_MAX;
                VS1003_SetVolume(fade_vol);

                if (fade_vol >= VOL_MAX)
                {
                    if (s_cmd == PLAYMUSIC_FADE_PAUSE)
                    {
                        s_cmd = PLAYMUSIC_PAUSE;
                        vol_faded = 1;
                        feed_data = 0;
                    }
                    else
                    {
                        if (f_close(&file) != FR_OK)
                        {
                            printf("close %s error\r\n",
                                   g_file_current_music->utf8_name);
                            while (1) { }
                        }

                        if (s_skip_fade_advance)
                        {
                            s_skip_fade_advance = 0;
                        }
                        else
                        {
                            File_MusicNode_t *start = g_file_current_music;
                            if (s_cmd == PLAYMUSIC_FADE_NEXT)
                            {
                                do {
                                    g_file_current_music = g_file_current_music->next;
                                } while (g_file_current_music->if_play == 0
                                      && g_file_current_music != start);
                            }
                            else
                            {
                                do {
                                    g_file_current_music = g_file_current_music->prev;
                                } while (g_file_current_music->if_play == 0
                                      && g_file_current_music != start);
                            }
                        }

                        File_SavePlaylist();

                        if (g_file_current_music->if_play == 0)
                        {
                            s_cmd = PLAYMUSIC_PAUSE;
                            vol_faded = 1;
                            feed_data = 0;
                        }
                        else
                        {
                            cover_art_decode(g_file_current_music);
                            if (f_open(&file,
                                       g_file_current_music->unicode_name,
                                       FA_READ) != FR_OK)
                            {
                                printf("open %s error\r\n",
                                       g_file_current_music->utf8_name);
                                while (1) { }
                            }
                            skip_id3_tag(&file);
                            update_song_metadata(&file);
                            opened_node = g_file_current_music;
                            need_vol_restore = FLUSH_CHUNKS;
                            vol_faded = 0;
                            bytes_count = 0;
                            s_cmd = PLAYMUSIC_PLAY;
                        }
                    }
                }
            }
            break;

        /* ============================================================ */
        case PLAYMUSIC_PAUSE:
            need_vol_restore = 0;
            g_seek_active = 0;
            if (prev_cmd != PLAYMUSIC_PAUSE
             && prev_cmd != PLAYMUSIC_FADE_PAUSE)
            {
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_PAUSE;
                feed_data = 1;
            }
            break;

        /* ============================================================ */
        case PLAYMUSIC_NEXT:
            need_vol_restore = 0;
            g_seek_active = 0;
            g_song_bitrate = 0;
            g_reset_progress = 1;

            if (g_play_mode == PLAY_MODE_SHUFFLE)
            {
                File_MusicNode_t *r = pick_random_song();
                if (r != NULL)
                {
                    g_file_current_music = r;
                    s_skip_fade_advance = 1;
                    File_SavePlaylist();
                }
            }
            fade_vol = g_volume;
            fade_tick = HAL_GetTick();
            s_cmd = PLAYMUSIC_FADE_NEXT;
            feed_data = 1;
            break;

        /* ============================================================ */
        case PLAYMUSIC_PREV:
            need_vol_restore = 0;
            g_seek_active = 0;
            g_song_bitrate = 0;
            g_reset_progress = 1;

            if (g_play_mode == PLAY_MODE_SHUFFLE)
            {
                File_MusicNode_t *r = pick_random_song();
                if (r != NULL)
                {
                    g_file_current_music = r;
                    s_skip_fade_advance = 1;
                }
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_NEXT;
            }
            else
            {
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_PREV;
            }
            feed_data = 1;
            break;

        /* ============================================================ */
        case PLAYMUSIC_SEEK:
            VS1003_SetVolume(VOL_MAX);
            {
                uint32_t bps = (uint32_t)g_song_bitrate * 1000 / 8;
                if (bps == 0) bps = 16000;
                f_lseek(&file, g_song_audio_start
                              + (FSIZE_t)g_song_seek_target * bps);
                find_mpeg_sync(&file);
            }
            bytes_count = 0;
            s_skip_metadata_update = 1;
            need_vol_restore = FLUSH_CHUNKS;
            s_cmd = PLAYMUSIC_PLAY;
            feed_data = 1;
            break;

        /* ============================================================ */
        case PLAYMUSIC_REINIT:
            f_close(&file);
            opened_node = NULL;
            File_ReinitAfterUSB();
            s_cmd = PLAYMUSIC_PAUSE;
            feed_data = 0;
            break;

        default:
            break;
        }

        /* ============================================================ */
        if (feed_data)
        {
            if (g_file_current_music == NULL
             || g_file_current_music->if_play == 0)
            {
                g_song_bitrate = 0;
                g_reset_progress = 1;
                fade_vol = g_volume;
                fade_tick = HAL_GetTick();
                s_cmd = PLAYMUSIC_FADE_NEXT;
                continue;
            }

            feed_audio_data(&file, streamBuff, &br,
                            &need_vol_restore, &fade_vol,
                            &fade_tick, &s_cmd);
        }
    }
}
