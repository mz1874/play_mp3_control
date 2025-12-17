/* Play mp3 file by audio pipeline
   with possibility to start, stop, pause and resume playback
   as well as adjust volume

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_mem.h"
#include "audio_common.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "fatfs_stream.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "board.h"

static const char *TAG = "PLAY_SD_MP3";

#define MOUNT_POINT "/sdcard"

void app_main(void)
{
    const char *file_path = MOUNT_POINT "/1.mp3";

    ESP_LOGI(TAG, "[ 0 ] Init SD card and FATFS");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_SDR50; // Set to 40 MHz (max supported); default is 20 MHz

#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create on-chip LDO power control driver");
        return;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#else
    slot_config.width = 1;
#endif

#if SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = CONFIG_EXAMPLE_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_PIN_D0;
#ifdef CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4
    slot_config.d1 = CONFIG_EXAMPLE_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_PIN_D3;
#endif
#endif

    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem at %s", MOUNT_POINT);
    ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. Set EXAMPLE_FORMAT_IF_MOUNT_FAILED to format automatically.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Check SD pin connections and pull-ups.", esp_err_to_name(ret));
        }
        return;
    }

    sdmmc_card_print_info(stdout, card);

    audio_pipeline_handle_t pipeline;
    audio_event_iface_handle_t evt;
    audio_element_handle_t i2s_stream_writer, mp3_decoder, file_stream;

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

    ESP_LOGI(TAG, "[2.1] Create FATFS stream reader");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.type = AUDIO_STREAM_READER;
    file_stream = fatfs_stream_init(&fatfs_cfg);
    audio_element_set_uri(file_stream, file_path);

    ESP_LOGI(TAG, "[2.2] Create mp3 decoder");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[2.3] Create i2s stream to write data to codec chip");
#if defined CONFIG_ESP32_C3_LYRA_V2_BOARD
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_PDM_TX_CFG_DEFAULT();
#else
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
#endif
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, file_stream, "file");
    audio_pipeline_register(pipeline, mp3_decoder, "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together file->mp3->i2s");
    const char *link_tag[3] = {"file", "mp3", "i2s"};
    audio_pipeline_link(pipeline, link_tag, 3);

    ESP_LOGI(TAG, "[2.6] Set up event listener for end-of-stream");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[ 3 ] Start audio_pipeline from SD: %s", file_path);
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 4 ] Playing from SD (wait for completion)");

    while (1)
    {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT &&
            msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
            (int)msg.data == AEL_STATUS_STATE_FINISHED)
        {
            ESP_LOGI(TAG, "Playback finished");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 5 ] Stopping pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);
    audio_pipeline_remove_listener(pipeline);
    audio_event_iface_destroy(evt);

    ESP_LOGI(TAG, "[ 6 ] Unmount SD card");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_del_on_chip_ldo(host.pwr_ctrl_handle);
#endif

    ESP_LOGI(TAG, "[ 7 ] Done");
    vTaskDelay(portMAX_DELAY);
}