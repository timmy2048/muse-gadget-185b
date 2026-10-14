/*
 * Azure STT via VPS WireGuard proxy (secrets stay on VPS).
 * POST PCM/WAV to CONFIG_MUSE_STT_URL; returns malloc'd UTF-8 text or NULL.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#if CONFIG_MUSE_STT_HTTP
/* Recognize mono PCM (rate = MUSE_AUDIO_RATE). Returns heap string (caller frees) or NULL. */
char *muse_stt_recognize_pcm(const int16_t *pcm, size_t frames);
/*
 * Same as muse_stt_recognize_pcm, but asks the VPS for auto language detection
 * (?lang=auto). On success, writes "zh" or "en" into lang_out when provided.
 */
char *muse_stt_recognize_pcm_auto(const int16_t *pcm, size_t frames, char *lang_out, size_t lang_cap);
#endif
