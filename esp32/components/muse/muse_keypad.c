/*
 * Touch keypad: 26-key pinyin, T9 digit pinyin, abc, 123, symbols.
 * Pinyin accumulates (letters or T9 digits) and refreshes candidates each key.
 */
#include "muse_keypad.h"
#include "muse_pinyin.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#if CONFIG_MUSE_CJK_FONT
#include "lvgl.h"
LV_FONT_DECLARE(muse_font_cjk_16)
#endif

#define COLOR_TEXT 0xf2efff
#define COLOR_CARD 0x1a1530
#define COLOR_CARD_PRESSED 0x2e2552
#define COLOR_ACCENT 0xa77dff
#define COLOR_DIM 0x8a82a8

#define TAP_MS 1000
#define HOLD_MS 600
#define BUF_MAX 32
#define CAND_MAX 8
#define CAND_BAR_H 30

enum {
    PINYIN_26 = 0,
    PINYIN_T9,
    LETTERS,
    DIGITS,
    SYMBOLS,
    MODE_COUNT
};

/* T9 grid key indices (same 12-key phone pad). */
enum {
    T9_1 = 0,
    T9_2,
    T9_3,
    T9_4,
    T9_5,
    T9_6,
    T9_7,
    T9_8,
    T9_9,
    T9_SHIFT,
    T9_SPACE,
    T9_BS,
    T9_MODE,
    T9_SHOW,
    T9_DONE,
    T9_KEY_COUNT
};

static const char *const T9_CHARS[MODE_COUNT][12] = {
    /* unused for PINYIN_26 */
    {0},
    /* PINYIN_T9: digit on tap for 2-9; letters shown as labels */
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", NULL, "0", NULL },
    { ".-_@1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", NULL, " 0", NULL },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", NULL },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", " ", "0", NULL },
};

static const char *const T9_LABELS[MODE_COUNT][12] = {
    {0},
    { "1", "abc\n2", "def\n3", "ghi\n4", "jkl\n5", "mno\n6", "pqrs\n7", "tuv\n8", "wxyz\n9",
      "选", "空格", LV_SYMBOL_BACKSPACE },
    { ".-_@", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", LV_SYMBOL_UP, "space",
      LV_SYMBOL_BACKSPACE },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", LV_SYMBOL_BACKSPACE },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", "space", "0",
      LV_SYMBOL_BACKSPACE },
};

static const char *const UPPER_LABELS[9] = { ".-_@", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV",
                                             "WXYZ" };

/* Mode key names the mode it switches TO. */
static const char *const NEXT_MODE[MODE_COUNT] = { "九键", "abc", "123", "#+=", "全键" };

static lv_obj_t *s_kp, *s_ta;
static lv_obj_t *s_cand_bar, *s_py_lbl;
static lv_obj_t *s_cand_btns[CAND_MAX];
static lv_timer_t *s_tap_timer;
static const char *s_map[48];
static bool s_round;
static int s_mode;
static bool s_upper;
static int s_pending = -1;
static int s_step;
static uint32_t s_pressed_ms;
static bool s_held;

/* Compose buffer: letters (a-z) for 26-key, digits (2-9) for T9. */
static char s_buf[BUF_MAX];
static muse_py_hit_t s_hits[CAND_MAX];
static int s_cand_n;
static char s_cand_owned[CAND_MAX][32];
static uint8_t s_cand_consume[CAND_MAX];

#if CONFIG_MUSE_CJK_FONT
static const lv_font_t *cjk_font(void)
{
    static lv_font_t font;
    static bool ready;
    if (!ready) {
        font = lv_font_montserrat_14;
        font.fallback = &muse_font_cjk_16;
        ready = true;
    }
    return &font;
}
#else
static const lv_font_t *cjk_font(void) { return &lv_font_montserrat_14; }
#endif

static int is_pinyin_mode(void) { return s_mode == PINYIN_26 || s_mode == PINYIN_T9; }

static int t9_key_button(int key)
{
    return s_round && key >= T9_MODE ? key + 1 : key;
}

