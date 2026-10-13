/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-LCD-1.85B: ESP32-S3R8 (16 MB flash, 8 MB octal
 * PSRAM), round 1.85" 360x360 ST77916 LCD on QSPI with CST816S touch,
 * ES8311 speaker + ES7210 dual mic, BOOT (GPIO0) for talk, BQ27220 gauge
 * at I2C 0x55. PWR is hardware-only (docs: no custom functions). Native USB
 * Serial/JTAG (D-/D+ on GPIO19/20).
 *
 * Pins from Waveshare docs (docs.waveshare.com/ESP32-S3-Touch-LCD-1.85B
 * Peripheral Quick Reference / GPIO Allocation). ST77916 vendor init and
 * RDID 0x04 panel-revision probe from waveshareteam/ESP32-S3-Touch-LCD-1.85B
 * Examples/Arduino-V3.2.0/examples/01_lvgl_demo/Display_ST77916.cpp.
 * Touch register layout from that repo's Touch_CST816.h (CST816_ADDR 0x15).
 *
 * No waveshare BSP is published for this board on the ESP Component
 * Registry (unlike the AMOLED 1.75/1.75C). Drive esp_lcd ST77916 + codecs
 * directly, as board_aipi.c / board_m5stack_stopwatch.c do.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_lcd_bands.h"
#include "muse_mem.h"

#include "board_waveshare_s3_185b_lcd_init.h"

static const char *TAG = "board";

#define LCD_RES 360
#define LCD_HOST SPI2_HOST
#define LCD_CS GPIO_NUM_21
#define LCD_PCLK GPIO_NUM_40
#define LCD_D0 GPIO_NUM_46
#define LCD_D1 GPIO_NUM_45
#define LCD_D2 GPIO_NUM_42
#define LCD_D3 GPIO_NUM_41
#define LCD_RST GPIO_NUM_3
#define LCD_BL GPIO_NUM_5          /* PWM backlight, active high */
#define DRAW_BUF_LINES 90          /* four bands to the screen (muse_lcd_bands.h) */
#define LCD_CHUNK_BYTES (LCD_RES * 8 * 2)
#define LCD_OPCODE_READ_CMD (0x0BULL)

#define I2C_SCL GPIO_NUM_10
#define I2C_SDA GPIO_NUM_11

#define I2S_MCLK GPIO_NUM_2
#define I2S_BCLK GPIO_NUM_48
#define I2S_WS GPIO_NUM_38
#define I2S_DIN GPIO_NUM_39        /* record */
#define I2S_DOUT GPIO_NUM_47       /* play */
#define PA_EN GPIO_NUM_9           /* amp enable, high = on */

#define TALK_GPIO GPIO_NUM_0       /* BOOT */

#define TP_ADDR 0x15
#define TP_RST GPIO_NUM_1
#define TP_INT GPIO_NUM_4
#define TP_REG_POINTS 0x02         /* finger count, then X and Y, 12 bits each */
#define TP_REG_SLEEP 0xE5          /* 0x03: deep sleep until reset (CST8xx family) */

/* BQ27220 fuel gauge at 7-bit 0x55 on this I2C bus (shared with touch /
 * codecs). Standard commands are 16-bit little-endian. */
#define BQ_ADDR 0x55
#define BQ_REG_VOLT 0x08           /* Voltage(), mV */
#define BQ_REG_FLAGS 0x0A          /* BatteryStatus() */
#define BQ_REG_SOC 0x2C            /* RelativeStateOfCharge(), % */
#define BQ_FLAG_DSG (1u << 0)      /* discharging when set */
#define BQ_FLAG_BATTPRES (1u << 3) /* battery present */

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp;
static i2c_master_dev_handle_t s_gauge;
static bool s_bq_logged;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static muse_gpio_button_t s_boot;

static esp_err_t tp_write(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[8];
    if (n + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, n);
    return i2c_master_transmit(s_tp, buf, n + 1, 50);
}

