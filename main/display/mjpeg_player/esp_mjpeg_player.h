#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* mjpeg_player_handle_t;

/**
 * @brief MJPEG player configuration
 */
typedef struct {
    size_t frame_buffer_size;     /*!< Frame buffer size for JPEG data */
    size_t cache_buffer_size;     /*!< File read cache size */
    bool cache_in_psram;         /*!< Use PSRAM for cache */
    int task_priority;           /*!< Task priority */
    int task_core;              /*!< CPU core ID */
    uint32_t target_fps;        /*!< Target playback FPS (0 = unlimited, recommended: 15-20) */
    void (*on_frame_cb)(uint8_t *rgb565, uint32_t width, uint32_t height, void *ctx); /*!< Frame callback */
    void *user_data;            /*!< User data passed to callback */
} mjpeg_player_config_t;

/**
 * @brief Create MJPEG player instance
 */
esp_err_t mjpeg_player_create(const mjpeg_player_config_t *config, mjpeg_player_handle_t *handle);

/**
 * @brief Start playing MJPEG file
 */
esp_err_t mjpeg_player_play_file(mjpeg_player_handle_t handle, const char *filepath);

/**
 * @brief Stop playback
 */
esp_err_t mjpeg_player_stop(mjpeg_player_handle_t handle);

/**
 * @brief Enable/disable loop playback
 */
esp_err_t mjpeg_player_set_loop(mjpeg_player_handle_t handle, bool enable);
esp_err_t mjpeg_player_set_target_fps(mjpeg_player_handle_t handle, uint32_t fps);

/**
 * @brief Destroy player instance
 */
esp_err_t mjpeg_player_destroy(mjpeg_player_handle_t handle);

#ifdef __cplusplus
}
#endif
