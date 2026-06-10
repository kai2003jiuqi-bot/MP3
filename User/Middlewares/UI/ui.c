/**
 * @file    ui.c
 * @brief   LVGL 用户界面实现
 *
 * ========== 界面结构 ==========
 *
 * 四个屏幕, 通过顶部左右箭头切换:
 *
 *   1. 播放器 (player)         — 屏幕 0, 主界面
 *      - 封面图片 (从 MP3 ID3v2 标签解码)
 *      - 歌曲标题滚动文本
 *      - 进度条 (可拖动 SEEK)
 *      - 剩余时间标签
 *      - 频谱可视化柱状条 (伪随机动画, 仅装饰用途)
 *      - 播放模式按钮 (ALL → ONE → SHUFFLE → …)
 *      - 底栏: 上一首/播放暂停/下一首 按钮
 *
 *   2. 播放列表 (playlist)     — 屏幕 1
 *      - 显示当前播放列表中的歌曲
 *      - 支持上下选择
 *      - 短按确认: 从列表中移除歌曲 (if_play = 0)
 *      - 长按: 进入移动模式, 可调整歌曲顺序
 *
 *   3. 曲库 (library)         — 屏幕 2
 *      - 显示 SD 卡所有歌曲
 *      - 支持上下选择
 *      - 短按确认: 将歌曲添加到播放列表 (if_play = 1)
 *
 *   4. USB 传输覆盖屏 (usb_transfer) — 覆盖层
 *      - USB 插入时自动覆盖当前屏幕
 *      - USB 拔出时恢复到之前的屏幕
 *
 * ========== 系统层组件 ==========
 *   音量调节弹窗 (volume_popup)
 *     - 位于 lv_layer_sys() 系统层
 *     - 按键调节音量时显示, 1.5 秒后自动隐藏
 *     - 也可通过触摸滑动调节
 *
 * ========== 定时器 ==========
 *   player_timer (250ms 周期)
 *     - 检测 USB 连接状态变化, 切换 USB 覆盖屏
 *     - 检测屏幕切换请求 (g_switch_screen_next/prev)
 *     - 检测音量显示请求 (g_volume_show)
 *     - 更新封面、进度条、频谱、剩余时间
 *     - 同步播放模式按钮状态
 */

#include "app.h"
#include "file.h"
#include "lvgl.h"
#include "ui.h"
#include "vs1003.h"
#include "cover_art.h"
#include <stdio.h>
#include <string.h>

extern lv_font_t my_font_song_14;

/* ============================ Color palette ============================ */
/*
 * 深色主题配色方案:
 *   CLR_BG          — 背景色 (深蓝黑)
 *   CLR_SURFACE     — 面板表面色
 *   CLR_ACCENT      — 强调色 (青绿)
 *   CLR_ACCENT_DIM  — 强调色暗调
 *   CLR_TEXT        — 主文字色 (白色)
 *   CLR_SUBTEXT     — 副文字色 (灰蓝)
 *   CLR_DANGER      — 危险/警告色 (红色, 用于模式按钮)
 *   CLR_BAR_START   — 频谱条起始色
 *   CLR_BAR_END     — 频谱条结束色
 *   CLR_MOVE        — 移动模式高亮色 (绿色)
 */
#define CLR_BG          lv_color_hex(0x060610)
#define CLR_SURFACE     lv_color_hex(0x141430)
#define CLR_ACCENT      lv_color_hex(0x0FD0C8)
#define CLR_ACCENT_DIM  lv_color_hex(0x0A9088)
#define CLR_TEXT        lv_color_white()
#define CLR_SUBTEXT     lv_color_hex(0x808090)
#define CLR_DANGER      lv_color_hex(0xFF4060)
#define CLR_BAR_START   lv_color_hex(0x0FC8A0)
#define CLR_BAR_END     lv_color_hex(0x0F80E0)
#define CLR_MOVE        lv_color_hex(0x00CC66)

#define N_BARS          12U     /* 频谱条数量 */

/* ============================ Global handles ============================ */
/*
 * 各屏幕对象的全局句柄, 供外部 (如 task_key 的长按切换) 访问
 */
lv_obj_t *g_scr_player;
lv_obj_t *g_scr_playlist;
lv_obj_t *g_scr_library;
lv_obj_t *g_scr_usb_transfer;
lv_obj_t *g_ui_label_lyric;
lv_obj_t *g_ui_label_song_title;

/* ============================ Internal state ============================ */
/*
 * 内部状态变量:
 *   s_current_screen        — 当前屏幕编号 (0/1/2)
 *   s_prev_screen_before_usb — USB 插入前的屏幕编号
 *   s_is_playing            — 播放状态 (用于更新播放/暂停按钮图标)
 *   s_was_in_usb_screen     — 标志是否在 USB 覆盖屏中
 *   s_player_timer          — 定时器, 每 250ms 刷新播放器界面
 *   s_playlist_cont         — 播放列表内容容器
 *   s_library_cont          — 曲库内容容器
 *   s_playlist/lib_selected_idx — 当前选中的列表项索引
 *   s_playlist_move_mode    — 播放列表是否处于移动模式
 *   s_playlist_scroll_ofs   — 播放列表滚动偏移
 *   s_library_scroll_ofs    — 曲库滚动偏移
 *   s_btn_play_pause_label  — 播放/暂停按钮的标签
 *   s_viz_bars[]            — 频谱柱状条对象数组
 *   s_vol_cont/slider       — 音量弹窗
 *   s_progress_bar          — 进度条滑块
 *   s_label_remaining       — 剩余时间标签
 *   s_cover_art_img/icon    — 封面图片和占位图标
 *   s_cover_dsc             — 封面图片的 LVGL 图像描述符
 */
static uint8_t    s_current_screen = 0;
static uint8_t    s_prev_screen_before_usb = 0;
static uint8_t    s_is_playing = 0;
static uint8_t    s_was_in_usb_screen = 0;
static lv_timer_t *s_player_timer;
static lv_obj_t   *s_playlist_cont;
static lv_obj_t   *s_library_cont;
static int         s_playlist_selected_idx = 0;
static int         s_library_selected_idx = 0;
static uint8_t    s_playlist_move_mode = 0;
static uint8_t    s_playlist_ignore_next_click = 0;
static int         s_playlist_scroll_ofs = 0;
static int         s_library_scroll_ofs = 0;
static lv_obj_t   *s_btn_play_pause_label;
static lv_obj_t   *s_viz_bars[N_BARS];
static uint32_t    s_viz_seed = 0x12345678;
static lv_obj_t   *s_dots_player[3];
static lv_obj_t   *s_dots_playlist[3];
static lv_obj_t   *s_dots_library[3];
static lv_obj_t   *s_vol_cont;
static lv_obj_t   *s_vol_slider;
static lv_timer_t *s_vol_hide_timer;
static lv_obj_t   *s_vol_dismiss_overlay;
static lv_obj_t   *s_progress_bar;
static lv_obj_t   *s_label_remaining;
static lv_obj_t   *s_mode_btn_label;
static uint8_t     s_progress_updating = 0;
static lv_obj_t   *s_cover_art_img;
static lv_obj_t   *s_cover_art_icon;