static esp_err_t tp_read(uint8_t reg, uint8_t *data, size_t n)
{
    return i2c_master_transmit_receive(s_tp, &reg, 1, data, n, 50);
}

static void tp_reset(void)
{
    gpio_set_level(TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    /* Keep the controller answering I2C while idle (Waveshare DisAutoSleep). */
    (void)tp_write(0xFE, (uint8_t[]){ 0x01 }, 1);
}

static esp_err_t init(void)
{
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c");

    const gpio_config_t tp_pins = {
        .pin_bit_mask = (1ULL << TP_RST) | (1ULL << TP_INT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&tp_pins), TAG, "touch pins");
    gpio_set_direction(TP_INT, GPIO_MODE_INPUT);
    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &tp_cfg, &s_tp), TAG, "touch i2c");
    tp_reset();

    const i2c_device_config_t gauge_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BQ_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_probe(s_i2c, BQ_ADDR, 100) != ESP_OK ||
        i2c_master_bus_add_device(s_i2c, &gauge_cfg, &s_gauge) != ESP_OK) {
        ESP_LOGW(TAG, "no BQ27220: battery unavailable");
        s_gauge = NULL;
    } else {
        ESP_LOGI(TAG, "BQ27220 at 0x%02x", BQ_ADDR);
    }

    return muse_gpio_button_init(&s_boot, TALK_GPIO);
}

/* Round 360 px panel: keep invalidate windows even-aligned like CO5300 boards. */
static void round_area(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void tp_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint8_t b[5];
    if (tp_read(TP_REG_POINTS, b, sizeof(b)) != ESP_OK || (b[0] & 0x0F) == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int x = (b[1] & 0x0F) << 8 | b[2];
    int y = (b[3] & 0x0F) << 8 | b[4];
    data->point.x = x < LCD_RES ? x : LCD_RES - 1;
    data->point.y = y < LCD_RES ? y : LCD_RES - 1;
    data->state = LV_INDEV_STATE_PRESSED;
}

/*
 * Probe RDID (0x04) the way Waveshare's Arduino demo does, then pick the
 * matching vendor init table. Waveshare resets RST before the probe; without
 * that, many 1.85B panels return 00 00 00 00 and the old code blindly kept
 * init v1, which garbles the newer (v2) panels.
 */
static void lcd_hw_reset(void)
{
    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&rst));
    gpio_set_level(LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1);
    /* Waveshare uses 50 ms; give the panel a bit more after USB re-plug. */
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void use_st77916_init_v1(st77916_vendor_config_t *vendor)
{
    vendor->init_cmds = s_st77916_init_v1;
    vendor->init_cmds_size = sizeof(s_st77916_init_v1) / sizeof(s_st77916_init_v1[0]);
}

static void use_st77916_init_v2(st77916_vendor_config_t *vendor)
{
    vendor->init_cmds = s_st77916_init_v2;
    vendor->init_cmds_size = sizeof(s_st77916_init_v2) / sizeof(s_st77916_init_v2[0]);
}

