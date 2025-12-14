/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c.h"
#include "board.h"
#include "es8311_codec.h"
#include "es8311.h"

/* Fallbacks for older ADF/IDF where these macros may be missing */
#ifndef AUDIO_HAL_CTRL_RESUME
#define AUDIO_HAL_CTRL_RESUME AUDIO_HAL_CTRL_START
#endif
#ifndef AUDIO_HAL_CTRL_PAUSE
#define AUDIO_HAL_CTRL_PAUSE AUDIO_HAL_CTRL_STOP
#endif

/* Fallbacks for ES8311 symbols if the header is absent in the include path */
#ifndef ES8311_RESOLUTION_16
typedef enum
{
    ES8311_RESOLUTION_16 = 16,
    ES8311_RESOLUTION_18 = 18,
    ES8311_RESOLUTION_20 = 20,
    ES8311_RESOLUTION_24 = 24,
    ES8311_RESOLUTION_32 = 32,
} es8311_resolution_t;
#endif

#ifndef ES8311_CLOCK_CONFIG_FWD
#define ES8311_CLOCK_CONFIG_FWD
typedef struct es8311_clock_config_t
{
    bool mclk_inverted;
    bool sclk_inverted;
    bool mclk_from_mclk_pin;
    int mclk_frequency;
    int sample_frequency;
} es8311_clock_config_t;
#endif

#ifndef ES8311_ADDRRES_0
#define ES8311_ADDRRES_0 0x18u
#endif

#ifndef es8311_handle_t
typedef void *es8311_handle_t;
#endif

#ifndef ES8311_API_FWD
#define ES8311_API_FWD
es8311_handle_t es8311_create(const i2c_port_t port, const uint16_t dev_addr);
void es8311_delete(es8311_handle_t dev);
esp_err_t es8311_init(es8311_handle_t dev, const es8311_clock_config_t *const clk_cfg,
                      const es8311_resolution_t res_in, const es8311_resolution_t res_out);
esp_err_t es8311_sample_frequency_config(es8311_handle_t dev, int mclk_frequency, int sample_frequency);
esp_err_t es8311_microphone_config(es8311_handle_t dev, bool digital_mic);
esp_err_t es8311_voice_volume_set(es8311_handle_t dev, int volume, int *volume_set);
#endif

static const char *TAG = "es8311_board_codec";

/* Keep the ES8311 wiring compatible with the example I2S playback */
#define ES8311_I2C_PORT I2C_NUM_0
#define ES8311_I2C_CLK_HZ (100000) /* Standard mode is enough */
#define ES8311_MCLK_MULTIPLE (256)

static es8311_handle_t s_es8311 = NULL;
static int s_volume = 60;
static bool s_muted = false;
static bool s_i2c_started = false;

static int hal_samples_to_rate(audio_hal_iface_samples_t samples)
{
    switch (samples)
    {
    case AUDIO_HAL_08K_SAMPLES:
        return 8000;
    case AUDIO_HAL_11K_SAMPLES:
        return 11025;
    case AUDIO_HAL_16K_SAMPLES:
        return 16000;
    case AUDIO_HAL_22K_SAMPLES:
        return 22050;
    case AUDIO_HAL_24K_SAMPLES:
        return 24000;
    case AUDIO_HAL_32K_SAMPLES:
        return 32000;
    case AUDIO_HAL_44K_SAMPLES:
        return 44100;
    case AUDIO_HAL_48K_SAMPLES:
        return 48000;
    default:
        return 44100;
    }
}

static es8311_resolution_t hal_bits_to_resolution(audio_hal_iface_bits_t bits)
{
    switch (bits)
    {
    case AUDIO_HAL_BIT_LENGTH_24BITS:
        return ES8311_RESOLUTION_24;
    case AUDIO_HAL_BIT_LENGTH_32BITS:
        return ES8311_RESOLUTION_32;
    case AUDIO_HAL_BIT_LENGTH_16BITS:
    default:
        return ES8311_RESOLUTION_16;
    }
}

static esp_err_t es8311_setup_clock(int sample_rate, es8311_resolution_t res)
{
    const es8311_clock_config_t clk_cfg = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = sample_rate * ES8311_MCLK_MULTIPLE,
        .sample_frequency = sample_rate,
    };

    ESP_RETURN_ON_ERROR(es8311_init(s_es8311, &clk_cfg, res, res), TAG, "es8311 init failed");
    return es8311_sample_frequency_config(s_es8311, clk_cfg.mclk_frequency, sample_rate);
}