/*
 * 封面图片的 LVGL 图像描述符:
 *   指向 COVER_BUF_ADDR 处的 RGB565 像素数据,
 *   尺寸为 DECODE_W × DECODE_H,
 *   由 cover_art_decode() 从 MP3 ID3v2 标签解码填充。
 */
static const lv_img_dsc_t s_cover_dsc = {
    .header.always_zero = 0,
    .header.w  = DECODE_W,
    .header.h  = DECODE_H,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data_size = COVER_BUF_SIZE,
    .data      = (const uint8_t *)COVER_BUF_ADDR,
};

/* ============================ Styles ============================ */
/*
 * LVGL 样式定义:
 *   s_style_bg              — 背景样式
 *   s_style_dot_active      — 导航点激活样式 (实心)
 *   s_style_dot_inactive    — 导航点非激活样式 (描边)
 *   s_style_circle_btn      — 圆形按钮样式 (音量/上下曲)
 *   s_style_circle_btn_big  — 大圆形按钮样式 (播放/暂停)
 *   s_style_action_btn      — 操作按钮样式 (模式/列表按钮)
 *   s_style_progress        — 进度条样式
 */
static lv_style_t s_style_bg;
static lv_style_t s_style_dot_active;
static lv_style_t s_style_dot_inactive;
static lv_style_t s_style_circle_btn;
static lv_style_t s_style_circle_btn_big;
static lv_style_t s_style_action_btn;
static lv_style_t s_style_progress;

/* ============================ Forward declarations ============================ */
static const char *play_mode_label(uint8_t mode);
static void build_playlist_content(void);
static void build_library_content(void);
static void vol_hide_timer_cb(lv_timer_t *t);
static void vol_slider_cb(lv_event_t *e);
static void destroy_screen_player(void);
static void ensure_screen_player(void);

/* ============================ Style init ============================ */
/*
 * 函数: init_styles
 * 功能: 初始化所有 LVGL 样式对象
 *       在 UI_Init() 中调用, 只执行一次
 */
