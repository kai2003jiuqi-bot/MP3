/**
 * @file file.h
 * @brief 文件系统管理模块头文件 - MP3文件扫描、ID3v2封面解析、播放列表持久化
 *
 * 提供歌曲链表节点定义、文件扫描初始化、以及播放列表保存/恢复接口。
 */
#ifndef FILE_H
#define FILE_H

#include <stdint.h>
#include "ff.h"
#include "FreeRTOS.h"

/* ==================================================================== */
/*                   常量定义                                             */
/* ==================================================================== */

/** @brief 最大文件数量 */
#define FILE_COUNT_MAX          10U

/** @brief 最大文件名长度 (TCHAR) */
#define FILE_NAME_MAX_LEN       50U

/** @brief 播放列表保存魔法数 "PLSV" */
#define FILE_PLAYLIST_MAGIC     0x504C5356UL

/** @brief 播放列表保存版本号 */
#define FILE_PLAYLIST_VERSION   2U

/** @brief 模式配置文件保存魔法数 "MCFG" */
#define FILE_CONFIG_MAGIC       0x4D434647UL

/** @brief 模式配置文件版本号 */
#define FILE_CONFIG_VERSION     1U

/* ==================================================================== */
/*                   枚举类型                                             */
/* ==================================================================== */

/** @brief 内嵌图片类型 */
typedef enum {
    FILE_IMG_NONE = 0,  /*!< 无图片 */
    FILE_IMG_JPEG = 1,  /*!< JPEG 格式 */
    FILE_IMG_PNG  = 2,  /*!< PNG 格式 */
} File_ImageType_t;

/* ==================================================================== */
/*                   结构体类型                                           */
/* ==================================================================== */

/** @brief 歌曲链表节点 */
typedef struct File_MusicNode {
    uint8_t  if_play;                             /*!< 是否添加到播放列表: 0=未添加, 1=已添加 */
    uint8_t  img_type;                            /*!< 封面图片类型: File_ImageType_t */
    uint32_t img_offset;                          /*!< 封面图片数据在文件中的偏移 */
    uint32_t img_size;                            /*!< 封面图片数据大小 */
    TCHAR    unicode_name[FILE_NAME_MAX_LEN];     /*!< Unicode 文件名 */
    uint8_t  utf8_name[FILE_NAME_MAX_LEN];        /*!< UTF-8 文件名 */
    struct File_MusicNode *next;                  /*!< 下一首歌曲 */
    struct File_MusicNode *prev;                  /*!< 上一首歌曲 */
} File_MusicNode_t;

/** @cond COMPATIBILITY */
/* 向后兼容：cover_jpegdec.c / cover_tjpgd.c 仍使用旧名 */
typedef File_MusicNode_t MusicNode_t;
#define IMG_NONE  FILE_IMG_NONE
#define IMG_JPEG  FILE_IMG_JPEG
#define IMG_PNG   FILE_IMG_PNG
/** @endcond */

/** @brief 播放列表序列化结构体 (packed) */
#pragma pack(1)
typedef struct {
    uint32_t magic;                               /*!< 魔法数校验 */
    uint32_t version;                             /*!< 版本号 */
    uint32_t saved_file_count;                    /*!< 保存时的歌曲总数 */
    uint32_t current_index;                       /*!< 当前播放歌曲索引 */
    uint8_t  if_play[FILE_COUNT_MAX];              /*!< 每首歌曲的播放标记 */
} File_PlaylistSave_t;
#pragma pack()

/** @brief 模式配置序列化结构体 (packed) */
#pragma pack(1)
typedef struct {
    uint32_t magic;                               /*!< 魔法数校验 */
    uint32_t version;                             /*!< 版本号 */
    uint32_t play_mode;                           /*!< 播放模式 */
    uint32_t volume;                              /*!< 音量值 */
} File_ConfigSave_t;
#pragma pack()

/* ==================================================================== */
/*                   全局变量声明                                         */
/* ==================================================================== */

extern File_MusicNode_t *g_file_music_head;      /*!< 歌曲链表头指针 */
extern uint32_t          g_file_count;            /*!< 歌曲总数 */
extern File_MusicNode_t *g_file_current_music;    /*!< 当前播放歌曲指针 */

/* ==================================================================== */
/*                   函数声明                                             */
/* ==================================================================== */

/**
 * @brief 初始化文件系统 (挂载 SD 卡和外部 Flash, 扫描 MP3 文件)
 */
void File_Init(void);

/**
 * @brief 释放歌曲链表所有节点内存
 */
void File_FreeList(void);

/**
 * @brief USB 拔出后重新初始化文件系统
 */
void File_ReinitAfterUSB(void);

/**
 * @brief 将播放列表序列化保存到 W25Q64 Flash (歌单 + 当前歌曲索引)
 */
void File_SavePlaylist(void);

/**
 * @brief 从 W25Q64 Flash 恢复上次保存的播放列表
 */
void File_RestorePlaylist(void);

/**
 * @brief 保存播放模式 + 音量到 W25Q64 Flash (1:mode.cfg)
 *
 * 单独保存, 避免按音量键/切换模式时重写整份歌单, 减轻 Flash 写入磨损。
 */
void File_SaveConfig(void);

/**
 * @brief 从 W25Q64 Flash 恢复播放模式 + 音量
 */
void File_RestoreConfig(void);

#endif /* FILE_H */
