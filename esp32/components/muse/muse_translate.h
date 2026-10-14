/*
 * Bidirectional ZH⇄EN translate page for Muse Gadget (360 round UI).
 * Hold BOOT to talk (same PTT as Muse); auto language; Azure STT + VPS
 * translate + TTS. Idle screen has no coaching tip text.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the translate tile. LVGL task only. */
void muse_translate_build(lv_obj_t *tile, int w, int h);
/* Slide to the translate tile. LVGL task only. */
void muse_translate_show(void);
/* Periodic UI refresh. LVGL task only. */
void muse_translate_tick(float now);

/* True when the active tileview page is translate. */
bool muse_translate_page_active(void);

/* Voice task: PTT began while on translate page. */
void muse_translate_on_listen(void);
/*
 * Voice task: PTT released with PCM. Takes a copy into SPIRAM and returns
 * immediately; returns true if translate mode handled the turn (caller must
 * not send to Muse). pcm may be freed by caller after return.
 */
bool muse_translate_submit_pcm(const int16_t *pcm, size_t frames);

#ifdef __cplusplus
}
#endif