static void init_styles(void)
{
    lv_style_init(&s_style_bg);
    lv_style_set_bg_color(&s_style_bg, CLR_BG);
    lv_style_set_border_width(&s_style_bg, 0);

    lv_style_init(&s_style_dot_active);
    lv_style_set_bg_color(&s_style_dot_active, CLR_ACCENT);
    lv_style_set_border_width(&s_style_dot_active, 0);
    lv_style_set_radius(&s_style_dot_active, LV_RADIUS_CIRCLE);

    lv_style_init(&s_style_dot_inactive);
    lv_style_set_bg_opa(&s_style_dot_inactive, LV_OPA_TRANSP);
    lv_style_set_border_width(&s_style_dot_inactive, 1);
    lv_style_set_border_color(&s_style_dot_inactive, CLR_SUBTEXT);
    lv_style_set_radius(&s_style_dot_inactive, LV_RADIUS_CIRCLE);

    lv_style_init(&s_style_circle_btn);
    lv_style_set_bg_color(&s_style_circle_btn, CLR_SURFACE);
    lv_style_set_radius(&s_style_circle_btn, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_style_circle_btn, 2);
    lv_style_set_border_color(&s_style_circle_btn, CLR_ACCENT_DIM);
    lv_style_set_text_color(&s_style_circle_btn, CLR_ACCENT);
    lv_style_set_shadow_width(&s_style_circle_btn, 6);
    lv_style_set_shadow_ofs_y(&s_style_circle_btn, 3);
    lv_style_set_shadow_color(&s_style_circle_btn, lv_color_hex(0x000000));

    lv_style_init(&s_style_circle_btn_big);
    lv_style_set_bg_color(&s_style_circle_btn_big, CLR_ACCENT);
    lv_style_set_radius(&s_style_circle_btn_big, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(&s_style_circle_btn_big, 0);
    lv_style_set_text_color(&s_style_circle_btn_big, lv_color_black());
    lv_style_set_shadow_width(&s_style_circle_btn_big, 10);
    lv_style_set_shadow_ofs_y(&s_style_circle_btn_big, 4);
    lv_style_set_shadow_color(&s_style_circle_btn_big, lv_color_hex(0x000000));

    lv_style_init(&s_style_action_btn);
    lv_style_set_bg_color(&s_style_action_btn, CLR_SURFACE);
    lv_style_set_radius(&s_style_action_btn, 6);
    lv_style_set_border_width(&s_style_action_btn, 1);
    lv_style_set_border_color(&s_style_action_btn, CLR_ACCENT_DIM);
    lv_style_set_text_color(&s_style_action_btn, CLR_ACCENT);

    lv_style_init(&s_style_progress);
    lv_style_set_bg_color(&s_style_progress, CLR_SURFACE);
    lv_style_set_radius(&s_style_progress, 4);
    lv_style_set_border_width(&s_style_progress, 0);
}

/* ============================ Screen indicator dots ============================ */
/*
 * 函数: create_screen_dots
 * 功能: 在屏幕顶部创建 3 个导航指示点
 *
 * 指示点位置: (101, 12) 起始, 间距 16px
 * 激活的点使用实心样式, 其余使用描边样式
 *
 * @param scr    所属屏幕对象
 * @param dots   存储 3 个点对象的数组
 * @param active  当前激活的点索引 (0/1/2)
 */
static void create_screen_dots(lv_obj_t *scr, lv_obj_t *dots[3], uint8_t active)
{
    for (int i = 0; i < 3; i++)
    {
        dots[i] = lv_obj_create(scr);
        lv_obj_set_size(dots[i], 7, 7);
        lv_obj_set_pos(dots[i], 101 + i * 16, 12);
        lv_obj_add_style(dots[i],
                         i == active ? &s_style_dot_active : &s_style_dot_inactive, 0);
        lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(dots[i], LV_SCROLLBAR_MODE_OFF);
    }
}

/* ============================ Utility ============================ */
/*
 * 函数: strip_mp3
 * 功能: 移除文件名字符串末尾的 ".mp3" 后缀
 */
static const char *strip_mp3(const uint8_t *name)
{
    static char buf[256];
    strncpy(buf, (const char *)name, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    if (len > 4 && strcmp(buf + len - 4, ".mp3") == 0)
        buf[len - 4] = '\0';
    return buf;
}

/* ============================ Screen cycle ============================ */
/*
 * 函数: UI_CycleScreen / UI_CycleScreenPrev
 * 功能: 在三个主屏幕间切换
 *
 * 切换逻辑:
 *   1. 递增/递减当前屏幕编号 (0→1→2→0 或 0←2←1←0)
 *   2. 新屏幕不存在则创建, 存在则加载
 *   3. 旧屏幕销毁或清空内容以释放资源
 *   4. player 屏幕完全销毁再重建 (包含封面图片等复杂内容)
 *   5. playlist/library 只清空内容容器 (框架保留)
 *
 * 触发: 长按音量±键 → g_switch_screen_next/prev 标志
 */
void UI_CycleScreen(void)
{
    uint8_t old_screen = s_current_screen;
    s_current_screen = (s_current_screen + 1) % 3;

    switch (s_current_screen)
    {
    case 0: ensure_screen_player(); lv_scr_load(g_scr_player);   break;
    case 1: build_playlist_content();  lv_scr_load(g_scr_playlist); break;
    case 2: build_library_content();   lv_scr_load(g_scr_library);  break;
    default: break;
    }

    if (old_screen == 0)
        destroy_screen_player();
    else if (old_screen == 1)
        lv_obj_clean(s_playlist_cont);
    else if (old_screen == 2)
        lv_obj_clean(s_library_cont);
}

void UI_CycleScreenPrev(void)
{
    uint8_t old_screen = s_current_screen;
    s_current_screen = (s_current_screen + 2) % 3;

    switch (s_current_screen)
    {
    case 0: ensure_screen_player(); lv_scr_load(g_scr_player);   break;
    case 1: build_playlist_content();  lv_scr_load(g_scr_playlist); break;
    case 2: build_library_content();   lv_scr_load(g_scr_library);  break;
    default: break;
    }

    if (old_screen == 0)
        destroy_screen_player();
    else if (old_screen == 1)
        lv_obj_clean(s_playlist_cont);
    else if (old_screen == 2)
        lv_obj_clean(s_library_cont);
}

/* ============================ Remaining label position ============================ */
/*
 * 函数: update_remaining_label_pos
 * 功能: 根据进度条滑块当前值, 动态更新剩余时间标签的位置
 *
 * 剩余时间标签跟随进度条滑块旋钮移动,
 * 始终保持在旋钮右侧, 且不超出进度条边界。
 */
static void update_remaining_label_pos(void)
{
    uint8_t pct = lv_slider_get_value(s_progress_bar);
    lv_coord_t knob_x = 10 + 170 * pct / 100;
    lv_coord_t label_x = knob_x + 12;
    if (label_x + 50 > 188) label_x = 188 - 50;
    lv_obj_set_x(s_label_remaining, label_x);
}

static void progress_label_cb(lv_event_t *e)
{
    (void)e;
    if (s_progress_updating) return;
    update_remaining_label_pos();
}

/* ============================ Player timer ============================ */
/*
 * 函数: player_timer_cb
 * 功能: LVGL 定时器回调, 每 250ms 执行一次
 *
 * 本函数是 UI 更新的核心, 包含以下几个模块:
 *
 *   A. USB 连接状态检测
 *      - 插入: 切换到 USB 覆盖屏
 *      - 拔出: 恢复到之前的屏幕
 *
 *   B. 屏幕切换请求处理
 *      - 长按音量±键设置的 g_switch_screen_next/prev
 *
 *   C. 音量调节弹窗
 *      - g_volume_show 标记由 task_key 设置
 *      - 显示音量弹窗, 1.5s 后自动隐藏
 *
 *   D. 播放器界面更新 (仅 player 屏幕可见时)
 *      - 模式按钮文字同步
 *      - 封面显示/隐藏
 *      - 歌曲标题 (滚动文字)
 *      - 进度条 (基于 VS1003 解码时间)
 *      - 剩余时间
 *      - 频谱柱状条 (伪随机动画)
 *      - 播放/暂停按钮状态同步
 */
static void player_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* ---- A. USB 连接状态检测 ---- */

    /* USB 插入: 切换到 USB 覆盖屏 */
    if (g_usb_connected && !s_was_in_usb_screen)
    {
        s_was_in_usb_screen = 1;
        s_prev_screen_before_usb = s_current_screen;
        lv_scr_load(g_scr_usb_transfer);

        if (s_prev_screen_before_usb == 0)
            destroy_screen_player();
        else if (s_prev_screen_before_usb == 1)
            lv_obj_clean(s_playlist_cont);
        else if (s_prev_screen_before_usb == 2)
            lv_obj_clean(s_library_cont);
    }
    /* USB 拔出: 恢复到之前的屏幕 */
    else if (!g_usb_connected && s_was_in_usb_screen)
    {
        s_was_in_usb_screen = 0;
        s_is_playing = 0;

        switch (s_prev_screen_before_usb)
        {
        case 0:
            ensure_screen_player();
            lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PLAY);
            s_progress_updating = 1;
            lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
            s_progress_updating = 0;
            lv_label_set_text(s_label_remaining, "-0:00");
            update_remaining_label_pos();
            lv_scr_load(g_scr_player);
            s_current_screen = 0;
            break;
        case 1:
            build_playlist_content();
            lv_scr_load(g_scr_playlist);
            s_current_screen = 1;
            break;
        case 2:
            build_library_content();
            lv_scr_load(g_scr_library);
            s_current_screen = 2;
            break;
        default:
            ensure_screen_player();
            lv_scr_load(g_scr_player);
            s_current_screen = 0;
            break;
        }
    }

    /* ---- B. 屏幕切换请求 ---- */
    if (g_switch_screen_next && !g_usb_connected)
    {
        g_switch_screen_next = 0;
        UI_CycleScreen();
    }
    if (g_switch_screen_prev && !g_usb_connected)
    {
        g_switch_screen_prev = 0;
        UI_CycleScreenPrev();
    }

    /* ---- C. 音量调节弹窗 ---- */
    if (g_volume_show)
    {
        g_volume_show = 0;
        lv_slider_set_value(s_vol_slider, VOL_MAX - g_volume, LV_ANIM_OFF);
        lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_vol_dismiss_overlay, LV_OBJ_FLAG_HIDDEN);
        if (s_vol_hide_timer) lv_timer_del(s_vol_hide_timer);
        s_vol_hide_timer = lv_timer_create(vol_hide_timer_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_vol_hide_timer, 1);
    }

    /* player 屏幕不存在时跳过后续更新 */
    if (g_scr_player == NULL) return;

    /* ---- D. 播放器界面更新 ---- */

    /* 同步模式按钮文字 */
    lv_label_set_text(s_mode_btn_label, play_mode_label(g_play_mode));
    lv_obj_set_style_text_color(s_mode_btn_label,
        g_play_mode == PLAY_MODE_ALL ? CLR_ACCENT : CLR_DANGER, 0);

    /* 封面图片更新 */
    if (s_cover_art_img)
    {
        if (g_cover_valid)
        {
            lv_img_set_src(s_cover_art_img, &s_cover_dsc);
            lv_img_set_pivot(s_cover_art_img, 0, 0);
            lv_obj_clear_flag(s_cover_art_img, LV_OBJ_FLAG_HIDDEN);
            if (s_cover_art_icon)
                lv_obj_add_flag(s_cover_art_icon, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(s_cover_art_img, LV_OBJ_FLAG_HIDDEN);
            if (s_cover_art_icon)
                lv_obj_clear_flag(s_cover_art_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* 正在播放且歌曲有效 */
    if (s_is_playing
     && g_file_current_music != NULL
     && g_file_current_music->if_play)
    {
        lv_label_set_text(g_ui_label_song_title,
                          strip_mp3(g_file_current_music->utf8_name));

        /* 进度条更新 (仅在非 SEEK 状态) */
        if (g_seek_active)
        {
            /* SEEK 进行中, 保持进度条当前值不变 */
        }
        else if (g_reset_progress)
        {
            /* 新歌/切歌: 复位进度条 */
            g_reset_progress = 0;
            s_progress_updating = 1;
            lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
            s_progress_updating = 0;
            lv_label_set_text(s_label_remaining, "-0:00");
            update_remaining_label_pos();
        }
        else if (g_song_bitrate > 0 && g_song_file_size > 0)
        {
            /*
             * 正常播放: 计算播放进度
             *   总秒数 = 文件大小(bytes) × 8 / 码率(kbps) / 1000
             *   已播秒数 = 当前解码时间 - 解码基值
             *   进度 % = 已播秒数 / 总秒数 × 100
             */
            uint32_t duration = (uint32_t)g_song_file_size * 8
                              / g_song_bitrate / 1000;
            if (duration > 0)
            {
                uint32_t song_pos = (uint32_t)VS1003_GetDecodeTime()
                                  - g_song_decode_base;
                if (song_pos > duration) song_pos = duration;
                uint8_t pct = (uint8_t)(song_pos * 100 / duration);
                if (pct > 100) pct = 100;

                s_progress_updating = 1;
                lv_slider_set_value(s_progress_bar, pct, LV_ANIM_OFF);
                s_progress_updating = 0;

                /* 更新剩余时间标签 */
                char buf[8];
                uint32_t remain = duration - song_pos;
                snprintf(buf, sizeof(buf), "-%u:%02u",
                         remain / 60, remain % 60);
                lv_label_set_text(s_label_remaining, buf);
                update_remaining_label_pos();
            }
        }
        else
        {
            /* 无码率信息, 进度条保持 0 */
            s_progress_updating = 1;
            lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
            s_progress_updating = 0;
            lv_label_set_text(s_label_remaining, "-0:00");
            update_remaining_label_pos();
        }

        /* 频谱柱状条动画 (伪随机, 纯装饰) */
        for (int i = 0; i < N_BARS; i++)
        {
            s_viz_seed = s_viz_seed * 1103515245 + 12345;
            lv_coord_t h = 1 + ((s_viz_seed >> 16) % 14);
            lv_obj_set_size(s_viz_bars[i], 10, h);
            lv_obj_set_y(s_viz_bars[i], 198 - h);
        }
    }
    /* 歌曲有效但暂停 */
    else if (g_file_current_music != NULL && g_file_current_music->if_play)
    {
        lv_label_set_text(g_ui_label_song_title,
                          strip_mp3(g_file_current_music->utf8_name));
        /* 频谱条缩到最小 */
        for (int i = 0; i < N_BARS; i++)
        {
            lv_obj_set_size(s_viz_bars[i], 10, 1);
            lv_obj_set_y(s_viz_bars[i], 198 - 1);
        }
    }
    /* 无有效歌曲: 清空界面 */
    else
    {
        lv_label_set_text(g_ui_label_song_title, "");
        s_progress_updating = 1;
        lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
        s_progress_updating = 0;
        lv_label_set_text(s_label_remaining, "-0:00");
        update_remaining_label_pos();
        for (int i = 0; i < N_BARS; i++)
        {
            lv_obj_set_size(s_viz_bars[i], 10, 1);
            lv_obj_set_y(s_viz_bars[i], 198 - 1);
        }
        if (s_is_playing)
        {
            lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PLAY);
            s_is_playing = 0;
        }
    }
}

/* ============================ Volume popup ============================ */
/*
 * 系统层音量调节弹窗:
 *   当用户按音量键或触摸滑动时显示,
 *   1.5 秒无操作后自动隐藏。
 */
static void vol_hide(void)
{
    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_vol_dismiss_overlay, LV_OBJ_FLAG_HIDDEN);
    if (s_vol_hide_timer)
    {
        lv_timer_del(s_vol_hide_timer);
        s_vol_hide_timer = NULL;
    }
}

static void vol_dismiss_cb(lv_event_t *e)
{
    (void)e;
    vol_hide();
}

static void vol_hide_timer_cb(lv_timer_t *t)
{
    (void)t;
    vol_hide();
}

static void vol_slider_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *slider = lv_event_get_target(e);
    int16_t val = lv_slider_get_value(slider);
    g_volume = (uint8_t)(VOL_MAX - val);
    VS1003_SetVolume(g_volume);
    // File_SaveConfig();   /* 注释: 音量由 task_key 保存 */

    /* 重置自动隐藏定时器 */
    if (s_vol_hide_timer) lv_timer_del(s_vol_hide_timer);
    s_vol_hide_timer = lv_timer_create(vol_hide_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(s_vol_hide_timer, 1);
}

/* ============================ Progress bar seek ============================ */
/*
 * 函数: progress_seek_cb
 * 功能: 进度条释放事件回调, 触发 SEEK 操作
 *
 * 用户拖动进度条到新位置后:
 *   1. 根据当前码率和文件大小计算总时长
 *   2. 根据进度条百分比计算目标时间 (秒)
 *   3. 设置 g_song_seek_target 和 g_seek_active
 *   4. 发送 PLAYMUSIC_SEEK 命令到播放任务
 */
static void progress_seek_cb(lv_event_t *e)
{
    (void)e;
    if (g_song_bitrate == 0 || g_song_file_size == 0) return;

    uint32_t duration = (uint32_t)g_song_file_size * 8
                      / g_song_bitrate / 1000;
    if (duration == 0) return;

    uint8_t pct = lv_slider_get_value(s_progress_bar);
    g_song_seek_target = (uint32_t)pct * duration / 100;
    g_seek_active = 1;

    STATE_PLAYMUSIC cmd = PLAYMUSIC_SEEK;
    xQueueOverwrite(g_queue_play_music, &cmd);
}

/* ============================ Screen cycle button callbacks ============================ */
static void cycle_btn_cb(lv_event_t *e)
{
    (void)e;
    g_switch_screen_next = 1;
}

static void cycle_btn_prev_cb(lv_event_t *e)
{
    (void)e;
    g_switch_screen_prev = 1;
}

/* ============================ Play mode button ============================ */
static const char *play_mode_label(uint8_t mode)
{
    switch (mode)
    {
    case PLAY_MODE_ONE:     return "ONE";
    case PLAY_MODE_SHUFFLE: return "SHF";
    default:                return "ALL";
    }
}

static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    g_play_mode = (g_play_mode + 1) % 3;
    lv_label_set_text(s_mode_btn_label, play_mode_label(g_play_mode));
    lv_obj_set_style_text_color(s_mode_btn_label,
        g_play_mode == PLAY_MODE_ALL ? CLR_ACCENT : CLR_DANGER, 0);
    // File_SaveConfig();   /* 注释: 保存由其他模块处理 */
}

