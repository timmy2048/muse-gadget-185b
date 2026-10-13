#pragma once

#include <stdbool.h>

#include "lvgl.h"

/*
 * A phone-style keypad for small touch screens: twelve big keys with a few
 * characters each. Default mode is toneless pinyin (拼) with a candidate bar;
 * the mode key cycles 拼 → abc → 123 → #+=. Only one keypad exists at a time.
 */

lv_obj_t *muse_keypad_create(lv_obj_t *parent, lv_obj_t *ta, bool round);

/* Place candidate bar + keys: top_y from parent top, total height for both. */
void muse_keypad_layout(lv_obj_t *kp, int top_y, int width, int height);

void muse_keypad_reset(lv_obj_t *kp, bool password);
