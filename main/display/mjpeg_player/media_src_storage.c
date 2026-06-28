#include "media_src_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "media_src_storage";

#define CACHE_SIZE (32 * 1024)

typedef struct {
    uint8_t *align_buffer;
    int readed;
    int filled;
    bool eof;
    int seek_pos;
    int align_pos;
    int buffer_pos;
    FILE *fp;
} storage_src_t;

static void media_src_storage_flush(storage_src_t *m) {
    m->filled = m->readed = 0;
    m->eof = false;
    m->align_pos = m->seek_pos = 0;
    m->buffer_pos = 0;
}

static int cache_data(storage_src_t *m, void *data, int n) {
    int sent = 0;
    if (m->filled > m->readed) {
        sent = m->filled - m->readed;
        if (m->align_pos < m->seek_pos) {
            int need_skip = m->seek_pos - m->align_pos;
            if (need_skip > sent) {
                m->readed += sent;
                sent = 0;
                m->align_pos += sent;
            } else {
                sent -= need_skip;
                m->readed += need_skip;
                m->align_pos += need_skip;
            }
        }
        if (sent > n) {
            sent = n;
        }
        if (sent) {
            memcpy(data, m->align_buffer + m->readed, sent);
            m->readed += sent;
        }
    }
    if (m->readed >= m->filled) {
        m->buffer_pos += m->filled;
        m->readed = m->filled = 0;
    }
    if (m->filled == 0) {
        int nread = read(fileno(m->fp), m->align_buffer, CACHE_SIZE);
        if (nread < 0) {
            return nread;
        }
        if (nread < CACHE_SIZE) {
            m->eof = true;
        }
        m->filled = nread;
    }
    return sent;
}

static int read_from_cache(storage_src_t *m, void *data, size_t len) {
    int read_bytes = 0;
    while (len > 0) {
        if (m->eof && m->filled == 0) {
            break;
        }
        int n = cache_data(m, data, len);
        if (n < 0) {
            ESP_LOGE(TAG, "Fail to read from cache");
            return n;
        }
        data = (uint8_t *)data + n;
        len -= n;
        read_bytes += n;
    }
    return read_bytes;
}

int media_src_storage_open(media_src_t *src) {
    storage_src_t *m = calloc(1, sizeof(storage_src_t));
    if (m == NULL) {
        return -1;
    }
    m->align_buffer = heap_caps_aligned_alloc(128, CACHE_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (m->align_buffer == NULL) {
        ESP_LOGE(TAG, "No memory");
        free(m);
        return -1;
    }
    src->sub_src = m;
    return 0;
}

int media_src_storage_connect(media_src_t *src, const char *uri) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose(m->fp);
        m->fp = NULL;
    }
    media_src_storage_flush(m);

    ESP_LOGI(TAG, "Open file %s", uri);
    m->fp = fopen(uri, "rb");
    if (m->fp) {
        return 0;
    }

    ESP_LOGE(TAG, "Fail to open file: %s", uri);
    return -1;
}

int media_src_storage_disconnect(media_src_t *src) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose(m->fp);
        m->fp = NULL;
    }
    media_src_storage_flush(m);
    return 0;
}

int media_src_storage_read(media_src_t *src, void *data, size_t len) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        return read_from_cache(m, data, len);
    }
    ESP_LOGE(TAG, "Fail to read file");
    return -1;
}

int media_src_storage_seek(media_src_t *src, uint64_t position) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        if (m->filled && position >= m->buffer_pos && position <= m->buffer_pos + m->filled) {
            m->readed = (int)(position - m->buffer_pos);
            return 0;
        }
        media_src_storage_flush(m);
        int align_pos = (int)(position & (~1023));
        m->align_pos = align_pos;
        m->seek_pos = (int)position;
        m->buffer_pos = align_pos;
        return fseek(m->fp, align_pos, SEEK_SET);
    }
    ESP_LOGE(TAG, "Fail to seek file");
    return -1;
}

int media_src_storage_get_position(media_src_t *src, uint64_t *position) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        *position = (uint64_t)ftell(m->fp);
        return 0;
    }
    return -1;
}

int media_src_storage_get_size(media_src_t *src, uint64_t *size) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        long old = ftell(m->fp);
        fseek(m->fp, 0, SEEK_END);
        long end = ftell(m->fp);
        fseek(m->fp, old, SEEK_SET);
        *size = (end <= 0 ? 0 : (uint64_t)end);
        return 0;
    }
    return -1;
}

int media_src_storage_close(media_src_t *src) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose((FILE *)m->fp);
        m->fp = NULL;
    }
    if (m->align_buffer) {
        free(m->align_buffer);
    }
    free(m);
    return 0;
}