/* ============================ Music control callbacks ============================ */
static void btn_prev_cb(lv_event_t *e)
{
    (void)e;
    STATE_PLAYMUSIC cmd = PLAYMUSIC_PREV;
    xQueueOverwrite(g_queue_play_music, &cmd);
    lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PAUSE);
    s_is_playing = 1;
}

static void btn_play_pause_cb(lv_event_t *e)
{
    (void)e;
    STATE_PLAYMUSIC cmd;

    if (s_is_playing)
    {
        cmd = PLAYMUSIC_PAUSE;
        lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PLAY);
        s_is_playing = 0;
    }
    else
    {
        cmd = PLAYMUSIC_PLAY;
        lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PAUSE);
        s_is_playing = 1;
    }
    xQueueOverwrite(g_queue_play_music, &cmd);
}

static void btn_next_cb(lv_event_t *e)
{
    (void)e;
    STATE_PLAYMUSIC cmd = PLAYMUSIC_NEXT;
    xQueueOverwrite(g_queue_play_music, &cmd);
    lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PAUSE);
    s_is_playing = 1;
}

/* ============================ Playlist navigation ============================ */
/*
 * 函数: playlist_swap_data
 * 功能: 交换两个歌曲节点的所有数据
 *
 * 在播放列表移动模式下, 交换两个节点的所有字段:
 *   utf8_name, unicode_name, img_type, img_offset, img_size
 * 同时更新 g_file_current_music 指针 (如果涉及当前歌曲)
 */
