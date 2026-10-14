/*
 * Chat-driven todo list for Muse Gadget (360 round UI).
 * See muse_todo_parse.h for the command / JSON contract.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#include "muse_todo_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the todos tile. LVGL task only. */
void muse_todo_build(lv_obj_t *tile, int w, int h);
/* Slide to the todos tile. LVGL task only. */
void muse_todo_show(void);
/* Periodic: remote sync + due reminders. LVGL task only. */
void muse_todo_tick(float now);

/* Apply a structured command (device.todo / chat marker). Any task; UI refresh deferred. */
int muse_todo_apply_cmd(const muse_todo_cmd_t *cmd);
/* Scan assistant text for <<<MUSE_TODO...>>> markers, apply, strip in place. */
int muse_todo_apply_chat_markers(char *text);

/* Snapshot helpers (tests / device.todo list). */
int muse_todo_count(void);
int muse_todo_get(int idx, muse_todo_item_t *out);
/* Tap dismiss / complete on reminder popup — LVGL task. */
void muse_todo_reminder_dismiss(bool complete);

#ifdef __cplusplus
}
#endif
