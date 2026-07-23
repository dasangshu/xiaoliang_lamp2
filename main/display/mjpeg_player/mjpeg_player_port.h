#ifndef MJPEG_PLAYER_PORT_H
#define MJPEG_PLAYER_PORT_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t buffer_size;
    int core_id;
    bool use_psram;
    int task_priority;
    uint32_t target_fps;  /*!< Target playback FPS (0 = use default 15fps) */
} mjpeg_player_port_config_t;

esp_err_t mjpeg_player_port_init(mjpeg_player_port_config_t *config);
esp_err_t mjpeg_player_port_play_file(const char *filepath);
esp_err_t mjpeg_player_port_stop(void);
esp_err_t mjpeg_player_port_stop_wait(uint32_t timeout_ms);
void mjpeg_player_port_set_loop(bool enable);
void mjpeg_player_port_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