static void playlist_swap_data(File_MusicNode_t *a, File_MusicNode_t *b)
{
    uint8_t tmp_utf8[FILE_NAME_MAX_LEN];
    memcpy(tmp_utf8, a->utf8_name, FILE_NAME_MAX_LEN);
    memcpy(a->utf8_name, b->utf8_name, FILE_NAME_MAX_LEN);
    memcpy(b->utf8_name, tmp_utf8, FILE_NAME_MAX_LEN);

    TCHAR tmp_uni[FILE_NAME_MAX_LEN];
    memcpy(tmp_uni, a->unicode_name, sizeof(TCHAR) * FILE_NAME_MAX_LEN);
    memcpy(a->unicode_name, b->unicode_name, sizeof(TCHAR) * FILE_NAME_MAX_LEN);
    memcpy(b->unicode_name, tmp_uni, sizeof(TCHAR) * FILE_NAME_MAX_LEN);

    uint8_t  tmp_img = a->img_type;
    a->img_type = b->img_type;
    b->img_type = tmp_img;

    uint32_t tmp_off = a->img_offset;
    a->img_offset = b->img_offset;
    b->img_offset = tmp_off;

    uint32_t tmp_sz = a->img_size;
    a->img_size = b->img_size;
    b->img_size = tmp_sz;

    if (g_file_current_music == a)
        g_file_current_music = b;
    else if (g_file_current_music == b)
        g_file_current_music = a;
}

/*
 * 播放列表导航: 上移 / 下移 / 确认删除 / 长按进入移动模式
 */
static void playlist_up_cb(lv_event_t *e)
{
    (void)e;
    if (s_playlist_move_mode)
    {
        /*
         * 移动模式: 与上一首交换数据
         * 找到列表中当前选中项和上一项的节点指针
         */
        if (s_playlist_selected_idx < 1) return;

        File_MusicNode_t *a = g_file_music_head;
        File_MusicNode_t *b = g_file_music_head;
        int cnt = 0;
        File_MusicNode_t *p = g_file_music_head;

        do {
            if (p->if_play)
            {
                if (cnt == s_playlist_selected_idx) a = p;
                if (cnt == s_playlist_selected_idx - 1) b = p;
                cnt++;
            }
            p = p->next;
        } while (p != g_file_music_head);

        playlist_swap_data(a, b);
        s_playlist_selected_idx--;
    }
    else
    {
        /* 普通模式: 上移选中索引 */
        if (s_playlist_selected_idx > 0)
        {
            s_playlist_selected_idx--;
        }
        else
        {
            return;
        }
    }
    build_playlist_content();
}

static void playlist_down_cb(lv_event_t *e)
{
    (void)e;
    /* 统计可播放歌曲总数 */
    int count = 0;
    File_MusicNode_t *p = g_file_music_head;
    if (!p) return;

    do {
        if (p->if_play) count++;
        p = p->next;
    } while (p != g_file_music_head);

    if (s_playlist_move_mode)
    {
        /* 移动模式: 与下一首交换数据 */
        if (s_playlist_selected_idx >= count - 1) return;

        File_MusicNode_t *a = g_file_music_head;
        File_MusicNode_t *b = g_file_music_head;
        int cnt = 0;
        p = g_file_music_head;

        do {
            if (p->if_play)
            {
                if (cnt == s_playlist_selected_idx) a = p;
                if (cnt == s_playlist_selected_idx + 1) b = p;
                cnt++;
            }
            p = p->next;
        } while (p != g_file_music_head);

        playlist_swap_data(a, b);
        s_playlist_selected_idx++;
    }
    else
    {
        /* 普通模式: 下移选中索引 */
        if (s_playlist_selected_idx >= count - 1) return;
        s_playlist_selected_idx++;
    }

    build_playlist_content();
}

/*
 * 函数: playlist_confirm_cb
 * 功能: 播放列表确认按钮回调
 *
 * 普通模式: 删除选中歌曲 (if_play = 0)
 *   如果删除的是当前播放歌曲 → 自动切换到下一首可播放歌曲
 *   如果全部删除完 → 进入暂停
 *
 * 移动模式: 退出移动模式
 */
static void playlist_confirm_cb(lv_event_t *e)
{
    (void)e;
    if (s_playlist_ignore_next_click)
    {
        s_playlist_ignore_next_click = 0;
        return;
    }

    if (s_playlist_move_mode)
    {
        s_playlist_move_mode = 0;
        build_playlist_content();
        return;
    }

    /* 查找选中的歌曲节点 */
    File_MusicNode_t *p = g_file_music_head;
    if (!p) return;

    int idx = 0;
    do {
        if (p->if_play)
        {
            if (idx == s_playlist_selected_idx)
            {
                p->if_play = 0;   /* 标记为不可播放 */

                /* 如果删除的是当前播放歌曲, 自动跳到下一首 */
                if (p == g_file_current_music)
                {
                    File_MusicNode_t *n = p->next;
                    while (n != p && n->if_play == 0) n = n->next;
                    if (n->if_play) g_file_current_music = n;

                    STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
                    xQueueOverwrite(g_queue_play_music, &cmd);
                    s_is_playing = 0;
                }

                /* 统计剩余可播放歌曲数, 修正选中索引 */
                int rem = 0;
                File_MusicNode_t *q = g_file_music_head;
                do {
                    if (q->if_play) rem++;
                    q = q->next;
                } while (q != g_file_music_head);

                if (rem == 0)
                {
                    s_playlist_selected_idx = 0;
                }
                else
                {
                    if (s_playlist_selected_idx >= rem)
                        s_playlist_selected_idx = rem - 1;
                }

                build_playlist_content();
                File_SavePlaylist();
                return;
            }
            idx++;
        }
        p = p->next;
    } while (p != g_file_music_head);
}

/*
 * 长按播放列表确认按钮 → 进入移动模式
 * 移动模式下可调整歌曲顺序
 */
static void playlist_confirm_long_cb(lv_event_t *e)
{
    (void)e;
    if (!s_playlist_move_mode)
    {
        s_playlist_move_mode = 1;
        s_playlist_ignore_next_click = 1;
        build_playlist_content();
    }
}

/* ============================ Library navigation ============================ */
/*
 * 曲库导航: 在 SD 卡所有歌曲中浏览和添加
 */
static void library_up_cb(lv_event_t *e)
{
    (void)e;
    if (s_library_selected_idx > 0)
    {
        s_library_selected_idx--;
        build_library_content();
    }
}

static void library_down_cb(lv_event_t *e)
{
    (void)e;
    if (s_library_selected_idx < (int)g_file_count - 1)
    {
        s_library_selected_idx++;
        build_library_content();
    }
}

/*
 * 曲库确认: 将选中歌曲添加到播放列表
 * 如果当前没有可播放歌曲, 自动设为当前歌曲
 */