static void pick_st77916_init(st77916_vendor_config_t *vendor)
{
    /* Safe default for current Waveshare 1.85B stock: newer panel = v2.
     * Overridden below when RDID clearly identifies a revision. */
    use_st77916_init_v2(vendor);

    lcd_hw_reset();

    esp_lcd_panel_io_handle_t probe_io = NULL;
    /* Match Waveshare Arduino probe clock (5 MHz), not the full draw rate. */
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = 5 * 1000 * 1000;
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &probe_io) != ESP_OK) {
        ESP_LOGW(TAG, "ST77916 RDID probe IO failed; using init v2 (1.85B default)");
        return;
    }

    uint8_t id[4] = { 0 };
    int lcd_cmd = (int)((LCD_OPCODE_READ_CMD << 24) | (0x04 << 8));
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        memset(id, 0, sizeof(id));
        err = esp_lcd_panel_io_rx_param(probe_io, lcd_cmd, id, sizeof(id));
        if (err == ESP_OK && !(id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0)) {
            break;
        }
        ESP_LOGW(TAG, "ST77916 RDID attempt %d: err=%s id=%02x %02x %02x %02x",
                 attempt + 1, esp_err_to_name(err), id[0], id[1], id[2], id[3]);
        lcd_hw_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_lcd_panel_io_del(probe_io);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ST77916 RDID read failed (%s); using init v2 (1.85B default)",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "ST77916 RDID: %02x %02x %02x %02x", id[0], id[1], id[2], id[3]);

    if (id[0] == 0x00 && id[1] == 0x7F && id[2] == 0x7F && id[3] == 0x7F) {
        use_st77916_init_v1(vendor);
        ESP_LOGI(TAG, "ST77916 using vendor init v1 (RDID 00 7F 7F 7F)");
    } else if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        use_st77916_init_v2(vendor);
        ESP_LOGI(TAG, "ST77916 using vendor init v2 (RDID 00 02 7F 7F)");
    } else if (id[0] == 0 && id[1] == 0 && id[2] == 0 && id[3] == 0) {
        ESP_LOGW(TAG, "ST77916 RDID all zeros after reset/retries; using init v2 "
                      "(newer 1.85B panels; old default v1 caused garbled screen)");
    } else {
        ESP_LOGW(TAG, "ST77916 unknown RDID; using init v2 (1.85B default)");
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    /* TODO(verify-on-hardware):
     * - Waveshare's Arduino demo drives a TE pin on GPIO18; that GPIO is not
     *   in the published GPIO Allocation table, so it is left alone here.
     * - Their LVGL path byte-swaps RGB565 before draw_bitmap; muse_lcd_bands
     *   draws in panel order — if colours look swapped, fix byte order here.
     */

    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    if (ledc_timer_config(&bl_timer) != ESP_OK || ledc_channel_config(&bl_ch) != ESP_OK) {
        return NULL;
    }

    const spi_bus_config_t bus =
        ST77916_PANEL_BUS_QSPI_CONFIG(LCD_PCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, LCD_CHUNK_BYTES);
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return NULL;
    }

    st77916_vendor_config_t vendor_cfg = {
        .flags.use_qspi_interface = 1,
    };
    pick_st77916_init(&vendor_cfg);

    const esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_CS, NULL, NULL);
    if (esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &s_io) != ESP_OK) {
        return NULL;
    }
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    if (esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel) != ESP_OK) {
        return NULL;
    }
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_core_id = MUSE_UI_CORE;
    adapter_cfg.task_priority = MUSE_UI_PRIORITY;
    if (esp_lv_adapter_init(&adapter_cfg) != ESP_OK) {
        return NULL;
    }
    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = LCD_RES,
            .ver_res = LCD_RES,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    lv_display_t *disp = muse_lcd_bands_register(disp_cfg, DRAW_BUF_LINES, LCD_CHUNK_BYTES);
    if (!disp) {
        return NULL;
    }
    lv_display_add_event_cb(disp, round_area, LV_EVENT_INVALIDATE_AREA, NULL);

    *touch = lv_indev_create();
    if (!*touch) {
        return NULL;
    }
    lv_indev_set_type(*touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(*touch, disp);
    lv_indev_set_read_cb(*touch, tp_read_cb);
    if (esp_lv_adapter_start() != ESP_OK) {
        return NULL;
    }
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

static void set_brightness(int pct)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, pct * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void send_sleep(void *sleep)
{
    /* QSPI command path: opcode 0x02 << 24 | cmd << 8, as on CO5300 boards. */
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | ((*(bool *)sleep ? 0x10 : 0x11) << 8), NULL, 0);
}

