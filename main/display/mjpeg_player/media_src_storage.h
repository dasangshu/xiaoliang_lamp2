#ifndef MEDIA_SRC_STORAGE_H
#define MEDIA_SRC_STORAGE_H

#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *sub_src;
} media_src_t;

int media_src_storage_open(media_src_t *src);
int media_src_storage_set_preload(media_src_t *src, bool enable);
int media_src_storage_connect(media_src_t *src, const char *uri);
int media_src_storage_disconnect(media_src_t *src);
int media_src_storage_read(media_src_t *src, void *data, size_t len);
int media_src_storage_seek(media_src_t *src, uint64_t position);
int media_src_storage_get_position(media_src_t *src, uint64_t *position);
int media_src_storage_get_size(media_src_t *src, uint64_t *size);
int media_src_storage_is_preloaded(media_src_t *src);
int media_src_storage_get_preload_view(media_src_t *src, const uint8_t **data, size_t *size, size_t *pos);
int media_src_storage_set_preload_pos(media_src_t *src, size_t pos);
int media_src_storage_close(media_src_t *src);

#ifdef __cplusplus
}
#endif

#endif