static void library_confirm_cb(lv_event_t *e)
{
    (void)e;
    File_MusicNode_t *p = g_file_music_head;
    if (!p) return;

    int idx = 0;
    do {
        if (idx == s_library_selected_idx)
        {
            p->if_play = 1;
            if (g_file_current_music == NULL
             || g_file_current_music->if_play == 0)
            {
                g_file_current_music = p;
            }
            build_library_content();
            File_SavePlaylist();
            return;
        }
        idx++;
        p = p->next;
    } while (p != g_file_music_head);
}

/* ============================ List row (shared) ============================ */
/*
 * 函数: create_list_row
 * 功能: 创建播放列表/曲库中的一行条目
 *
 * 行布局:
 *   [序号] [歌曲名称]
 *
 * 选中行: 使用强调色背景 + 白色文字
 * 非选中行: 半透明表面色背景 + 灰/白文字
 * 移动模式选中行: 绿色背景 + 白色文字
 */
static lv_obj_t *create_list_row(lv_obj_t *cont, File_MusicNode_t *p,
                                 int idx, lv_coord_t row_y,
                                 uint8_t sel, uint8_t move_mode,
                                 lv_coord_t name_w)
{
    char num[8];
    snprintf(num, sizeof(num), "%d.", idx + 1);

    lv_obj_t *row = lv_obj_create(cont);
    lv_obj_set_size(row, 220, 30);
    lv_obj_set_pos(row, 10, row_y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    if (move_mode && sel)
    {
        lv_obj_set_style_bg_color(row, CLR_MOVE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(row, sel ? CLR_ACCENT : CLR_SURFACE, 0);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : LV_OPA_30, 0);
    }

    lv_obj_t *nl = lv_label_create(row);
    lv_label_set_text(nl, num);
    if (move_mode && sel)
        lv_obj_set_style_text_color(nl, lv_color_white(), 0);
    else
        lv_obj_set_style_text_color(nl, sel ? lv_color_black() : CLR_SUBTEXT, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, strip_mp3(p->utf8_name));
    if (move_mode && sel)
        lv_obj_set_style_text_color(nm, lv_color_white(), 0);
    else
        lv_obj_set_style_text_color(nm, sel ? lv_color_black() : CLR_TEXT, 0);
    lv_obj_set_style_text_font(nm, &my_font_song_14, 0);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 32, 0);
    lv_obj_set_width(nm, name_w);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_CLIP);

    return row;
}

/* ============================ Build playlist content ============================ */
static void build_playlist_content(void)
{
    lv_obj_clean(s_playlist_cont);

    if (g_file_music_head == NULL || g_file_count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_playlist_cont);
        lv_label_set_text(empty, "Playlist empty");
        lv_obj_center(empty);
        lv_obj_set_style_text_color(empty, CLR_SUBTEXT, 0);
        return;
    }

    s_playlist_scroll_ofs = (s_playlist_selected_idx - 1) * 34;
    int scroll_ofs = s_playlist_scroll_ofs;

    File_MusicNode_t *p = g_file_music_head;
    int idx = 0;

    do {
        if (p->if_play)
        {
            uint8_t sel = (idx == s_playlist_selected_idx);
            lv_coord_t row_y = 2 + idx * 34 - scroll_ofs;

            create_list_row(s_playlist_cont, p, idx, row_y,
                            sel, s_playlist_move_mode, 175);
            idx++;
        }
        p = p->next;
    } while (p != g_file_music_head);
}

/* ============================ Build library content ============================ */
static void build_library_content(void)
{
    lv_obj_clean(s_library_cont);

    if (g_file_music_head == NULL || g_file_count == 0)
    {
        lv_obj_t *empty = lv_label_create(s_library_cont);
        lv_label_set_text(empty, "No songs found");
        lv_obj_center(empty);
        lv_obj_set_style_text_color(empty, CLR_SUBTEXT, 0);
        return;
    }

    s_library_scroll_ofs = (s_library_selected_idx - 1) * 34;
    int scroll_ofs = s_library_scroll_ofs;

    File_MusicNode_t *p = g_file_music_head;
    int idx = 0;

    do {
        uint8_t sel = (idx == s_library_selected_idx);
        lv_coord_t row_y = 2 + idx * 34 - scroll_ofs;

        lv_obj_t *row = create_list_row(s_library_cont, p, idx, row_y,
                                        sel, 0, 160);

        /* 已添加到播放列表的歌曲显示 ✓ 标记 */
        if (p->if_play)
        {
            lv_obj_t *icon = lv_label_create(row);
            lv_label_set_text(icon, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(icon, sel ? lv_color_black() : CLR_ACCENT, 0);
            lv_obj_align(icon, LV_ALIGN_RIGHT_MID, -6, 0);
        }

        idx++;
        p = p->next;
    } while (p != g_file_music_head);
}

/* ============================ Nav arrows ============================ */
/*
 * 函数: create_nav_arrows
 * 功能: 在每个屏幕顶部创建左右切换箭头按钮
 */
static void create_nav_arrows(lv_obj_t *scr)
{
    lv_obj_t *btn, *lab;

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 36, 28);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 10, 4);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(lab, CLR_SUBTEXT, 0);
    lv_obj_add_event_cb(btn, cycle_btn_prev_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 36, 28);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -10, 4);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(lab, CLR_SUBTEXT, 0);
    lv_obj_add_event_cb(btn, cycle_btn_cb, LV_EVENT_CLICKED, NULL);
}

/* ============================ Screen: Player ============================ */
/*
 * 函数: create_screen_player
 * 功能: 创建播放器屏幕的所有 UI 组件
 *
 * 布局 (240×320):
 *   顶部:   导航箭头 + 指示点 (y=0~20)
 *   封面:   居中显示 (y=42~150)
 *   歌名:   居中滚动文本 (y=168)
 *   频谱:   12 根装饰柱状条 (y=198~212)
 *   进度:   滑条 + 剩余时间 + 模式按钮 (y=208~228)
 *   底栏:   上曲 | 播放暂停 | 下曲 (y~236~300)
 *
 * 注意: 屏幕完全重建, 销毁前需清空所有句柄
 */
