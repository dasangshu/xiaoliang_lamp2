#include "esp_mjpeg_player.h"
#include "media_src_storage.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

esp_err_t jpeg_to_image(const uint8_t *src, size_t src_len, uint8_t **out, size_t *out_len,
                        size_t *width, size_t *height, size_t *stride);
esp_err_t jpeg_to_image_into(const uint8_t *src, size_t src_len, uint8_t *out_buf, size_t out_buf_size,
                             size_t *out_len, size_t *width, size_t *height, size_t *stride);
void jpeg_decoder_warmup(void);

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
    uint8_t *(*acquire_rgb_buffer)(size_t min_bytes, size_t *out_size, void *ctx);
    void (*present_rgb_buffer)(uint8_t *buf, uint32_t width, uint32_t height, void *ctx);
    void *buffer_ctx;
    void *user_data;
    UBaseType_t task_priority;
    int task_core;
    uint32_t target_fps;
} mjpeg_player_t;

static bool mjpeg_player_decode_frame(mjpeg_player_t *player, const uint8_t *jpeg, size_t jpeg_len,
                                      uint32_t *frame_counter) {
    size_t out_len = 0;
    size_t out_width = 0;
    size_t out_height = 0;
    size_t out_stride = 0;
    esp_err_t ret = ESP_FAIL;

    if (player->acquire_rgb_buffer && player->present_rgb_buffer) {
        size_t target_size = 0;
        uint8_t *target = player->acquire_rgb_buffer(jpeg_len, &target_size, player->buffer_ctx);
        if (target != NULL) {
            ret = jpeg_to_image_into(jpeg, jpeg_len, target, target_size, &out_len, &out_width, &out_height, &out_stride);
            if (ret == ESP_OK) {
                player->present_rgb_buffer(target, (uint32_t)out_width, (uint32_t)out_height, player->buffer_ctx);
                (*frame_counter)++;
                return true;
            }
        }
    }

    uint8_t *out_data = NULL;
    ret = jpeg_to_image(jpeg, jpeg_len, &out_data, &out_len, &out_width, &out_height, &out_stride);
    if (ret == ESP_OK && out_data != NULL) {
        if (player->on_frame_cb) {
            player->on_frame_cb(out_data, (uint32_t)out_width, (uint32_t)out_height, player->user_data);
        }
        heap_caps_free(out_data);
        (*frame_counter)++;
        return true;
    }

    static int last_err = 0;
    if (ret != last_err) {
        ESP_LOGW(TAG, "JPEG decode failed: %d frame_size=%u", ret, (unsigned)jpeg_len);
        last_err = ret;
    }
    return false;
}

static void mjpeg_player_wait_frame_interval(TickType_t *frame_start_tick, TickType_t frame_interval_ticks,
                                             TickType_t min_frame_yield_ticks) {
    if (frame_interval_ticks > 0) {
        TickType_t elapsed = xTaskGetTickCount() - *frame_start_tick;
        if (elapsed < frame_interval_ticks) {
            vTaskDelay(frame_interval_ticks - elapsed);
        } else {
            vTaskDelay(min_frame_yield_ticks);
        }
    } else {
        vTaskDelay(min_frame_yield_ticks);
    }
    *frame_start_tick = xTaskGetTickCount();
}

static bool mjpeg_player_find_jpeg_frame(const uint8_t *data, size_t size, size_t *pos,
                                         const uint8_t **frame_start, size_t *frame_len) {
    while (*pos + 1 < size) {
        if (data[*pos] == 0xFF && data[*pos + 1] == 0xD8) {
            size_t start = *pos;
            *pos += 2;
            while (*pos + 1 < size) {
                if (data[*pos] == 0xFF && data[*pos + 1] == 0xD9) {
                    *pos += 2;
                    *frame_start = data + start;
                    *frame_len = *pos - start;
                    return true;
                }
                (*pos)++;
            }
            return false;
        }
        (*pos)++;
    }
    return false;
}

