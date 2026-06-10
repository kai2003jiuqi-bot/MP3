/**
 * @file    ui.c
 * @brief   LVGL 用户界面实现
 *
 * 三个主屏幕 + 一个 USB 传输覆盖屏:
 *   1. 播放器 (player)  — 封面, 进度条, 频谱, 控制按钮
 *   2. 播放列表 (playlist) — 当前播放列表, 支持删除 / 移动排序
 *   3. 曲库 (library)   — SD 卡所有歌曲, 添加到播放列表
 *   4. USB 传输覆盖屏
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

#define N_BARS          12U

/* ============================ Global handles ============================ */
lv_obj_t *g_scr_player;
lv_obj_t *g_scr_playlist;
lv_obj_t *g_scr_library;
lv_obj_t *g_scr_usb_transfer;
lv_obj_t *g_ui_label_lyric;
lv_obj_t *g_ui_label_song_title;

/* ============================ Internal state ============================ */
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

static const lv_img_dsc_t s_cover_dsc = {
    .header.always_zero = 0,
    .header.w  = DECODE_W,
    .header.h  = DECODE_H,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data_size = COVER_BUF_SIZE,
    .data      = (const uint8_t *)COVER_BUF_ADDR,
};

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
static void player_timer_cb(lv_timer_t *timer)
{
    (void)timer;

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

    if (g_scr_player == NULL) return;

    /* --- Mode button sync (playlist restore may have changed g_play_mode) --- */
    lv_label_set_text(s_mode_btn_label, play_mode_label(g_play_mode));
    lv_obj_set_style_text_color(s_mode_btn_label,
        g_play_mode == PLAY_MODE_ALL ? CLR_ACCENT : CLR_DANGER, 0);

    /* --- Cover art update --- */
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

    if (s_is_playing
     && g_file_current_music != NULL
     && g_file_current_music->if_play)
    {
        lv_label_set_text(g_ui_label_song_title,
                          strip_mp3(g_file_current_music->utf8_name));

        if (g_seek_active)
        {
            /* let the bar hold its current value */
        }
        else if (g_reset_progress)
        {
            g_reset_progress = 0;
            s_progress_updating = 1;
            lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
            s_progress_updating = 0;
            lv_label_set_text(s_label_remaining, "-0:00");
            update_remaining_label_pos();
        }
        else if (g_song_bitrate > 0 && g_song_file_size > 0)
        {
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
            s_progress_updating = 1;
            lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);
            s_progress_updating = 0;
            lv_label_set_text(s_label_remaining, "-0:00");
            update_remaining_label_pos();
        }

        for (int i = 0; i < N_BARS; i++)
        {
            s_viz_seed = s_viz_seed * 1103515245 + 12345;
            lv_coord_t h = 1 + ((s_viz_seed >> 16) % 14);
            lv_obj_set_size(s_viz_bars[i], 10, h);
            lv_obj_set_y(s_viz_bars[i], 198 - h);
        }
    }
    else if (g_file_current_music != NULL && g_file_current_music->if_play)
    {
        lv_label_set_text(g_ui_label_song_title,
                          strip_mp3(g_file_current_music->utf8_name));
        for (int i = 0; i < N_BARS; i++)
        {
            lv_obj_set_size(s_viz_bars[i], 10, 1);
            lv_obj_set_y(s_viz_bars[i], 198 - 1);
        }
    }
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
    // File_SaveConfig();

    if (s_vol_hide_timer) lv_timer_del(s_vol_hide_timer);
    s_vol_hide_timer = lv_timer_create(vol_hide_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(s_vol_hide_timer, 1);
}

/* ============================ Progress bar seek ============================ */
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
    // File_SaveConfig();
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

static void playlist_up_cb(lv_event_t *e)
{
    (void)e;
    if (s_playlist_move_mode)
    {
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
    int count = 0;
    File_MusicNode_t *p = g_file_music_head;
    if (!p) return;

    do {
        if (p->if_play) count++;
        p = p->next;
    } while (p != g_file_music_head);

    if (s_playlist_move_mode)
    {
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
        if (s_playlist_selected_idx >= count - 1) return;
        s_playlist_selected_idx++;
    }

    build_playlist_content();
}

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

    File_MusicNode_t *p = g_file_music_head;
    if (!p) return;

    int idx = 0;
    do {
        if (p->if_play)
        {
            if (idx == s_playlist_selected_idx)
            {
                p->if_play = 0;

                if (p == g_file_current_music)
                {
                    File_MusicNode_t *n = p->next;
                    while (n != p && n->if_play == 0) n = n->next;
                    if (n->if_play) g_file_current_music = n;

                    STATE_PLAYMUSIC cmd = PLAYMUSIC_PAUSE;
                    xQueueOverwrite(g_queue_play_music, &cmd);
                    s_is_playing = 0;
                }

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
static void create_screen_player(void)
{
    g_scr_player = lv_obj_create(NULL);
    lv_obj_add_style(g_scr_player, &s_style_bg, 0);

    create_screen_dots(g_scr_player, s_dots_player, 0);
    create_nav_arrows(g_scr_player);

    lv_obj_t *art = lv_obj_create(g_scr_player);
    lv_obj_set_size(art, COVER_W, COVER_H);
    lv_obj_set_pos(art, (240 - COVER_W) / 2, 42);
    lv_obj_set_style_bg_opa(art, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(art, 0, 0);
    lv_obj_set_style_shadow_width(art, 0, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(art, LV_SCROLLBAR_MODE_OFF);

    s_cover_art_icon = lv_label_create(art);
    lv_label_set_text(s_cover_art_icon, LV_SYMBOL_AUDIO);
    lv_obj_center(s_cover_art_icon);
    lv_obj_set_style_text_color(s_cover_art_icon, CLR_ACCENT, 0);

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

    g_ui_label_song_title = lv_label_create(g_scr_player);
    lv_label_set_text(g_ui_label_song_title, "");
    lv_obj_set_pos(g_ui_label_song_title, 10, 168);
    lv_obj_set_size(g_ui_label_song_title, 220, 24);
    lv_obj_set_style_text_align(g_ui_label_song_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_ui_label_song_title, CLR_TEXT, 0);
    lv_obj_set_style_text_font(g_ui_label_song_title, &my_font_song_14, 0);
    lv_label_set_long_mode(g_ui_label_song_title, LV_LABEL_LONG_SCROLL_CIRCULAR);

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

    s_label_remaining = lv_label_create(g_scr_player);
    lv_label_set_text(s_label_remaining, "-0:00");
    lv_obj_set_pos(s_label_remaining, 22, 208);
    lv_obj_set_style_text_color(s_label_remaining, CLR_SUBTEXT, 0);

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