static void create_screen_player(void)
{
    g_scr_player = lv_obj_create(NULL);
    lv_obj_add_style(g_scr_player, &s_style_bg, 0);

    create_screen_dots(g_scr_player, s_dots_player, 0);
    create_nav_arrows(g_scr_player);

    /* 封面容器 */
    lv_obj_t *art = lv_obj_create(g_scr_player);
    lv_obj_set_size(art, COVER_W, COVER_H);
    lv_obj_set_pos(art, (240 - COVER_W) / 2, 42);
    lv_obj_set_style_bg_opa(art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_set_style_shadow_width(art, 0, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(art, LV_SCROLLBAR_MODE_OFF);

    /* 封面占位图标 (无封面时显示) */
    s_cover_art_icon = lv_label_create(art);
    lv_label_set_text(s_cover_art_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(s_cover_art_icon);
    lv_obj_set_style_text_color(s_cover_art_icon, CLR_ACCENT, 0);

    /* 封面图片 (有封面时显示) */
    s_cover_art_img = lv_img_create(art);
    lv_obj_set_pos(s_cover_art_img, 0, 0);
    lv_obj_set_size(s_cover_art_img, COVER_W, COVER_H);
    lv_img_set_zoom(s_cover_art_img, 512);
    lv_img_set_pivot(s_cover_art_img, 0, 0);

    if (g_cover_valid)
    {
        lv_img_set_src(s_cover_art_img, &s_cover_dsc);
        lv_img_set_pivot(s_cover_art_img, 0, 0);
        lv_obj_add_flag(s_cover_art_icon, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s_cover_art_img, LV_OBJ_FLAG_HIDDEN);
    }

    /* 歌曲标题 (滚动文本) */
    g_ui_label_song_title = lv_label_create(g_scr_player);
    lv_label_set_text(g_ui_label_song_title, "");
    lv_obj_set_pos(g_ui_label_song_title, 10, 168);
    lv_obj_set_size(g_ui_label_song_title, 220, 24);
    lv_obj_set_style_text_align(g_ui_label_song_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_ui_label_song_title, CLR_TEXT, 0);
    lv_obj_set_style_text_font(g_ui_label_song_title, &my_font_song_14, 0);
    lv_label_set_long_mode(g_ui_label_song_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

    /* 进度条滑块 */
    s_progress_bar = lv_slider_create(g_scr_player);
    lv_obj_set_size(s_progress_bar, 170, 15);
    lv_obj_set_pos(s_progress_bar, 10, 208);
    lv_slider_set_range(s_progress_bar, 0, 100);
    lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(s_progress_bar, CLR_SURFACE, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_progress_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_progress_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_progress_bar, 2, LV_PART_MAIN);

    lv_obj_set_style_bg_color(s_progress_bar, CLR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 3, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(s_progress_bar, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress_bar, 0, LV_PART_KNOB);
    lv_obj_set_style_radius(s_progress_bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_scrollbar_mode(s_progress_bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_progress_bar, progress_seek_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_progress_bar, progress_label_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 剩余时间标签 */
    s_label_remaining = lv_label_create(g_scr_player);
    lv_label_set_text(s_label_remaining, "-0:00");
    lv_obj_set_pos(s_label_remaining, 22, 208);
    lv_obj_set_style_text_color(s_label_remaining, CLR_SUBTEXT, 0);

    /* 播放模式按钮 */
    lv_obj_t *mode_btn = lv_btn_create(g_scr_player);
    lv_obj_set_size(mode_btn, 36, 22);
    lv_obj_set_pos(mode_btn, 190, 205);
    lv_obj_add_style(mode_btn, &s_style_action_btn, 0);
    s_mode_btn_label = lv_label_create(mode_btn);
    lv_label_set_text(s_mode_btn_label, play_mode_label(g_play_mode));
    lv_obj_center(s_mode_btn_label);
    lv_obj_set_style_text_color(s_mode_btn_label,
        g_play_mode == PLAY_MODE_ALL ? CLR_ACCENT : CLR_DANGER, 0);
    lv_obj_add_event_cb(mode_btn, mode_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 频谱柱状条 */
    lv_coord_t bar_area_w = N_BARS * 10 + (N_BARS - 1) * 5;
    lv_coord_t bar_x0 = (240 - bar_area_w) / 2;

    for (int i = 0; i < N_BARS; i++)
    {
        s_viz_bars[i] = lv_obj_create(g_scr_player);
        lv_obj_set_size(s_viz_bars[i], 10, 1);
        lv_obj_set_pos(s_viz_bars[i], bar_x0 + i * 15, 198);
        lv_obj_set_style_bg_color(s_viz_bars[i],
            i < N_BARS / 2 ? CLR_BAR_START : CLR_BAR_END, 0);
        lv_obj_set_style_radius(s_viz_bars[i], 3, 0);
        lv_obj_set_style_border_width(s_viz_bars[i], 0, 0);
        lv_obj_clear_flag(s_viz_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_viz_bars[i], LV_SCROLLBAR_MODE_OFF);
    }

    /* 底栏控制按钮 */
    lv_obj_t *btn, *lab;

    btn = lv_btn_create(g_scr_player);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 30, -52 + 20);
    lv_obj_add_style(btn, &s_style_circle_btn, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_PREV);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, btn_prev_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_btn_create(g_scr_player);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -44 + 20);
    lv_obj_add_style(btn, &s_style_circle_btn_big, 0);
    s_btn_play_pause_label = lv_label_create(btn);
    lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PLAY);
    lv_obj_center(s_btn_play_pause_label);
    lv_obj_add_event_cb(btn, btn_play_pause_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_btn_create(g_scr_player);
    lv_obj_set_size(btn, 50, 50);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -30, -52 + 20);
    lv_obj_add_style(btn, &s_style_circle_btn, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_NEXT);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, btn_next_cb, LV_EVENT_CLICKED, NULL);
}

/*
 * 函数: destroy_screen_player
 * 功能: 销毁播放器屏幕并清空所有关联句柄
 *       防止 LVGL 对象删除后句柄悬挂
 */
static void destroy_screen_player(void)
{
    if (s_vol_hide_timer)
    {
        lv_timer_del(s_vol_hide_timer);
        s_vol_hide_timer = NULL;
    }
    if (g_scr_player)
    {
        lv_obj_del(g_scr_player);
        g_scr_player = NULL;
    }
    g_ui_label_lyric = NULL;
    g_ui_label_song_title = NULL;
    s_btn_play_pause_label = NULL;
    s_progress_bar = NULL;
    s_label_remaining = NULL;
    s_mode_btn_label = NULL;
    s_cover_art_img = NULL;
    s_cover_art_icon = NULL;
    for (int i = 0; i < N_BARS; i++) s_viz_bars[i] = NULL;
    for (int i = 0; i < 3; i++) s_dots_player[i] = NULL;
}

/*
 * 函数: ensure_screen_player
 * 功能: 确保播放器屏幕存在, 不存在则创建并恢复 UI 状态
 */
static void ensure_screen_player(void)
{
    if (g_scr_player != NULL) return;

    create_screen_player();

    if (g_file_current_music != NULL)
    {
        lv_label_set_text(g_ui_label_song_title,
                          strip_mp3(g_file_current_music->utf8_name));
    }
    if (s_is_playing)
    {
        lv_label_set_text(s_btn_play_pause_label, LV_SYMBOL_PAUSE);
    }
    lv_label_set_text(s_mode_btn_label, play_mode_label(g_play_mode));
    lv_obj_set_style_text_color(s_mode_btn_label,
        g_play_mode == PLAY_MODE_ALL ? CLR_ACCENT : CLR_DANGER, 0);

    if (s_cover_art_img && g_cover_valid)
    {
        lv_img_set_src(s_cover_art_img, &s_cover_dsc);
        lv_img_set_pivot(s_cover_art_img, 0, 0);
        lv_obj_clear_flag(s_cover_art_img, LV_OBJ_FLAG_HIDDEN);
        if (s_cover_art_icon)
            lv_obj_add_flag(s_cover_art_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================ List screen shared framework ============================ */
/*
 * 函数: create_list_screen
 * 功能: 创建列表类型屏幕的通用框架 (播放列表和曲库共用)
 *
 * 布局:
 *   顶部: 导航箭头 + 指示点 + 标题 + 装饰线
 *   中部: 可滚动列表容器
 *   底部: 上移 | 确认 | 下移 三个按钮
 *
 * @param scr_out      输出: 屏幕对象指针
 * @param dots         指示点数组
 * @param dot_active   激活的指示点索引
 * @param title_text   屏幕标题文字
 * @param cont_out     输出: 内容容器指针
 * @param cb_up        上移按钮回调
 * @param cb_confirm   确认按钮回调
 * @param cb_down      下移按钮回调
 * @param confirm_sym  确认按钮图标
 * @param cb_long      确认按钮长按回调 (可选, 仅播放列表使用)
 */
static void create_list_screen(lv_obj_t **scr_out, lv_obj_t *dots[3],
                               int dot_active,
                               const char *title_text,
                               lv_obj_t **cont_out,
                               lv_event_cb_t cb_up,
                               lv_event_cb_t cb_confirm,
                               lv_event_cb_t cb_down,
                               const char *confirm_sym,
                               lv_event_cb_t cb_long)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &s_style_bg, 0);
    *scr_out = scr;

    create_screen_dots(scr, dots, dot_active);
    create_nav_arrows(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 40, 3);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_bg_color(line, CLR_ACCENT, 0);
    lv_obj_set_style_radius(line, 2, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(line, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 240, 134);
    lv_obj_set_pos(cont, 0, 82);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    *cont_out = cont;

    lv_obj_t *btn, *lab;

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 50, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, -60, -8);
    lv_obj_add_style(btn, &s_style_action_btn, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_UP);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, cb_up, LV_EVENT_CLICKED, NULL);

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 50, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_style(btn, &s_style_action_btn, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, confirm_sym);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, cb_confirm, LV_EVENT_CLICKED, NULL);
    if (cb_long)
    {
        lv_obj_add_event_cb(btn, cb_long, LV_EVENT_LONG_PRESSED, NULL);
    }

    btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 50, 36);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 60, -8);
    lv_obj_add_style(btn, &s_style_action_btn, 0);
    lab = lv_label_create(btn);
    lv_label_set_text(lab, LV_SYMBOL_DOWN);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, cb_down, LV_EVENT_CLICKED, NULL);
}