static esp_err_t es8311_i2c_bus_init(void)
{
    if (s_i2c_started)
    {
        return ESP_OK;
    }

    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = ES8311_I2C_CLK_HZ,
    };

    ESP_RETURN_ON_ERROR(get_i2c_pins(ES8311_I2C_PORT, &i2c_cfg), TAG, "get i2c pins failed");
    ESP_RETURN_ON_ERROR(i2c_param_config(ES8311_I2C_PORT, &i2c_cfg), TAG, "i2c param config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(ES8311_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c driver install failed");
    s_i2c_started = true;
    return ESP_OK;
}

audio_hal_func_t AUDIO_NEW_CODEC_DEFAULT_HANDLE = {
    .audio_codec_initialize = new_codec_init,
    .audio_codec_deinitialize = new_codec_deinit,
    .audio_codec_ctrl = new_codec_ctrl_state,
    .audio_codec_config_iface = new_codec_config_i2s,
    .audio_codec_set_mute = new_codec_set_voice_mute,
    .audio_codec_set_volume = new_codec_set_voice_volume,
    .audio_codec_get_volume = new_codec_get_voice_volume,
};

bool new_codec_initialized()
{
    return s_es8311 != NULL;
}

esp_err_t new_codec_init(audio_hal_codec_config_t *cfg)
{
    ESP_LOGI(TAG, "Initializing ES8311");

    ESP_RETURN_ON_ERROR(es8311_i2c_bus_init(), TAG, "i2c init failed");

    if (!s_es8311)
    {
        s_es8311 = es8311_create(ES8311_I2C_PORT, ES8311_ADDRRES_0);
        ESP_RETURN_ON_FALSE(s_es8311, ESP_FAIL, TAG, "es8311 handle create failed");
    }

    const int sample_rate = hal_samples_to_rate(cfg->i2s_iface.samples);
    const es8311_resolution_t res = hal_bits_to_resolution(cfg->i2s_iface.bits);

    ESP_RETURN_ON_ERROR(es8311_setup_clock(sample_rate, res), TAG, "clock setup failed");

    /* Configure microphone path off for playback-only use case */
    ESP_RETURN_ON_ERROR(es8311_microphone_config(s_es8311, false), TAG, "mic config failed");

    /* Set an initial volume */
    ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_es8311, s_volume, NULL), TAG, "volume set failed");
    s_muted = false;

    ESP_LOGI(TAG, "ES8311 ready: %d Hz, %d bits", sample_rate, cfg->i2s_iface.bits);
    return ESP_OK;
}

esp_err_t new_codec_deinit(void)
{
    if (s_es8311)
    {
        es8311_delete(s_es8311);
        s_es8311 = NULL;
    }
    if (s_i2c_started)
    {
        i2c_driver_delete(ES8311_I2C_PORT);
        s_i2c_started = false;
    }
    return ESP_OK;
}

esp_err_t new_codec_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    if (!s_es8311)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (ctrl_state == AUDIO_HAL_CTRL_START || ctrl_state == AUDIO_HAL_CTRL_RESUME)
    {
        return new_codec_set_voice_mute(false);
    }
    if (ctrl_state == AUDIO_HAL_CTRL_STOP || ctrl_state == AUDIO_HAL_CTRL_PAUSE)
    {
        return new_codec_set_voice_mute(true);
    }
    return ESP_OK;
}

esp_err_t new_codec_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    if (!s_es8311 || !iface)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int sample_rate = hal_samples_to_rate(iface->samples);
    const es8311_resolution_t res = hal_bits_to_resolution(iface->bits);
    return es8311_setup_clock(sample_rate, res);
}

esp_err_t new_codec_set_voice_mute(bool mute)
{
    s_muted = mute;
    if (!s_es8311)
    {
        return ESP_OK;
    }

    const int target = mute ? 0 : s_volume;
    return es8311_voice_volume_set(s_es8311, target, NULL);
}

esp_err_t new_codec_set_voice_volume(int volume)
{
    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 100)
    {
        volume = 100;
    }

    s_volume = volume;
    if (s_muted || !s_es8311)
    {
        return ESP_OK;
    }

    return es8311_voice_volume_set(s_es8311, volume, NULL);
}

esp_err_t new_codec_get_voice_volume(int *volume)
{
    if (!volume)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *volume = s_muted ? 0 : s_volume;
    return ESP_OK;
}
