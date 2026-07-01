#include "esp_mjpeg_player.h"
#include "media_src_storage.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Reuse the existing jpeg_to_image from the current project
// This handles both HW (P4) and SW JPEG decoding automatically
esp_err_t jpeg_to_image(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len,
                        size_t *width, size_t *height, size_t *stride);

static const char *TAG = "mjpeg_player";

typedef struct {
    volatile bool is_playing;
    bool is_loop;
    TaskHandle_t task_handle;
    media_src_t file;

    uint8_t *in_buff;
    uint32_t in_buff_size;
    uint8_t *cache_buff;
    size_t cache_buff_size;

    void (*on_frame_cb)(uint8_t *rgb565, uint32_t width, uint32_t height, void *ctx);
    void *user_data;
    UBaseType_t task_priority;
    int task_core;
    uint32_t target_fps;
} mjpeg_player_t;

static void mjpeg_player_task(void *arg) {
    mjpeg_player_t *player = (mjpeg_player_t *)arg;
    size_t bytes_read = 0;
    uint32_t frame_counter = 0;
    uint32_t dropped_frames = 0;
    bool collecting_frame = false;
    uint8_t previous_byte = 0;
    size_t frame_size = 0;
    TickType_t frame_interval_ticks = (player->target_fps > 0)
        ? pdMS_TO_TICKS(1000 / player->target_fps) : 0;
    const TickType_t min_frame_yield_ticks = pdMS_TO_TICKS(10);

    ESP_LOGI(TAG, "MJPEG player task started (target_fps=%u)", (unsigned)player->target_fps);

    TickType_t frame_start_tick = xTaskGetTickCount();
    while (player->is_playing) {
        bytes_read = media_src_storage_read(&player->file, player->cache_buff, player->cache_buff_size);

        if (bytes_read <= 0) {
            if (player->is_loop) {
                ESP_LOGD(TAG, "End of file, restarting loop...");
                media_src_storage_seek(&player->file, 0);
                collecting_frame = false;
                previous_byte = 0;
                frame_size = 0;
                continue;
            }
            ESP_LOGD(TAG, "End of file, stopping playback");
            break;
        }

        for (size_t i = 0; i < bytes_read && player->is_playing; i++) {
            uint8_t byte = player->cache_buff[i];

            if (!collecting_frame) {
                if (previous_byte == 0xFF && byte == 0xD8) {
                    collecting_frame = true;
                    frame_size = 0;
                    player->in_buff[frame_size++] = 0xFF;
                    player->in_buff[frame_size++] = 0xD8;
                }
                previous_byte = byte;
                continue;
            }

            if (frame_size >= player->in_buff_size) {
                dropped_frames++;
                if ((dropped_frames & 0x07) == 1) {
                    ESP_LOGW(TAG, "JPEG frame exceeds input buffer (%u bytes), dropping (count=%u)",
                             (unsigned)player->in_buff_size, (unsigned)dropped_frames);
                }
                collecting_frame = false;
                frame_size = 0;
                previous_byte = byte;
                continue;
            }

            player->in_buff[frame_size++] = byte;

            if (previous_byte == 0xFF && byte == 0xD9) {
                uint8_t *out_data = NULL;
                size_t out_len = 0;
                size_t out_width = 0;
                size_t out_height = 0;
                size_t out_stride = 0;

                esp_err_t ret = jpeg_to_image(player->in_buff, frame_size,
                                              &out_data, &out_len,
                                              &out_width, &out_height, &out_stride);
                if (ret == ESP_OK && out_data != NULL) {
                    if (player->on_frame_cb) {
                        player->on_frame_cb(out_data, (uint32_t)out_width,
                                            (uint32_t)out_height, player->user_data);
                    }
                    heap_caps_free(out_data);
                    frame_counter++;
                } else {
                    static int last_err = 0;
                    if (ret != last_err) {
                        ESP_LOGW(TAG, "JPEG decode failed: %d frame_size=%zu",
                                 ret, frame_size);
                        last_err = ret;
                    }
                }

                collecting_frame = false;
                frame_size = 0;

                // Frame rate control: sleep remainder of frame interval so audio/wake-word tasks get CPU
                if (frame_interval_ticks > 0) {
                    TickType_t elapsed = xTaskGetTickCount() - frame_start_tick;
                    if (elapsed < frame_interval_ticks) {
                        vTaskDelay(frame_interval_ticks - elapsed);
                    } else {
                        vTaskDelay(min_frame_yield_ticks);
                    }
                } else {
                    vTaskDelay(min_frame_yield_ticks);
                }
                frame_start_tick = xTaskGetTickCount();
            }

            previous_byte = byte;
        }
    }

    player->is_playing = false;
    ESP_LOGI(TAG, "MJPEG player task finished (frames: %u)", frame_counter);
    player->task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t mjpeg_player_create(const mjpeg_player_config_t *config, mjpeg_player_handle_t *handle) {
    if (!config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    mjpeg_player_t *player = (mjpeg_player_t *)heap_caps_calloc(1, sizeof(mjpeg_player_t), MALLOC_CAP_INTERNAL);
    if (!player) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_SPIRAM
    uint32_t psram_size_mb = (uint32_t)(esp_psram_get_size() / 1024 / 1024);
    if (psram_size_mb >= 16) {
        player->in_buff_size = config->frame_buffer_size ? config->frame_buffer_size : 300 * 1024;
        player->cache_buff_size = config->cache_buffer_size ? config->cache_buffer_size : 256 * 1024;
        ESP_LOGI(TAG, "PSRAM %uMB: Using %uKB input + %uKB cache", (unsigned)psram_size_mb, (unsigned)(player->in_buff_size / 1024), (unsigned)(player->cache_buff_size / 1024));
    } else if (psram_size_mb >= 8) {
        player->in_buff_size = config->frame_buffer_size ? config->frame_buffer_size : 150 * 1024;
        player->cache_buff_size = config->cache_buffer_size ? config->cache_buffer_size : 128 * 1024;
    } else {
        player->in_buff_size = config->frame_buffer_size ? config->frame_buffer_size : 96 * 1024;
        player->cache_buff_size = config->cache_buffer_size ? config->cache_buffer_size : 64 * 1024;
    }
#else
    player->in_buff_size = config->frame_buffer_size ? config->frame_buffer_size : 64 * 1024;
    player->cache_buff_size = config->cache_buffer_size ? config->cache_buffer_size : 32 * 1024;
#endif

    // Allocate input buffer in PSRAM to avoid starving internal RAM (used by AFE, etc.)
    player->in_buff = heap_caps_malloc(player->in_buff_size, MALLOC_CAP_SPIRAM);
    if (!player->in_buff) {
        ESP_LOGE(TAG, "Failed to allocate input buffer");
        heap_caps_free(player);
        return ESP_ERR_NO_MEM;
    }

    if (config->cache_in_psram) {
        player->cache_buff = heap_caps_malloc(player->cache_buff_size, MALLOC_CAP_SPIRAM);
        if (!player->cache_buff) {
            ESP_LOGW(TAG, "PSRAM cache failed, fallback to internal RAM");
            player->cache_buff = malloc(player->cache_buff_size);
        }
    } else {
        player->cache_buff = heap_caps_malloc(player->cache_buff_size, MALLOC_CAP_SPIRAM);
        if (!player->cache_buff) {
            player->cache_buff = malloc(player->cache_buff_size);
        }
    }

    if (!player->cache_buff) {
        ESP_LOGE(TAG, "Failed to allocate cache buffer");
        heap_caps_free(player->in_buff);
        heap_caps_free(player);
        return ESP_ERR_NO_MEM;
    }

    if (media_src_storage_open(&player->file) != 0) {
        ESP_LOGE(TAG, "Failed to open media source");
        heap_caps_free(player->cache_buff);
        heap_caps_free(player->in_buff);
        heap_caps_free(player);
        return ESP_FAIL;
    }

    player->on_frame_cb = config->on_frame_cb;
    player->user_data = config->user_data;
    player->target_fps = (config->target_fps > 0) ? config->target_fps : 15;

    UBaseType_t requested_priority = (config->task_priority > 0) ? (UBaseType_t)config->task_priority : (tskIDLE_PRIORITY + 5);
    if (requested_priority >= configMAX_PRIORITIES) {
        requested_priority = configMAX_PRIORITIES - 1;
    }
    player->task_priority = requested_priority;

    if (config->task_core >= 0 && config->task_core < portNUM_PROCESSORS) {
        player->task_core = config->task_core;
    } else {
        player->task_core = -1;
    }

    *handle = player;
    ESP_LOGI(TAG, "MJPEG player created (input=%uKB, cache=%uKB)", (unsigned)(player->in_buff_size / 1024), (unsigned)(player->cache_buff_size / 1024));
    return ESP_OK;
}

esp_err_t mjpeg_player_play_file(mjpeg_player_handle_t handle, const char *filepath) {
    mjpeg_player_t *player = (mjpeg_player_t *)handle;
    if (!player || !filepath) {
        return ESP_ERR_INVALID_ARG;
    }

    if (player->is_playing || player->task_handle != NULL) {
        esp_err_t stop_ret = mjpeg_player_stop(handle);
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "Previous playback did not stop, skip new file: %s", filepath);
            return stop_ret;
        }
    }

    if (media_src_storage_connect(&player->file, filepath) != 0) {
        ESP_LOGE(TAG, "Failed to connect to file: %s", filepath);
        return ESP_FAIL;
    }

    uint64_t file_size = 0;
    if (media_src_storage_get_size(&player->file, &file_size) != 0) {
        ESP_LOGE(TAG, "Failed to get file size");
        media_src_storage_disconnect(&player->file);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Playing file: %s (%" PRIu64 " bytes)", filepath, file_size);

    media_src_storage_seek(&player->file, 0);
    player->is_playing = true;

    BaseType_t ret;
    if (player->task_core >= 0 && player->task_core < portNUM_PROCESSORS) {
        ret = xTaskCreatePinnedToCore(mjpeg_player_task, "mjpeg_player",
            16 * 1024, player, player->task_priority, &player->task_handle, player->task_core);
    } else {
        ret = xTaskCreate(mjpeg_player_task, "mjpeg_player",
            16 * 1024, player, player->task_priority, &player->task_handle);
    }

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create player task");
        player->is_playing = false;
        media_src_storage_disconnect(&player->file);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mjpeg_player_stop(mjpeg_player_handle_t handle) {
    mjpeg_player_t *player = (mjpeg_player_t *)handle;
    if (!player) {
        return ESP_ERR_INVALID_ARG;
    }

    if (player->is_playing || player->task_handle != NULL) {
        player->is_playing = false;

        if (player->task_handle) {
            uint32_t timeout = 2000;
            while (player->task_handle != NULL && timeout-- > 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (timeout == 0) {
                ESP_LOGW(TAG, "Task did not finish in 2000ms; leaving it to exit safely");
                return ESP_ERR_TIMEOUT;
            }
        }

        media_src_storage_disconnect(&player->file);
        ESP_LOGD(TAG, "Player stopped");
    }

    return ESP_OK;
}

esp_err_t mjpeg_player_set_loop(mjpeg_player_handle_t handle, bool enable) {
    mjpeg_player_t *player = (mjpeg_player_t *)handle;
    if (!player) {
        return ESP_ERR_INVALID_ARG;
    }
    player->is_loop = enable;
    return ESP_OK;
}

esp_err_t mjpeg_player_destroy(mjpeg_player_handle_t handle) {
    mjpeg_player_t *player = (mjpeg_player_t *)handle;
    if (!player) {
        return ESP_ERR_INVALID_ARG;
    }

    mjpeg_player_stop(handle);
    media_src_storage_close(&player->file);

    if (player->in_buff) {
        heap_caps_free(player->in_buff);
    }
    if (player->cache_buff) {
        heap_caps_free(player->cache_buff);
    }
    heap_caps_free(player);
    ESP_LOGI(TAG, "MJPEG player destroyed");
    return ESP_OK;
}
