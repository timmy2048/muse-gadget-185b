/*
 * Stock watchlist app (preview): 360x360 round-screen quotes, dark theme,
 * red-up / green-down.
 *
 * Watchlist source, in priority order:
 *   1. Local NVS list (device: NVS namespace "stock", key "watchlist";
 *      simulator: ./nvs_stock_watchlist.json in the working directory).
 *      Written by the in-app manager below; survives reboots.
 *   2. Remote JSON: https://raw.githubusercontent.com/timmy2048/muse-gadget-185b/main/watchlist.json
 *      (a JSON array of symbol strings, e.g. ["META","SPCX"]), fetched on
 *      boot and on every periodic refresh. Editing the GitHub file is the
 *      no-rebuild way to change the list from chat.
 *   3. Built-in default (META, SPCX) when both above fail.
 *
 * In-app manager: the quotes tile has a Manage button -> edit view where
 * each symbol can be deleted and new ones added via text entry. On the
 * 999.1.6 board the text entry uses the T9/26-key input method; the
 * simulator feeds SDL keyboard events through muse_stock_text_*().
 *
 * Real-time updates: during US trading hours (Mon-Fri 9:30-16:00 ET) the
 * app polls every 60 s. Each poll refreshes quotes and re-fetches the
 * remote watchlist.json, comparing its content hash; a changed list is
 * adopted immediately and the UI rebuilt, no restart needed (a chat-side
 * edit lands on the board within ~1 min). A locally managed (NVS) list is
 * never overridden by the remote file. Outside trading hours polling stops
 * to save power. On-board edits via the manager apply instantly.
 *
 * Quotes in this preview are DEMO data. The real quote feed plugs into
 * stock_quote_refresh() later.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Build the stocks tile content. LVGL task only. */
void muse_stock_build(lv_obj_t *tile, int w, int h);
/* Slide the tileview to the stocks tile (quotes view). LVGL task only. */
void muse_stock_show(void);
/* Select a quote row (0-based); -1 clears the selection. LVGL task only. */
void muse_stock_select(int idx);
/* Open the in-app watchlist manager. LVGL task only. */
void muse_stock_manage(void);
/* Open the "add symbol" text entry. LVGL task only. */
void muse_stock_add_begin(void);
/* Delete a watchlist entry (persists to NVS). LVGL task only. */
void muse_stock_delete(int idx);
/* Periodic refresh: during trading hours, poll quotes every 60 s; each
 * poll also re-fetches the remote watchlist and adopts it live when its
 * content hash changed. No polling outside trading hours. LVGL task only. */
void muse_stock_tick(float now);
/* Test/sim hook: force the trading-hours state (<0 automatic, 0 off, >0 on). */
void muse_stock_set_trading_override(int v);
/* Test/sim hook: scroll the quote list so row idx is at the top. */
void muse_stock_scroll_to(int idx);

/* Text entry for the "add symbol" view. The simulator routes SDL keyboard
 * events here; on device the board's input method calls the same hooks. */
bool muse_stock_text_active(void);
void muse_stock_text_char(char c);
void muse_stock_text_backspace(void);
void muse_stock_text_done(void);
void muse_stock_text_cancel(void);
