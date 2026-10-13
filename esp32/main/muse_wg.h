/*
 * Optional WireGuard outbound tunnel so Muse HTTPS can reach api.muse.ai
 * on networks that block foreign destinations. Keys come from Kconfig
 * (use devices/sdkconfig.wg.local — gitignored — never commit secrets).
 */
#pragma once

#include <stdbool.h>

void muse_wg_notify_wifi_up(void);
void muse_wg_notify_wifi_down(void);
bool muse_wg_is_up(void);
