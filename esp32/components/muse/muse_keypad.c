/*
 * Phone-style keypad for small touch screens, with optional toneless pinyin IME.
 * Modes: 拼 (pinyin) → abc → 123 → #+= . Candidate bar sits above the keys.
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
#define PY_MAX 24
#define CAND_MAX 8
#define CAND_BAR_H 30

enum { PINYIN, LETTERS, DIGITS, SYMBOLS, MODE_COUNT };

enum { KEY_SHIFT = 9, KEY_SPACE = 10, KEY_BACKSPACE = 11, KEY_MODE, KEY_SHOW, KEY_DONE, KEY_COUNT };

static const char *const CHARS[MODE_COUNT][12] = {
    /* PINYIN: same multitap letters as LETTERS; digits on hold */
    { ".-_@1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", NULL, " 0", NULL },
    { ".-_@1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9", NULL, " 0", NULL },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", NULL },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", " ", "0", NULL },
};

static const char *const LABELS[MODE_COUNT][12] = {
    { ".-_@", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", LV_SYMBOL_UP, "space", LV_SYMBOL_BACKSPACE },
    { ".-_@", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", LV_SYMBOL_UP, "space", LV_SYMBOL_BACKSPACE },
    { "1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", LV_SYMBOL_BACKSPACE },
    { ".,?!", "@#$%", "&*+=", "-_/\\", "()[]", "{}<>", "'\"`", ":;|", "^~", "space", "0", LV_SYMBOL_BACKSPACE },
};

static const char *const UPPER_LABELS[9] = { ".-_@", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ" };

/* Mode key names the mode it switches TO. */
static const char *const NEXT_MODE[MODE_COUNT] = { "abc", "123", "#+=", "拼" };

static lv_obj_t *s_kp, *s_ta;
static lv_obj_t *s_cand_bar, *s_py_lbl;
static lv_obj_t *s_cand_btns[CAND_MAX];
static lv_timer_t *s_tap_timer;
static const char *s_map[KEY_COUNT + 2 + 4 + 1];
static bool s_round;
static int s_mode;
static bool s_upper;
static int s_pending = -1;
static int s_step;
static uint32_t s_pressed_ms;
static bool s_held;

static char s_py[PY_MAX];
static const char *s_cands[CAND_MAX];
static int s_cand_n;
static char s_cand_owned[CAND_MAX][32]; /* copy of candidate text for button labels */

#if CONFIG_MUSE_CJK_FONT
static const lv_font_t *cjk_font(void)
{
    static lv_font_t font;
    static bool ready;
    if (!ready) {
        font = lv_font_montserrat_16;
        font.fallback = &muse_font_cjk_16;
        ready = true;
    }
    return &font;
}
#else
static const lv_font_t *cjk_font(void) { return &lv_font_montserrat_16; }
#endif

static int key_button(int key)
{
    return s_round && key >= KEY_MODE ? key + 1 : key;
}

static int button_key(uint32_t btn)
{
    int key = s_round && btn >= KEY_MODE ? (int)btn - 1 : (int)btn;
    return key >= 0 && key < KEY_COUNT && !(s_round && btn == KEY_MODE) ? key : -1;
}

static void set_ctrl(int key, lv_buttonmatrix_ctrl_t ctrl, bool on)
{
    if (on) {
        lv_buttonmatrix_set_button_ctrl(s_kp, key_button(key), ctrl);
    } else {
        lv_buttonmatrix_clear_button_ctrl(s_kp, key_button(key), ctrl);
    }
}

static void update_map(void)
{
    int i = 0;
    for (int k = 0; k < 12; k++) {
        s_map[i++] = s_mode == LETTERS && s_upper && k < 9 ? UPPER_LABELS[k]
                     : LABELS[s_mode][k];
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
    set_ctrl(KEY_SHIFT, LV_BUTTONMATRIX_CTRL_CHECKED, s_mode == LETTERS && s_upper);
}

static void commit(void)
{
    if (s_pending >= 0 && s_kp) {
        set_ctrl(s_pending, LV_BUTTONMATRIX_CTRL_CHECKED, false);
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

static void add_char(char c)
{
    lv_textarea_add_char(s_ta, (uint32_t)(unsigned char)c);
}

static void add_utf8(const char *s)
{
    if (!s || !s[0]) {
        return;
    }
    lv_textarea_add_text(s_ta, s);
}

static void refresh_cands(void);

static void clear_py(void)
{
    s_py[0] = 0;
    refresh_cands();
}

static void refresh_cands(void)
{
    s_cand_n = 0;
    if (s_mode == PINYIN && s_py[0]) {
        s_cand_n = muse_pinyin_lookup(s_py, s_cands, CAND_MAX);
        for (int i = 0; i < s_cand_n; i++) {
            strncpy(s_cand_owned[i], s_cands[i], sizeof(s_cand_owned[i]) - 1);
            s_cand_owned[i][sizeof(s_cand_owned[i]) - 1] = 0;
        }
    }
    if (s_py_lbl) {
        if (s_mode == PINYIN && s_py[0]) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%s", s_py);
            lv_label_set_text(s_py_lbl, buf);
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
    if (s_mode == PINYIN && (s_py[0] || s_cand_n > 0)) {
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
    clear_py();
}

static void on_cand_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    pick_cand(idx);
}

static void py_add_letter(char c)
{
    size_t n = strlen(s_py);
    if (n + 1 >= sizeof(s_py)) {
        return;
    }
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }
    if (c < 'a' || c > 'z') {
        return;
    }
    s_py[n] = c;
    s_py[n + 1] = 0;
    refresh_cands();
}

static void type_key_ascii(int key)
{
    const char *chars = CHARS[s_mode][key];
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
    set_ctrl(key, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    lv_timer_reset(s_tap_timer);
    lv_timer_resume(s_tap_timer);
}

/* Pinyin multitap: pending letter lives in py buffer, not textarea. */
static void type_key_pinyin(int key)
{
    const char *chars = CHARS[PINYIN][key];
    if (!chars) {
        return;
    }
    int n = (int)strlen(chars);
    /* Prefer letters over trailing digit for pinyin. */
    char letters[8];
    int ln = 0;
    for (int i = 0; i < n && ln < (int)sizeof(letters) - 1; i++) {
        if (chars[i] >= 'a' && chars[i] <= 'z') {
            letters[ln++] = chars[i];
        }
    }
    letters[ln] = 0;
    if (ln == 0) {
        /* space / symbols — commit first cand or insert char */
        if (key == KEY_SPACE) {
            if (s_cand_n > 0) {
                commit();
                pick_cand(0);
            } else if (!s_py[0]) {
                commit();
                add_char(' ');
            }
            return;
        }
        commit();
        add_char(chars[0]);
        return;
    }
    if (key == s_pending) {
        s_step = (s_step + 1) % ln;
        /* replace last py letter */
        size_t plen = strlen(s_py);
        if (plen > 0) {
            s_py[plen - 1] = letters[s_step];
        }
    } else {
        commit();
        s_step = 0;
        py_add_letter(letters[0]);
        s_pending = key;
        set_ctrl(key, LV_BUTTONMATRIX_CTRL_CHECKED, true);
        lv_timer_reset(s_tap_timer);
        lv_timer_resume(s_tap_timer);
        return;
    }
    refresh_cands();
    set_ctrl(key, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    lv_timer_reset(s_tap_timer);
    lv_timer_resume(s_tap_timer);
}

static void type_key(int key)
{
    if (s_mode == PINYIN) {
        type_key_pinyin(key);
    } else {
        type_key_ascii(key);
    }
}

static char held_digit(int key)
{
    if (key < 0 || key >= 12 || (s_mode != LETTERS && s_mode != PINYIN) || !CHARS[s_mode][key]) {
        return 0;
    }
    const char *chars = CHARS[s_mode][key];
    char last = chars[strlen(chars) - 1];
    return isdigit((unsigned char)last) ? last : 0;
}

static void on_key(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int key = button_key(lv_buttonmatrix_get_selected_button(s_kp));

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
            if (s_mode == PINYIN && s_py[0]) {
                clear_py();
            }
            add_char(digit);
        }
        return;
    }

    key = button_key(*(const uint32_t *)lv_event_get_param(e));
    if (key < 0 || s_held) {
        return;
    }
    switch (key) {
    case KEY_BACKSPACE:
        commit();
        if (s_mode == PINYIN && s_py[0]) {
            s_py[strlen(s_py) - 1] = 0;
            refresh_cands();
        } else {
            lv_textarea_delete_char(s_ta);
        }
        break;
    case KEY_SHIFT:
        if (s_mode == LETTERS) {
            commit();
            s_upper = !s_upper;
            update_map();
        } else if (s_mode == PINYIN) {
            /* Shift in pinyin: commit first candidate if any */
            if (s_cand_n > 0) {
                pick_cand(0);
            }
        } else {
            type_key(key);
        }
        break;
    case KEY_MODE:
        commit();
        if (s_mode == PINYIN && s_py[0]) {
            clear_py();
        }
        s_mode = (s_mode + 1) % MODE_COUNT;
        s_upper = false;
        update_map();
        refresh_cands();
        break;
    case KEY_SHOW:
        lv_textarea_set_password_mode(s_ta, !lv_textarea_get_password_mode(s_ta));
        update_map();
        break;
    case KEY_DONE:
        commit();
        if (s_mode == PINYIN && s_cand_n > 0) {
            pick_cand(0);
        }
        if (s_mode == PINYIN && s_py[0]) {
            /* leftover letters: dump as latin */
            add_utf8(s_py);
            clear_py();
        }
        lv_obj_send_event(s_kp, LV_EVENT_READY, NULL);
        break;
    case KEY_SPACE:
        if (s_mode == PINYIN) {
            type_key_pinyin(KEY_SPACE);
        } else {
            type_key(key);
        }
        break;
    default:
        if (key < 12) {
            type_key(key);
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
    s_py[0] = 0;
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
        lv_obj_set_style_pad_hor(b, 8, 0);
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
    build_cand_bar(parent);

    s_kp = lv_buttonmatrix_create(parent);
    s_ta = ta;
    s_round = round;
    s_mode = PINYIN; /* default Chinese for Muse chat */
    s_upper = false;
    s_pending = -1;
    s_py[0] = 0;
    if (!s_tap_timer) {
        s_tap_timer = lv_timer_create(on_tap_timer, TAP_MS, NULL);
    }
    lv_timer_pause(s_tap_timer);
    lv_textarea_set_password_show_time(ta, TAP_MS + 500);

    lv_obj_remove_style_all(s_kp);
    lv_obj_set_style_pad_gap(s_kp, 6, 0);
    lv_obj_set_style_text_font(s_kp, cjk_font(), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kp, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kp, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_CARD_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kp, lv_color_hex(COLOR_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_kp, 12, LV_PART_ITEMS);
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
    s_mode = password ? LETTERS : PINYIN;
    s_upper = false;
    s_py[0] = 0;
    update_map();
    refresh_cands();

    const lv_buttonmatrix_ctrl_t tap = LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT;
    for (int k = 0; k < KEY_COUNT; k++) {
        if (k != KEY_BACKSPACE) {
            set_ctrl(k, tap, true);
        }
    }
    set_ctrl(KEY_DONE, LV_BUTTONMATRIX_CTRL_CHECKED, true);
    set_ctrl(KEY_SHOW, LV_BUTTONMATRIX_CTRL_HIDDEN, !password);
    int done = s_round ? 0 : 1;
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_MODE), password ? 3 : 4);
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_SHOW), password ? 3 : 1);
    lv_buttonmatrix_set_button_width(s_kp, key_button(KEY_DONE), (password ? 3 : 4) + done);
    if (s_round) {
        const uint32_t spacers[] = { KEY_MODE, KEY_COUNT + 1 };
        for (int i = 0; i < 2; i++) {
            lv_buttonmatrix_set_button_ctrl(s_kp, spacers[i], LV_BUTTONMATRIX_CTRL_HIDDEN);
            lv_buttonmatrix_set_button_width(s_kp, spacers[i], 2);
        }
    }
}