static int t9_button_key(uint32_t btn)
{
    int key = s_round && btn >= T9_MODE ? (int)btn - 1 : (int)btn;
    return key >= 0 && key < T9_KEY_COUNT && !(s_round && btn == T9_MODE) ? key : -1;
}

static void set_ctrl_idx(uint32_t btn, lv_buttonmatrix_ctrl_t ctrl, bool on)
{
    if (on) {
        lv_buttonmatrix_set_button_ctrl(s_kp, btn, ctrl);
    } else {
        lv_buttonmatrix_clear_button_ctrl(s_kp, btn, ctrl);
    }
}

static void set_ctrl_t9(int key, lv_buttonmatrix_ctrl_t ctrl, bool on)
{
    set_ctrl_idx(t9_key_button(key), ctrl, on);
}

static void commit(void)
{
    if (s_pending >= 0 && s_kp && s_mode != PINYIN_26) {
        set_ctrl_t9(s_pending, LV_BUTTONMATRIX_CTRL_CHECKED, false);
    }
    s_pending = -1;
    if (s_tap_timer) {
        lv_timer_pause(s_tap_timer);
    }
}

static void on_tap_timer(lv_timer_t *t)
{
    (void)t;
    commit();
}

static void add_char(char c) { lv_textarea_add_char(s_ta, (uint32_t)(unsigned char)c); }

static void add_utf8(const char *s)
{
    if (!s || !s[0]) {
        return;
    }
    lv_textarea_add_text(s_ta, s);
}

static void refresh_cands(void);

static void clear_buf(void)
{
    s_buf[0] = 0;
    refresh_cands();
}

