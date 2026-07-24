#include "mjpeg_player_port.h"
#include "esp_mjpeg_player.h"
#include "esp_heap_caps.h"
#include "fs_manager.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "lcd_display.h"
#include "lvgl_display/jpg/jpeg_to_image.h"
#include <string.h>
#include "freertos/task.h"

static const char *TAG = "mjpeg_player_port";

static uint8_t* player_acquire_rgb_buffer(size_t min_bytes, size_t *out_size, void *ctx) {
    (void)ctx;
    auto display = dynamic_cast<LcdDisplay *>(Board::GetInstance().GetDisplay());
    if (!display) {
        return nullptr;
    }
    return display->AcquireFaceDecodeBuffer(min_bytes, out_size);
}

static void player_present_rgb_buffer(uint8_t *buf, uint32_t width, uint32_t height, void *ctx) {
    (void)ctx;
    auto display = dynamic_cast<LcdDisplay *>(Board::GetInstance().GetDisplay());
    if (display) {
        display->PresentFaceDecodeBuffer(buf, width, height);
    }
}

static void player_frame_callback(uint8_t *rgb565, uint32_t width, uint32_t height, void *ctx) {
    (void)ctx;
    auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetFaceImage(rgb565, width, height);
    }
}

typedef enum {
    PLAYER_TASK_PLAY,
    PLAYER_TASK_STOP,
    PLAYER_TASK_SET_LOOP
} player_task_type_t;

typedef struct {
    player_task_type_t type;
    char filepath[256];
    bool loop_mode;
} player_task_t;

typedef enum {
    PLAYER_STATE_IDLE,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_STOPPING,
    PLAYER_STATE_SWITCHING
} player_state_t;

typedef struct {
    mjpeg_player_handle_t handle;
    QueueHandle_t task_queue;
    TaskHandle_t manager_task;
    SemaphoreHandle_t state_mutex;
    player_state_t state;
    bool loop_enabled;
    char current_file[256];
    volatile bool shutdown_requested;
} player_context_t;

static player_context_t s_player = {
    .handle = NULL,
    .task_queue = NULL,
    .manager_task = NULL,
    .state_mutex = NULL,
    .state = PLAYER_STATE_IDLE,
    .loop_enabled = true,
    .current_file = {0},
    .shutdown_requested = false
};

