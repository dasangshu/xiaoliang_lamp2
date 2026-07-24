#include "media_src_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/unistd.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "media_src_storage";

#define CACHE_SIZE (32 * 1024)
#define PRELOAD_PSRAM_RESERVE (512 * 1024)

typedef struct {
    uint8_t *align_buffer;
    uint8_t *preload_data;
    size_t preload_size;
    size_t preload_pos;
    bool preloaded;
    bool preload_enabled;
    int readed;
    int filled;
    bool eof;
    int seek_pos;
    int align_pos;
    int buffer_pos;
    FILE *fp;
} storage_src_t;

static void media_src_storage_free_preload(storage_src_t *m) {
    if (m->preload_data) {
        heap_caps_free(m->preload_data);
        m->preload_data = NULL;
    }
    m->preload_size = 0;
    m->preload_pos = 0;
    m->preloaded = false;
}

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

int media_src_storage_set_preload(media_src_t *src, bool enable) {
    if (!src || !src->sub_src) {
        return -1;
    }
    storage_src_t *m = (storage_src_t *)src->sub_src;
    m->preload_enabled = enable;
    return 0;
}

static int media_src_storage_try_preload(media_src_t *src, const char *uri) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (!m->preload_enabled || m->fp == NULL) {
        return -1;
    }

    uint64_t file_size_u64 = 0;
    if (media_src_storage_get_size(src, &file_size_u64) != 0 || file_size_u64 == 0) {
        return -1;
    }

    size_t file_size = (size_t)file_size_u64;
    if ((uint64_t)file_size != file_size_u64) {
        ESP_LOGW(TAG, "File too large for PSRAM preload: %" PRIu64, file_size_u64);
        return -1;
    }

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (file_size + PRELOAD_PSRAM_RESERVE > free_psram) {
        ESP_LOGW(TAG, "Skip preload (%s): need %u bytes, free PSRAM=%u",
                 uri, (unsigned)file_size, (unsigned)free_psram);
        return -1;
    }

    uint8_t *preload_buf = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!preload_buf) {
        ESP_LOGW(TAG, "Failed to allocate %u bytes PSRAM for preload", (unsigned)file_size);
        return -1;
    }

    ESP_LOGI(TAG, "Preloading %s to PSRAM (%u bytes)...", uri, (unsigned)file_size);
    size_t total_read = 0;
    while (total_read < file_size) {
        size_t chunk = file_size - total_read;
        if (chunk > CACHE_SIZE) {
            chunk = CACHE_SIZE;
        }
        size_t n = fread(preload_buf + total_read, 1, chunk, m->fp);
        if (n == 0) {
            ESP_LOGE(TAG, "Preload read failed at %u/%u", (unsigned)total_read, (unsigned)file_size);
            heap_caps_free(preload_buf);
            fseek(m->fp, 0, SEEK_SET);
            return -1;
        }
        total_read += n;
    }

    fclose(m->fp);
    m->fp = NULL;
    media_src_storage_flush(m);

    m->preload_data = preload_buf;
    m->preload_size = file_size;
    m->preload_pos = 0;
    m->preloaded = true;
    ESP_LOGI(TAG, "Preloaded to PSRAM: %s (%u bytes)", uri, (unsigned)file_size);
    return 0;
}

int media_src_storage_connect(media_src_t *src, const char *uri) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose(m->fp);
        m->fp = NULL;
    }
    media_src_storage_free_preload(m);
    media_src_storage_flush(m);

    ESP_LOGI(TAG, "Open file %s", uri);
    m->fp = fopen(uri, "rb");
    if (!m->fp) {
        ESP_LOGE(TAG, "Fail to open file: %s", uri);
        return -1;
    }

    if (media_src_storage_try_preload(src, uri) == 0) {
        return 0;
    }

    return 0;
}

int media_src_storage_disconnect(media_src_t *src) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose(m->fp);
        m->fp = NULL;
    }
    media_src_storage_free_preload(m);
    media_src_storage_flush(m);
    return 0;
}

int media_src_storage_read(media_src_t *src, void *data, size_t len) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->preloaded) {
        if (m->preload_pos >= m->preload_size) {
            return 0;
        }
        size_t avail = m->preload_size - m->preload_pos;
        if (len > avail) {
            len = avail;
        }
        memcpy(data, m->preload_data + m->preload_pos, len);
        m->preload_pos += len;
        return (int)len;
    }
    if (m->fp) {
        return read_from_cache(m, data, len);
    }
    ESP_LOGE(TAG, "Fail to read file");
    return -1;
}

int media_src_storage_seek(media_src_t *src, uint64_t position) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->preloaded) {
        if (position > m->preload_size) {
            return -1;
        }
        m->preload_pos = (size_t)position;
        return 0;
    }
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
    if (m->preloaded) {
        *position = (uint64_t)m->preload_pos;
        return 0;
    }
    if (m->fp) {
        *position = (uint64_t)ftell(m->fp);
        return 0;
    }
    return -1;
}

int media_src_storage_get_size(media_src_t *src, uint64_t *size) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->preloaded) {
        *size = (uint64_t)m->preload_size;
        return 0;
    }
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

int media_src_storage_is_preloaded(media_src_t *src) {
    if (!src || !src->sub_src) {
        return 0;
    }
    storage_src_t *m = (storage_src_t *)src->sub_src;
    return m->preloaded ? 1 : 0;
}

int media_src_storage_get_preload_view(media_src_t *src, const uint8_t **data, size_t *size, size_t *pos) {
    if (!src || !src->sub_src || !data || !size || !pos) {
        return -1;
    }
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (!m->preloaded) {
        return -1;
    }
    *data = m->preload_data;
    *size = m->preload_size;
    *pos = m->preload_pos;
    return 0;
}

int media_src_storage_set_preload_pos(media_src_t *src, size_t pos) {
    if (!src || !src->sub_src) {
        return -1;
    }
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (!m->preloaded || pos > m->preload_size) {
        return -1;
    }
    m->preload_pos = pos;
    return 0;
}

int media_src_storage_close(media_src_t *src) {
    storage_src_t *m = (storage_src_t *)src->sub_src;
    if (m->fp) {
        fclose((FILE *)m->fp);
        m->fp = NULL;
    }
    media_src_storage_free_preload(m);
    if (m->align_buffer) {
        free(m->align_buffer);
    }
    free(m);
    return 0;
}
