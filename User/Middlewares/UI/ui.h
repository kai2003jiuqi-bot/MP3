/**
 * @file ui.h
 * @brief LVGL 用户界面外部声明
 *
 * 提供三个主屏幕（播放器/播放列表/曲库）和一个 USB 传输覆盖屏的全局句柄,
 * 以及屏幕切换/USB 连接的外部接口函数。
 */
#ifndef UI_H
#define UI_H

#include "lvgl.h"

/* ==================================================================== */
/*                   屏幕全局句柄                                         */
/* ==================================================================== */

extern lv_obj_t *g_scr_player;        /*!< 播放器主屏幕 */
extern lv_obj_t *g_scr_playlist;      /*!< 播放列表面板 */
extern lv_obj_t *g_scr_library;       /*!< 曲库面板 */
extern lv_obj_t *g_scr_usb_transfer;  /*!< USB 传输覆盖屏 */

/* ==================================================================== */
/*                   播放器控件句柄                                       */
/* ==================================================================== */

extern lv_obj_t *g_ui_label_lyric;      /*!< 保留: 未来歌词显示用 */
extern lv_obj_t *g_ui_label_song_title; /*!< 当前歌曲歌名标签 */

/* ==================================================================== */
/*                   外部接口                                             */
/* ==================================================================== */

/**
 * @brief 初始化所有 UI 屏幕和控件
 */
void UI_Init(void);

/**
 * @brief 切换到下一屏 (播放器→播放列表→曲库)
 */
void UI_CycleScreen(void);

/**
 * @brief 切换到上一屏
 */
void UI_CycleScreenPrev(void);

/**
 * @brief USB 连接时切换到传输提示屏
 */
void UI_EnterUSBScreen(void);

/**
 * @brief USB 断开时恢复之前的屏幕
 */
void UI_LeaveUSBScreen(void);

#endif /* UI_H */