static player_state_t get_player_state(void) {
    player_state_t state = PLAYER_STATE_IDLE;
    if (s_player.state_mutex && xSemaphoreTake(s_player.state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = s_player.state;
        xSemaphoreGive(s_player.state_mutex);
    }
    return state;
}

static bool set_player_state(player_state_t new_state) {
    bool success = false;
    if (s_player.state_mutex && xSemaphoreTake(s_player.state_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        ESP_LOGI(TAG, "State: %d -> %d", s_player.state, new_state);
        s_player.state = new_state;
        success = true;
        xSemaphoreGive(s_player.state_mutex);
    }
    return success;
}

static uint32_t target_fps_for_file(const char *filepath) {
    if (filepath == nullptr) {
        return 12;
    }
    if (strstr(filepath, "idle.mjpeg") != nullptr) {
        return 8;
    }
    if (strstr(filepath, "loading.mjpeg") != nullptr) {
        return 10;
    }
    if (strstr(filepath, "listen.mjpeg") != nullptr || strstr(filepath, "talk.mjpeg") != nullptr) {
        return 12;
    }
    return 12;
}

static esp_err_t safe_stop_player(uint32_t timeout_ms) {
    (void)timeout_ms;
    player_state_t current_state = get_player_state();
    if (current_state == PLAYER_STATE_IDLE) {
        return ESP_OK;
    }

    if (!set_player_state(PLAYER_STATE_STOPPING)) {
        return ESP_FAIL;
    }

    esp_err_t ret = mjpeg_player_stop(s_player.handle);
    if (ret == ESP_OK) {
        set_player_state(PLAYER_STATE_IDLE);
    }
    return ret;
}

static void player_manager_task(void *arg) {
    player_task_t task;
    (void)arg;

    ESP_LOGI(TAG, "MJPEG manager task started");

    while (!s_player.shutdown_requested) {
        if (xQueueReceive(s_player.task_queue, &task, pdMS_TO_TICKS(500)) == pdTRUE) {
            switch (task.type) {
                case PLAYER_TASK_PLAY: {
                    ESP_LOGI(TAG, "Play: %s", task.filepath);

                    player_state_t current_state = get_player_state();
                    if (current_state != PLAYER_STATE_IDLE) {
                        set_player_state(PLAYER_STATE_SWITCHING);
                        safe_stop_player(1000);
                    }

                    // Get filesystem type and mount path (same as old project)
                    fs_type_t fs_type = fs_manager_get_type();
                    const char *mount_path = (fs_type == FS_TYPE_SD_CARD) ? "/sdcard" : "/spiffs";

                    // Build full path (same logic as old project)
                    char full_path[300] = {0};
                    size_t path_len = 0;

                    if (strncmp(task.filepath, "/sdcard/", 8) == 0 ||
                        strncmp(task.filepath, "/spiffs/", 8) == 0) {
                        // Absolute path with known prefix
                        path_len = strlen(task.filepath);
                        if (path_len < sizeof(full_path)) {
                            memcpy(full_path, task.filepath, path_len);
                        }
                    } else if (task.filepath[0] == '/') {
                        // Absolute path without prefix - prepend mount_path
                        path_len = strlen(mount_path) + strlen(task.filepath);
                        if (path_len < sizeof(full_path)) {
                            memcpy(full_path, mount_path, strlen(mount_path));
                            memcpy(full_path + strlen(mount_path), task.filepath, strlen(task.filepath));
                        }
                    } else {
                        // Relative path - prepend mount_path
                        path_len = strlen(mount_path) + 1 + strlen(task.filepath);
                        if (path_len < sizeof(full_path)) {
                            memcpy(full_path, mount_path, strlen(mount_path));
                            full_path[strlen(mount_path)] = '/';
                            memcpy(full_path + strlen(mount_path) + 1, task.filepath, strlen(task.filepath));
                        }
                    }
                    full_path[path_len] = '\0';

                    if (strcmp(s_player.current_file, full_path) == 0 &&
                        get_player_state() == PLAYER_STATE_PLAYING) {
                        ESP_LOGI(TAG, "Already playing: %s", full_path);
                        break;
                    }

                    uint32_t target_fps = target_fps_for_file(full_path);
                    mjpeg_player_set_target_fps(s_player.handle, target_fps);
                    ESP_LOGI(TAG, "Target FPS: %u", (unsigned)target_fps);

                    esp_err_t ret = mjpeg_player_play_file(s_player.handle, full_path);
                    if (ret == ESP_OK) {
                        set_player_state(PLAYER_STATE_PLAYING);
                        size_t copy_len = strlen(full_path);
                        if (copy_len >= sizeof(s_player.current_file)) {
                            copy_len = sizeof(s_player.current_file) - 1;
                        }
                        memcpy(s_player.current_file, full_path, copy_len);
                        s_player.current_file[copy_len] = '\0';
                        ESP_LOGI(TAG, "Playing: %s", full_path);
                    } else {
                        set_player_state(PLAYER_STATE_IDLE);
                        ESP_LOGE(TAG, "Play failed: %s error=%d", full_path, ret);
                    }
                    break;
                }

                case PLAYER_TASK_STOP: {
                    ESP_LOGI(TAG, "Stop");
                    safe_stop_player(1000);
                    memset(s_player.current_file, 0, sizeof(s_player.current_file));
                    break;
                }

                case PLAYER_TASK_SET_LOOP: {
                    s_player.loop_enabled = task.loop_mode;
                    mjpeg_player_set_loop(s_player.handle, task.loop_mode);
                    ESP_LOGI(TAG, "Loop: %s", task.loop_mode ? "on" : "off");
                    break;
                }
            }
        }
    }

    ESP_LOGI(TAG, "MJPEG manager task shutting down");
    vTaskDelete(NULL);
}

esp_err_t mjpeg_player_port_init(mjpeg_player_port_config_t *config) {
    if (s_player.handle != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_player.state_mutex = xSemaphoreCreateMutex();
    if (s_player.state_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_IDF_TARGET_ESP32P4
    fs_config_t sdcard_config = {
        .type = FS_TYPE_SD_CARD,
        .sd_card = {
            .mount_point = "/sdcard",
            .clk = GPIO_NUM_43,
            .cmd = GPIO_NUM_44,
            .d0 = GPIO_NUM_39,
            .d1 = GPIO_NUM_40,
            .d2 = GPIO_NUM_41,
            .d3 = GPIO_NUM_42,
            .format_if_mount_failed = false,
            .max_files = 5
        }
    };
#else
    fs_config_t sdcard_config = {
        .type = FS_TYPE_SD_CARD,
        .sd_card = {
            .mount_point = "/sdcard",
            .clk = GPIO_NUM_2,
            .cmd = GPIO_NUM_42,
            .d0 = GPIO_NUM_1,
            .d1 = GPIO_NUM_NC,
            .d2 = GPIO_NUM_NC,
            .d3 = GPIO_NUM_NC,
            .format_if_mount_failed = false,
            .max_files = 5
        }
    };
#endif

    fs_config_t spiffs_config = {
        .type = FS_TYPE_SPIFFS,
        .spiffs = {
            .base_path = "/spiffs",
            .partition_label = "storage",
            .max_files = 5,
            .format_if_mount_failed = true
        }
    };

    // Auto-init: try SD card first, fallback to SPIFFS (same as old project)
    esp_err_t ret = fs_manager_auto_init(&sdcard_config, &spiffs_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Filesystem init failed: %d", ret);
        vSemaphoreDelete(s_player.state_mutex);
        s_player.state_mutex = NULL;
        return ret;
    }

    // List files on mount point for debugging
    fs_type_t fs_type = fs_manager_get_type();
    const char *mount_path = (fs_type == FS_TYPE_SD_CARD) ? "/sdcard" : "/spiffs";
    ESP_LOGI(TAG, "Using filesystem: %s", mount_path);
    fs_manager_list_files(mount_path);

    mjpeg_player_config_t player_config = {
        .frame_buffer_size = config->buffer_size ? config->buffer_size : 150 * 1024,
        .cache_buffer_size = config->buffer_size ? config->buffer_size : 128 * 1024,
        .cache_in_psram = config->use_psram,
        .preload_to_psram = config->use_psram,
        .task_priority = config->task_priority,
        .task_core = config->core_id,
        .target_fps = config->target_fps > 0 ? config->target_fps : 30,
        .acquire_rgb_buffer = player_acquire_rgb_buffer,
        .present_rgb_buffer = player_present_rgb_buffer,
        .buffer_ctx = NULL,
        .on_frame_cb = player_frame_callback,
        .user_data = NULL
    };

    jpeg_decoder_warmup();

    // Create player first with a temporary callback, then set the real one
    ret = mjpeg_player_create(&player_config, &s_player.handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MJPEG player: %d", ret);
        vSemaphoreDelete(s_player.state_mutex);
        s_player.state_mutex = NULL;
        return ret;
    }

    s_player.task_queue = xQueueCreate(5, sizeof(player_task_t));
    if (s_player.task_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create task queue");
        mjpeg_player_destroy(s_player.handle);
        s_player.handle = NULL;
        vSemaphoreDelete(s_player.state_mutex);
        s_player.state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreatePinnedToCore(
        player_manager_task, "mjpeg_mgr",
        16 * 1024, NULL,
        config->task_priority, &s_player.manager_task,
        config->core_id >= 0 ? config->core_id : 1);

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create manager task");
        vQueueDelete(s_player.task_queue);
        s_player.task_queue = NULL;
        mjpeg_player_destroy(s_player.handle);
        s_player.handle = NULL;
        vSemaphoreDelete(s_player.state_mutex);
        s_player.state_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    mjpeg_player_set_loop(s_player.handle, true);
    ESP_LOGI(TAG, "MJPEG player port initialized (fs: %s)", mount_path);
    return ESP_OK;
}

esp_err_t mjpeg_player_port_play_file(const char *filepath) {
    if (s_player.handle == NULL || s_player.task_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_player.shutdown_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    player_task_t task = {.type = PLAYER_TASK_PLAY};
    size_t path_len = strlen(filepath);
    if (path_len >= sizeof(task.filepath)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(task.filepath, filepath, path_len);
    task.filepath[path_len] = '\0';

    // Clear stale tasks from queue
    player_task_t old;
    while (xQueueReceive(s_player.task_queue, &old, pdMS_TO_TICKS(10)) == pdTRUE) {}

    if (xQueueSend(s_player.task_queue, &task, pdMS_TO_TICKS(200)) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mjpeg_player_port_stop(void) {
    if (s_player.handle == NULL || s_player.task_queue == NULL) {
        return ESP_OK;
    }

    player_task_t task = {.type = PLAYER_TASK_STOP};
    if (xQueueSend(s_player.task_queue, &task, 0) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t mjpeg_player_port_stop_wait(uint32_t timeout_ms) {
    if (s_player.handle == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = safe_stop_player(timeout_ms);
    memset(s_player.current_file, 0, sizeof(s_player.current_file));
    return ret;
}

void mjpeg_player_port_set_loop(bool enable) {
    if (s_player.handle == NULL || s_player.task_queue == NULL) {
        return;
    }

    player_task_t task = {.type = PLAYER_TASK_SET_LOOP, .loop_mode = enable};
    xQueueSend(s_player.task_queue, &task, pdMS_TO_TICKS(1000));
}

void mjpeg_player_port_deinit(void) {
    if (s_player.handle == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Deinitializing MJPEG player port...");
    s_player.shutdown_requested = true;

    player_task_t stop_task = {.type = PLAYER_TASK_STOP};
    xQueueSend(s_player.task_queue, &stop_task, pdMS_TO_TICKS(500));

    if (s_player.manager_task != NULL) {
        uint32_t timeout = 2000;
        while (eTaskGetState(s_player.manager_task) != eDeleted && timeout-- > 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (timeout == 0) {
            ESP_LOGW(TAG, "Force deleting manager task");
            vTaskDelete(s_player.manager_task);
        }
        s_player.manager_task = NULL;
    }

    if (s_player.task_queue != NULL) {
        vQueueDelete(s_player.task_queue);
        s_player.task_queue = NULL;
    }

    if (s_player.handle != NULL) {
        mjpeg_player_destroy(s_player.handle);
        s_player.handle = NULL;
    }

    if (s_player.state_mutex != NULL) {
        vSemaphoreDelete(s_player.state_mutex);
        s_player.state_mutex = NULL;
    }

    memset(&s_player, 0, sizeof(s_player));
    ESP_LOGI(TAG, "MJPEG player port deinitialized");
}