static void refresh_cands(void)
{
    s_cand_n = 0;
    if (is_pinyin_mode() && s_buf[0]) {
        if (s_mode == PINYIN_T9) {
            s_cand_n = muse_pinyin_lookup_t9(s_buf, s_hits, CAND_MAX);
        } else {
            s_cand_n = muse_pinyin_lookup(s_buf, s_hits, CAND_MAX);
        }
        for (int i = 0; i < s_cand_n; i++) {
            strncpy(s_cand_owned[i], s_hits[i].text, sizeof(s_cand_owned[i]) - 1);
            s_cand_owned[i][sizeof(s_cand_owned[i]) - 1] = 0;
            s_cand_consume[i] = s_hits[i].consume;
        }
    }
    if (s_py_lbl) {
        if (is_pinyin_mode() && s_buf[0]) {
            lv_label_set_text(s_py_lbl, s_buf);
            lv_obj_remove_flag(s_py_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_py_lbl, "");
            lv_obj_add_flag(s_py_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!s_cand_bar) {
        return;
    }
    for (int i = 0; i < CAND_MAX; i++) {
        if (!s_cand_btns[i]) {
            continue;
        }
        if (i < s_cand_n) {
            lv_obj_t *lab = lv_obj_get_child(s_cand_btns[i], 0);
            if (lab) {
                lv_label_set_text(lab, s_cand_owned[i]);
            }
            lv_obj_remove_flag(s_cand_btns[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cand_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (is_pinyin_mode() && (s_buf[0] || s_cand_n > 0)) {
        lv_obj_remove_flag(s_cand_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cand_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pick_cand(int idx)
{
    if (idx < 0 || idx >= s_cand_n) {
        return;
    }
    add_utf8(s_cand_owned[idx]);
    int c = s_cand_consume[idx];
    size_t n = strlen(s_buf);
    if (c > (int)n) {
        c = (int)n;
    }
    if (c > 0) {
        memmove(s_buf, s_buf + c, n - (size_t)c + 1);
    } else {
        s_buf[0] = 0;
    }
    commit();
    refresh_cands();
}

static void on_cand_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pick_cand(idx);
}

static void buf_add(char c)
{
    size_t n = strlen(s_buf);
    if (n + 1 >= sizeof(s_buf)) {
        return;
    }
    s_buf[n] = c;
    s_buf[n + 1] = 0;
    refresh_cands();
}

static void buf_backspace(void)
{
    size_t n = strlen(s_buf);
    if (n == 0) {
        return;
    }
    s_buf[n - 1] = 0;
    refresh_cands();
}

static void update_map_t9_family(void)
{
    int i = 0;
    int m = s_mode;
    for (int k = 0; k < 12; k++) {
        if (m == LETTERS && s_upper && k < 9) {
            s_map[i++] = UPPER_LABELS[k];
        } else {
            s_map[i++] = T9_LABELS[m][k];
        }
        if (k % 3 == 2) {
            s_map[i++] = "\n";
        }
    }
    if (s_round) {
        s_map[i++] = " ";
    }
    s_map[i++] = NEXT_MODE[s_mode];
    s_map[i++] = lv_textarea_get_password_mode(s_ta) ? "Show" : "Hide";
    s_map[i++] = LV_SYMBOL_OK;
    if (s_round) {
        s_map[i++] = " ";
    }
    s_map[i] = "";
    lv_buttonmatrix_set_map(s_kp, s_map);
    set_ctrl_t9(T9_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED, s_mode == LETTERS && s_upper);
}

static void update_map_26(void)
{
    /* Compact QWERTY for ~360px round LCD. */
    static const char *rows[] = {
        "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
        "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
        "选", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    };
    int i = 0;
    for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
        s_map[i++] = rows[r];
    }
    if (s_round) {
        s_map[i++] = " ";
    }
    s_map[i++] = NEXT_MODE[s_mode];
    s_map[i++] = "空格";
    s_map[i++] = LV_SYMBOL_OK;
    if (s_round) {
        s_map[i++] = " ";
    }
    s_map[i] = "";
    lv_buttonmatrix_set_map(s_kp, s_map);
}

static void update_map(void)
{
    if (s_mode == PINYIN_26) {
        update_map_26();
    } else {
        update_map_t9_family();
    }
}

static void apply_width_controls(bool password)
{
    const lv_buttonmatrix_ctrl_t tap = LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
    uint32_t total = 0;
    /* Count buttons roughly by walking map until "" — LVGL stores internally; set per known layout. */
    if (s_mode == PINYIN_26) {
        /* 10 + 9 + 9 + (spacer?) + mode + space + ok + (spacer?) */
        uint32_t n = s_round ? 34 : 32; /* 10+1+9+1+9+1 + optional spacers + 3 */
        /* Safer: set ctrl on all existing buttons via get_map */
        for (uint32_t b = 0; b < 40; b++) {
            const char *t = lv_buttonmatrix_get_button_text(s_kp, b);
            if (!t) {
                break;
            }
            total = b + 1;
            lv_buttonmatrix_clear_button_ctrl(s_kp, b, 0xFFFF);
            set_ctrl_idx(b, tap, true);
            if (strcmp(t, " ") == 0) {
                set_ctrl_idx(b, LV_BUTTONMATRIX_CTRL_HIDDEN, true);
                lv_buttonmatrix_set_button_width(s_kp, b, 1);
            }
        }
        (void)n;
        (void)total;
        /* Hide Show in non-password; find OK and enlarge */
        for (uint32_t b = 0; b < 40; b++) {
            const char *t = lv_buttonmatrix_get_button_text(s_kp, b);
            if (!t) {
                break;
            }
            if (strcmp(t, "Show") == 0 || strcmp(t, "Hide") == 0) {
                set_ctrl_idx(b, LV_BUTTONMATRIX_CTRL_HIDDEN, !password);
            }
            if (strcmp(t, LV_SYMBOL_OK) == 0) {
                set_ctrl_idx(b, LV_BUTTONMATRIX_CTRL_CHECKED, true);
                lv_buttonmatrix_set_button_width(s_kp, b, 2);
            }
            if (strcmp(t, "空格") == 0) {
                lv_buttonmatrix_set_button_width(s_kp, b, 3);
            }
            if (strcmp(t, NEXT_MODE[s_mode]) == 0 || strcmp(t, "九键") == 0) {
                lv_buttonmatrix_set_button_width(s_kp, b, 2);
            }
        }
    } else {
        for (int k = 0; k < T9_KEY_COUNT; k++) {
            if (k != T9_BS) {
                set_ctrl_t9(k, tap, true);
            }
        }
        set_ctrl_t9(T9_DONE, LV_BUTTONMATRIX_CTRL_CHECKED, true);
        set_ctrl_t9(T9_SHOW, LV_BUTTONMATRIX_CTRL_HIDDEN, !password);
        int done = s_round ? 0 : 1;
        lv_buttonmatrix_set_button_width(s_kp, t9_key_button(T9_MODE), password ? 3 : 4);
        lv_buttonmatrix_set_button_width(s_kp, t9_key_button(T9_SHOW), password ? 3 : 1);
        lv_buttonmatrix_set_button_width(s_kp, t9_key_button(T9_DONE), (password ? 3 : 4) + done);
        if (s_round) {
            const uint32_t spacers[] = { T9_MODE, T9_KEY_COUNT + 1 };
            for (int i = 0; i < 2; i++) {
                lv_buttonmatrix_set_button_ctrl(s_kp, spacers[i], LV_BUTTONMATRIX_CTRL_HIDDEN);
                lv_buttonmatrix_set_button_width(s_kp, spacers[i], 2);
            }
        }
    }
}

static void type_key_ascii(int key)
{
    const char *chars = T9_CHARS[s_mode][key];
    if (!chars) {
        return;
    }
    int n = (int)strlen(chars);
    if (n == 1) {
        commit();
        add_char(chars[0]);
        return;
    }
    if (key == s_pending) {
        s_step = (s_step + 1) % n;
        lv_textarea_delete_char(s_ta);
    } else {
        commit();
        s_step = 0;
    }
    char c = chars[s_step];
    size_t before = strlen(lv_textarea_get_text(s_ta));
    add_char(s_upper && s_mode == LETTERS ? (char)toupper((unsigned char)c) : c);
    if (strlen(lv_textarea_get_text(s_ta)) == before) {
        commit();
        return;
    }
    s_pending = key;
    set_ctrl_t9(key, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    lv_timer_reset(s_tap_timer);
    lv_timer_resume(s_tap_timer);
}

/* T9 pinyin: each 2-9 key appends that digit; candidates refresh. */
static void type_key_t9_pinyin(int key)
{
    if (key == T9_SPACE) {
        commit();
        if (s_cand_n > 0) {
            pick_cand(0);
        } else if (!s_buf[0]) {
            add_char(' ');
        }
        return;
    }
    if (key < 0 || key > 8) {
        return;
    }
    /* keys 0..8 -> digits 1..9; digit 1 unused for pinyin */
    char digit = (char)('1' + key);
    if (digit < '2' || digit > '9') {
        return;
    }
    commit();
    buf_add(digit);
}

static void handle_26_button(const char *txt)
{
    if (!txt || !txt[0]) {
        return;
    }
    if (strcmp(txt, " ") == 0) {
        return;
    }
    if (strcmp(txt, NEXT_MODE[s_mode]) == 0 || strcmp(txt, "九键") == 0) {
        commit();
        if (s_buf[0]) {
            clear_buf();
        }
        s_mode = (s_mode + 1) % MODE_COUNT;
        s_upper = false;
        update_map();
        apply_width_controls(lv_textarea_get_password_mode(s_ta));
        refresh_cands();
        return;
    }
    if (strcmp(txt, LV_SYMBOL_OK) == 0) {
        commit();
        if (s_cand_n > 0) {
            pick_cand(0);
        }
        if (s_buf[0]) {
            /* dump leftover latin/digits */
            add_utf8(s_buf);
            clear_buf();
        }
        lv_obj_send_event(s_kp, LV_EVENT_READY, NULL);
        return;
    }
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        commit();
        if (s_buf[0]) {
            buf_backspace();
        } else {
            lv_textarea_delete_char(s_ta);
        }
        return;
    }
    if (strcmp(txt, "空格") == 0) {
        commit();
        if (s_cand_n > 0) {
            pick_cand(0);
        } else if (!s_buf[0]) {
            add_char(' ');
        }
        return;
    }
    if (strcmp(txt, "选") == 0) {
        if (s_cand_n > 0) {
            pick_cand(0);
        }
        return;
    }
    /* Single letter key */
    if (txt[0] >= 'a' && txt[0] <= 'z' && txt[1] == '\0') {
        commit();
        buf_add(txt[0]);
        return;
    }
}

static char held_digit(int key)
{
    if (key < 0 || key >= 12 || (s_mode != LETTERS && s_mode != PINYIN_T9) || !T9_CHARS[s_mode][key]) {
        return 0;
    }
    if (s_mode == PINYIN_T9) {
        return 0; /* digits are the primary action */
    }
    const char *chars = T9_CHARS[s_mode][key];
    char last = chars[strlen(chars) - 1];
    return isdigit((unsigned char)last) ? last : 0;
}

static void on_key(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (s_mode == PINYIN_26) {
        if (code != LV_EVENT_VALUE_CHANGED) {
            return;
        }
        uint32_t btn = *(const uint32_t *)lv_event_get_param(e);
        const char *txt = lv_buttonmatrix_get_button_text(s_kp, btn);
        handle_26_button(txt);
        return;
    }

    int key = t9_button_key(lv_buttonmatrix_get_selected_button(s_kp));

    if (code == LV_EVENT_PRESSED) {
        s_pressed_ms = lv_tick_get();
        s_held = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        char digit = held_digit(key);
        if (digit && !s_held && lv_tick_elaps(s_pressed_ms) >= HOLD_MS) {
            s_held = true;
            commit();
            if (is_pinyin_mode() && s_buf[0]) {
                clear_buf();
            }
            add_char(digit);
        }
        return;
    }

    key = t9_button_key(*(const uint32_t *)lv_event_get_param(e));
    if (key < 0 || s_held) {
        return;
    }
    switch (key) {
    case T9_BS:
        commit();
        if (is_pinyin_mode() && s_buf[0]) {
            buf_backspace();
        } else {
            lv_textarea_delete_char(s_ta);
        }
        break;
    case T9_SHIFT:
        if (s_mode == LETTERS) {
            commit();
            s_upper = !s_upper;
            update_map();
        } else if (s_mode == PINYIN_T9) {
            if (s_cand_n > 0) {
                pick_cand(0);
            }
        } else {
            type_key_ascii(key);
        }
        break;
    case T9_MODE:
        commit();
        if (s_buf[0]) {
            clear_buf();
        }
        s_mode = (s_mode + 1) % MODE_COUNT;
        s_upper = false;
        update_map();
        apply_width_controls(lv_textarea_get_password_mode(s_ta));
        refresh_cands();
        break;
    case T9_SHOW:
        lv_textarea_set_password_mode(s_ta, !lv_textarea_get_password_mode(s_ta));
        update_map();
        apply_width_controls(lv_textarea_get_password_mode(s_ta));
        break;
    case T9_DONE:
        commit();
        if (is_pinyin_mode() && s_cand_n > 0) {
            pick_cand(0);
        }
        if (is_pinyin_mode() && s_buf[0]) {
            add_utf8(s_buf);
            clear_buf();
        }
        lv_obj_send_event(s_kp, LV_EVENT_READY, NULL);
        break;
    case T9_SPACE:
        if (s_mode == PINYIN_T9) {
            type_key_t9_pinyin(T9_SPACE);
        } else {
            type_key_ascii(key);
        }
        break;
    default:
        if (key < 12) {
            if (s_mode == PINYIN_T9) {
                type_key_t9_pinyin(key);
            } else {
                type_key_ascii(key);
            }
        }
        break;
    }
}

static void on_ta_pressed(lv_event_t *e)
{
    (void)e;
    commit();
}

static void on_delete(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_kp) {
        return;
    }
    if (s_tap_timer) {
        lv_timer_delete(s_tap_timer);
        s_tap_timer = NULL;
    }
    s_kp = NULL;
    s_ta = NULL;
    s_cand_bar = NULL;
    s_py_lbl = NULL;
    for (int i = 0; i < CAND_MAX; i++) {
        s_cand_btns[i] = NULL;
    }
    s_pending = -1;
    s_buf[0] = 0;
}

static void build_cand_bar(lv_obj_t *parent)
{
    s_cand_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cand_bar);
    lv_obj_set_size(s_cand_bar, lv_pct(100), CAND_BAR_H);
    lv_obj_set_style_pad_hor(s_cand_bar, s_round ? 28 : 8, 0);
    lv_obj_set_flex_flow(s_cand_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_cand_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_cand_bar, 4, 0);
    lv_obj_add_flag(s_cand_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cand_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cand_bar, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(s_cand_bar, LV_DIR_HOR);

    s_py_lbl = lv_label_create(s_cand_bar);
    lv_obj_set_style_text_font(s_py_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_py_lbl, lv_color_hex(COLOR_ACCENT), 0);
    lv_label_set_text(s_py_lbl, "");
    lv_obj_add_flag(s_py_lbl, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < CAND_MAX; i++) {
        lv_obj_t *b = lv_button_create(s_cand_bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, CAND_BAR_H - 4);
        lv_obj_set_style_pad_hor(b, 6, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_CARD_PRESSED), LV_STATE_PRESSED);
        lv_obj_t *lab = lv_label_create(b);
        lv_obj_set_style_text_font(lab, cjk_font(), 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(COLOR_TEXT), 0);
        lv_label_set_text(lab, "");
        lv_obj_center(lab);
        lv_obj_add_event_cb(b, on_cand_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_cand_btns[i] = b;
    }
}

lv_obj_t *muse_keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round)
{
    s_round = round;
    build_cand_bar(parent);

    s_kp = lv_buttonmatrix_create(parent);
    s_ta = ta;
    s_mode = PINYIN_26; /* default: 26-key pinyin for smooth typing */
    s_upper = false;
    s_pending = -1;
    s_buf[0] = 0;
    if (!s_tap_timer) {
        s_tap_timer = lv_timer_create(on_tap_timer, TAP_MS, NULL);
    }
    lv_timer_pause(s_tap_timer);
    lv_textarea_set_password_show_time(ta, TAP_MS + 500);

    lv_obj_remove_style_all(s_kp);
    lv_obj_set_style_pad_gap(s_kp, s_mode == PINYIN_26 ? 3 : 6, 0);
    lv_obj_set_style_text_font(s_kp, cjk_font(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kp, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kp, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_kp, s_mode == PINYIN_26 ? 6 : 12, LV_PART_ITEMS);
    lv_obj_remove_flag(s_kp, LV_OBJ_FLAG_GESTURE_BUBBLE);

    update_map();
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_kp, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_kp, on_delete, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(ta, on_ta_pressed, LV_EVENT_PRESSED, NULL);
    muse_keypad_reset(s_kp, false);
    return s_kp;
}

void muse_keypad_layout(lv_obj_t *kp, int top_y, int width, int height)
{
    (void)kp;
    if (!s_kp) {
        return;
    }
    int cand_h = CAND_BAR_H + 2;
    if (s_cand_bar) {
        lv_obj_set_size(s_cand_bar, width, CAND_BAR_H);
        lv_obj_align(s_cand_bar, LV_ALIGN_TOP_MID, 0, top_y);
    }
    lv_obj_set_size(s_kp, width, height - cand_h);
    lv_obj_align(s_kp, LV_ALIGN_TOP_MID, 0, top_y + cand_h);
}

void muse_keypad_reset(lv_obj_t *kp, bool password)
{
    (void)kp;
    commit();
    s_mode = password ? LETTERS : PINYIN_26;
    s_upper = false;
    s_buf[0] = 0;
    lv_obj_set_style_pad_gap(s_kp, s_mode == PINYIN_26 ? 3 : 6, 0);
    lv_obj_set_style_radius(s_kp, s_mode == PINYIN_26 ? 6 : 12, LV_PART_ITEMS);
    update_map();
    apply_width_controls(password);
    refresh_cands();
}