/* ============================ Screen: Playlist ============================ */
static void create_screen_playlist(void)
{
    create_list_screen(&g_scr_playlist, s_dots_playlist, 1,
                       "Playlist", &s_playlist_cont,
                       playlist_up_cb,
                       playlist_confirm_cb,
                       playlist_down_cb,
                       LV_SYMBOL_OK,
                       playlist_confirm_long_cb);
}

/* ============================ Screen: Library ============================ */
static void create_screen_library(void)
{
    create_list_screen(&g_scr_library, s_dots_library, 2,
                       "Library", &s_library_cont,
                       library_up_cb,
                       library_confirm_cb,
                       library_down_cb,
                       LV_SYMBOL_PLUS,
                       NULL);
}

/* ============================ Screen: USB Transfer ============================ */
static void create_screen_usb_transfer(void)
{
    g_scr_usb_transfer = lv_obj_create(NULL);
    lv_obj_add_style(g_scr_usb_transfer, &s_style_bg, 0);

    lv_obj_t *icon = lv_label_create(g_scr_usb_transfer);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_center(icon);
    lv_obj_set_y(icon, -30);
    lv_obj_set_style_text_color(icon, CLR_ACCENT, 0);

    lv_obj_t *title = lv_label_create(g_scr_usb_transfer);
    lv_label_set_text(title, "File Transfer");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 20);
    lv_obj_set_style_text_color(title, CLR_TEXT, 0);

    lv_obj_t *sub = lv_label_create(g_scr_usb_transfer);
    lv_label_set_text(sub, "Do not disconnect");
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 50);
    lv_obj_set_style_text_color(sub, CLR_DANGER, 0);
}

void UI_EnterUSBScreen(void)
{
    s_prev_screen_before_usb = s_current_screen;
    lv_scr_load(g_scr_usb_transfer);

    if (s_prev_screen_before_usb == 0)
        destroy_screen_player();
    else if (s_prev_screen_before_usb == 1)
        lv_obj_clean(s_playlist_cont);
    else if (s_prev_screen_before_usb == 2)
        lv_obj_clean(s_library_cont);
}

void UI_LeaveUSBScreen(void)
{
    switch (s_prev_screen_before_usb)
    {
    case 0:
        ensure_screen_player();
        lv_scr_load(g_scr_player);
        s_current_screen = 0;
        break;
    case 1:
        build_playlist_content();
        lv_scr_load(g_scr_playlist);
        s_current_screen = 1;
        break;
    case 2:
        build_library_content();
        lv_scr_load(g_scr_library);
        s_current_screen = 2;
        break;
    default:
        ensure_screen_player();
        lv_scr_load(g_scr_player);
        s_current_screen = 0;
        break;
    }
}

/* ============================ Volume popup (system layer) ============================ */
/*
 * 创建音量调节弹窗 (系统层, 覆盖在所有屏幕之上)
 * 布局: 左侧音量图标 + 右侧滑块
 * 1.5 秒无操作自动隐藏
 */
static void create_volume_popup(void)
{
    s_vol_cont = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_vol_cont, 195, 44);
    lv_obj_set_pos(s_vol_cont, 22, 50);
    lv_obj_set_style_bg_color(s_vol_cont, CLR_SURFACE, 0);
    lv_obj_set_style_radius(s_vol_cont, 10, 0);
    lv_obj_set_style_border_width(s_vol_cont, 1, 0);
    lv_obj_set_style_border_color(s_vol_cont, CLR_ACCENT_DIM, 0);
    lv_obj_add_flag(s_vol_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_vol_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_vol_cont, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *vol_label = lv_label_create(s_vol_cont);
    lv_label_set_text(vol_label, LV_SYMBOL_VOLUME_MID);
    lv_obj_align(vol_label, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_text_color(vol_label, CLR_ACCENT, 0);

    s_vol_slider = lv_slider_create(s_vol_cont);
    lv_obj_set_size(s_vol_slider, 130, 10);
    lv_obj_align(s_vol_slider, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_slider_set_range(s_vol_slider, 0, VOL_MAX);
    lv_slider_set_value(s_vol_slider, VOL_MAX - g_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_vol_slider, CLR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, CLR_SURFACE, LV_PART_KNOB);
    lv_obj_add_event_cb(s_vol_slider, vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 点击弹窗外区域关闭弹窗的覆盖层 */
    s_vol_dismiss_overlay = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(s_vol_dismiss_overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_vol_dismiss_overlay, 0, 0);
    lv_obj_set_style_bg_opa(s_vol_dismiss_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_vol_dismiss_overlay, 0, 0);
    lv_obj_clear_flag(s_vol_dismiss_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_vol_dismiss_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_vol_dismiss_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_vol_dismiss_overlay, vol_dismiss_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_move_background(s_vol_dismiss_overlay);
}

/* ============================ Init ============================ */
/*
 * 函数: UI_Init
 * 功能: 初始化 LVGL 用户界面
 *
 * 初始化顺序:
 *   1. 初始化样式
 *   2. 创建四个屏幕: player / playlist / library / usb_transfer
 *   3. 创建音量弹窗 (系统层)
 *   4. 加载 player 屏幕为初始屏幕
 *   5. 创建 250ms 定时器用于 UI 更新
 */
void UI_Init(void)
{
    init_styles();

    create_screen_player();
    create_screen_playlist();
    create_screen_library();
    create_screen_usb_transfer();

    create_volume_popup();

    lv_scr_load(g_scr_player);
    s_current_screen = 0;

    s_player_timer = lv_timer_create(player_timer_cb, 250, NULL);
}