static void mjpeg_player_process_byte(mjpeg_player_t *player, uint8_t byte, bool *collecting_frame,
                                      uint8_t *previous_byte, size_t *frame_size,
                                      uint32_t *frame_counter, uint32_t *dropped_frames,
                                      TickType_t *frame_start_tick, TickType_t frame_interval_ticks,
                                      TickType_t min_frame_yield_ticks) {
    if (!*collecting_frame) {
        if (*previous_byte == 0xFF && byte == 0xD8) {
            *collecting_frame = true;
            *frame_size = 0;
            player->in_buff[(*frame_size)++] = 0xFF;
            player->in_buff[(*frame_size)++] = 0xD8;
        }
        *previous_byte = byte;
        return;
    }

    if (*frame_size >= player->in_buff_size) {
        (*dropped_frames)++;
        if (((*dropped_frames) & 0x07) == 1) {
            ESP_LOGW(TAG, "JPEG frame exceeds input buffer (%u bytes), dropping (count=%u)",
                     (unsigned)player->in_buff_size, (unsigned)(*dropped_frames));
        }
        *collecting_frame = false;
        *frame_size = 0;
        *previous_byte = byte;
        return;
    }

    player->in_buff[(*frame_size)++] = byte;

    if (*previous_byte == 0xFF && byte == 0xD9) {
        if (mjpeg_player_decode_frame(player, player->in_buff, *frame_size, frame_counter)) {
            mjpeg_player_wait_frame_interval(frame_start_tick, frame_interval_ticks, min_frame_yield_ticks);
        }

        *collecting_frame = false;
        *frame_size = 0;
        *previous_byte = byte;
        return;
    }

    *previous_byte = byte;
}

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
    const TickType_t min_frame_yield_ticks = pdMS_TO_TICKS(1);

    ESP_LOGI(TAG, "MJPEG player task started (target_fps=%u)", (unsigned)player->target_fps);

    TickType_t frame_start_tick = xTaskGetTickCount();

    const uint8_t *preload_data = NULL;
    size_t preload_size = 0;
    size_t preload_pos = 0;
    bool use_preload = media_src_storage_is_preloaded(&player->file) &&
        media_src_storage_get_preload_view(&player->file, &preload_data, &preload_size, &preload_pos) == 0;

    if (use_preload) {
        ESP_LOGI(TAG, "Using preloaded PSRAM buffer (%u bytes)", (unsigned)preload_size);
        while (player->is_playing) {
            const uint8_t *frame_start = NULL;
            size_t frame_len = 0;
            if (!mjpeg_player_find_jpeg_frame(preload_data, preload_size, &preload_pos, &frame_start, &frame_len)) {
                if (player->is_loop) {
                    preload_pos = 0;
                    continue;
                }
                break;
            }

            if (frame_len <= player->in_buff_size &&
                mjpeg_player_decode_frame(player, frame_start, frame_len, &frame_counter)) {
                mjpeg_player_wait_frame_interval(&frame_start_tick, frame_interval_ticks, min_frame_yield_ticks);
            } else if (frame_len > player->in_buff_size) {
                dropped_frames++;
                if ((dropped_frames & 0x07) == 1) {
                    ESP_LOGW(TAG, "JPEG frame exceeds input buffer (%u bytes), dropping (count=%u)",
                             (unsigned)player->in_buff_size, (unsigned)dropped_frames);
                }
            }
        }
        media_src_storage_set_preload_pos(&player->file, preload_pos);
    } else {
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
                mjpeg_player_process_byte(player, player->cache_buff[i], &collecting_frame, &previous_byte,
                                          &frame_size, &frame_counter, &dropped_frames, &frame_start_tick,
                                          frame_interval_ticks, min_frame_yield_ticks);
            }
        }
    }

    player->is_playing = false;
    ESP_LOGI(TAG, "MJPEG player task finished (frames: %u)", (unsigned)frame_counter);
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

#if CONFIG_SPIRAM
    bool preload_enabled = config->preload_to_psram;
#else
    bool preload_enabled = false;
#endif
    media_src_storage_set_preload(&player->file, preload_enabled);

    player->on_frame_cb = config->on_frame_cb;
    player->acquire_rgb_buffer = config->acquire_rgb_buffer;
    player->present_rgb_buffer = config->present_rgb_buffer;
    player->buffer_ctx = config->buffer_ctx;
    player->user_data = config->user_data;
    player->target_fps = (config->target_fps > 0) ? config->target_fps : 30;

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

esp_err_t mjpeg_player_set_target_fps(mjpeg_player_handle_t handle, uint32_t fps) {
    mjpeg_player_t *player = (mjpeg_player_t *)handle;
    if (!player) {
        return ESP_ERR_INVALID_ARG;
    }
    player->target_fps = fps > 0 ? fps : 30;
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
