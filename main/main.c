/* Play mp3 file by audio pipeline
   with possibility to start, stop, pause and resume playback
   as well as adjust volume

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "board.h"

static const char *TAG = "PLAY_FLASH_MP3_CONTROL";

static struct marker
{
    int pos;
    const uint8_t *start;
    const uint8_t *end;
} file_marker;

extern const uint8_t mp3_file_start[] asm("_binary_1_mp3_start");
extern const uint8_t mp3_file_end[] asm("_binary_1_mp3_end");

static int mp3_music_read_cb_impl(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
    int read_size = file_marker.end - file_marker.start - file_marker.pos;
    if (read_size == 0)
    {
        return AEL_IO_DONE;
    }
    else if (len < read_size)
    {
        read_size = len;
    }
    memcpy(buf, file_marker.start + file_marker.pos, read_size);
    file_marker.pos += read_size;
    return read_size;
}

int mp3_music_read_cb(audio_element_handle_t el, char *buf, int len, TickType_t wait_time, void *ctx)
{
    return mp3_music_read_cb_impl(el, buf, len, wait_time, ctx);
}

void app_main(void)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_writer, mp3_decoder;

    esp_log_level_set("*", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    audio_hal_set_volume(board_handle->audio_hal, 80);

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline, add all elements to pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create mp3 decoder to decode mp3 file and set custom read callback");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);
    audio_element_set_read_cb(mp3_decoder, mp3_music_read_cb, NULL);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
#if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
#endif
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.4] Link it together [mp3_music_read_cb]-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[2] = {"mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 2);

    ESP_LOGI(TAG, "[ 3 ] Start audio_pipeline");
    file_marker.start = mp3_file_start;
    file_marker.end = mp3_file_end;
    file_marker.pos = 0;
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 4 ] Playing 1.mp3 (no button control)");
    vTaskDelay(portMAX_DELAY); // Keep playing indefinitely
}