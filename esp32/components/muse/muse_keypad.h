#pragma once

#include <stdbool.h>

#include "lvgl.h"

/*
 * Round/rect touch keypad with Chinese IME:
 *   全键 (26-key pinyin) → 九键 (T9 digit pinyin) → abc → 123 → #+=
 * Candidate bar above keys; only one keypad at a time.
 */

lv_obj_t *muse_keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round);

/* Place candidate bar + keys: top_y from parent top, total height for both. */
void muse_keypad_layout(lv_obj_t *kp, int top_y, int width, int height);

void muse_keypad_reset(lv_obj_t *kp, bool password);