static void panel_sleep(bool sleep)
{
    muse_lcd_bands_run(send_sleep, &sleep);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void display_pause(bool pause)
{
    if (pause) {
        esp_lv_adapter_pause(-1);
        /* TODO(verify-on-hardware): confirm CST816S deep-sleep reg 0xE5=0x03. */
        (void)tp_write(TP_REG_SLEEP, (uint8_t[]){ 0x03 }, 1);
    } else {
        tp_reset();
        esp_lv_adapter_resume();
    }
}

/* ES8311 out and ES7210 in on one duplex I2S bus, MCLK from GPIO2.
 * Docs list 8-bit write addresses 0x30 / 0x80 (= 7-bit 0x18 / 0x40 defaults). */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx, &rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t dac_i2c = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c,
    };
    audio_codec_i2c_cfg_t adc_i2c = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = s_i2c,
    };
    const audio_codec_ctrl_if_t *dac_ctrl = audio_codec_new_i2c_ctrl(&dac_i2c);
    const audio_codec_ctrl_if_t *adc_ctrl = audio_codec_new_i2c_ctrl(&adc_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && dac_ctrl && adc_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t dac_cfg = {
        .ctrl_if = dac_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = PA_EN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *dac = es8311_codec_new(&dac_cfg);
    ESP_RETURN_ON_FALSE(dac, ESP_FAIL, TAG, "ES8311 not responding");
    es7210_codec_cfg_t adc_cfg = {
        .ctrl_if = adc_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
    };
    const audio_codec_if_t *adc = es7210_codec_new(&adc_cfg);
    ESP_RETURN_ON_FALSE(adc, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = dac, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = adc, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

static void set_mic_gain(esp_codec_dev_handle_t mic, int db)
{
    db = (db / 3) * 3;
    esp_codec_dev_set_in_gain(mic, db == 33 ? 34.5f : (float)db);
}

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_boot);
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_boot }, 1, timeout_ms);
}

static esp_err_t bq_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_gauge, &reg, 1, b, sizeof(b), 100);
    if (err != ESP_OK) {
        return err;
    }
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return ESP_OK;
}

static esp_err_t read_power(muse_power_t *out)
{
    out->battery_pct = -1;
    out->battery_mv = 0;
    out->charging = false;
    out->usb = usb_serial_jtag_is_connected();
    if (!s_gauge) {
        return ESP_OK;
    }
    uint16_t mv = 0, flags = 0, soc = 0;
    if (bq_read_u16(BQ_REG_VOLT, &mv) != ESP_OK || bq_read_u16(BQ_REG_FLAGS, &flags) != ESP_OK ||
        bq_read_u16(BQ_REG_SOC, &soc) != ESP_OK) {
        /* Transient I2C miss: keep USB-only rather than inventing a %. */
        return ESP_OK;
    }
    bool present = (flags & BQ_FLAG_BATTPRES) != 0 && mv >= 2500 && mv < 5000;
    if (!s_bq_logged) {
        s_bq_logged = true;
        ESP_LOGI(TAG, "BQ27220 mv=%u soc=%u flags=0x%04x present=%d usb=%d", (unsigned)mv,
                 (unsigned)(soc & 0xFF), (unsigned)flags, (int)present, (int)out->usb);
    }
    if (!present) {
        return ESP_OK;
    }
    out->battery_mv = (int)mv;
    out->battery_pct = soc > 100 ? -1 : (int)(soc & 0xFF);
    /* DSG clear => charge/relax toward charge; with USB, treat as charging. */
    out->charging = out->usb && !(flags & BQ_FLAG_DSG) && out->battery_pct >= 0 && out->battery_pct < 100;
    return ESP_OK;
}

static esp_err_t power_off(void)
{
    /* PWR is hardware-only; deep-sleep until BOOT wakes, as on FNK0104B. */
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    while (gpio_get_level(TALK_GPIO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_RETURN_ON_ERROR(esp_sleep_enable_ext0_wakeup(TALK_GPIO, 0), TAG, "wake button");
    esp_deep_sleep_start();
    return ESP_FAIL;
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-1.85B",
    .width = LCD_RES,
    .height = LCD_RES,
    .round = true,
    .touch = true,
    .diagonal_in = 1.85f,
    .talk_button = "boot",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -12 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = esp_lv_adapter_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .display_pause = display_pause,
    .audio_init = audio_init,
    .mic_slot = -1,             /* mix both ES7210 mics; TODO: confirm slots */
    .set_mic_gain = set_mic_gain,
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
