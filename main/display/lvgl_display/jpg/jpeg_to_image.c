#include <esp_check.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <sys/param.h>
#include <string.h>

#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"

#include "jpeg_to_image.h"

#ifdef CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL MAX(CONFIG_LOG_DEFAULT_LEVEL, ESP_LOG_DEBUG)
#endif  // CONFIG_XIAOZHI_ENABLE_CAMERA_DEBUG_MODE
#include <esp_log.h>

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
#include "driver/jpeg_decode.h"
#endif

#define TAG "jpeg_to_image"

static jpeg_dec_handle_t s_sw_jpeg_dec = NULL;
static bool s_sw_jpeg_disabled = false;

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static jpeg_decoder_handle_t s_hw_jpeg_dec = NULL;
static bool s_hw_jpeg_disabled = false;
#endif

static bool sw_jpeg_ensure_open(void) {
    if (s_sw_jpeg_disabled) {
        return false;
    }
    if (s_sw_jpeg_dec) {
        return true;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    if (jpeg_dec_open(&config, &s_sw_jpeg_dec) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open software JPEG decoder");
        s_sw_jpeg_disabled = true;
        s_sw_jpeg_dec = NULL;
        return false;
    }
    return true;
}

static esp_err_t decode_with_new_jpeg_into(const uint8_t* src, size_t src_len, uint8_t* out_buf, size_t out_buf_size,
                                           size_t* out_len, size_t* width, size_t* height, size_t* stride) {
    if (!sw_jpeg_ensure_open()) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};
    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;

    jpeg_error_t jpeg_ret = jpeg_dec_parse_header(s_sw_jpeg_dec, &jpeg_io, &out_info);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to parse JPEG header");
        return ESP_ERR_INVALID_ARG;
    }

    size_t needed = (size_t)out_info.width * (size_t)out_info.height * 2;
    if (out_buf == NULL || out_buf_size < needed) {
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_io.outbuf = out_buf;
    jpeg_ret = jpeg_dec_process(s_sw_jpeg_dec, &jpeg_io);
    if (jpeg_ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "Failed to decode JPEG");
        return ESP_FAIL;
    }

    *out_len = needed;
    *width = (size_t)out_info.width;
    *height = (size_t)out_info.height;
    *stride = (size_t)out_info.width * 2;
    return ESP_OK;
}

static esp_err_t decode_with_new_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                                      size_t* height, size_t* stride) {
    if (!sw_jpeg_ensure_open()) {
        return ESP_FAIL;
    }

    jpeg_dec_io_t jpeg_io = {0};
    jpeg_dec_header_info_t out_info = {0};
    jpeg_io.inbuf = (uint8_t*)src;
    jpeg_io.inbuf_len = (int)src_len;

    if (jpeg_dec_parse_header(s_sw_jpeg_dec, &jpeg_io, &out_info) != JPEG_ERR_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t needed = (size_t)out_info.width * (size_t)out_info.height * 2;
    uint8_t* decode_buf = jpeg_calloc_align(needed, 16);
    if (decode_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = decode_with_new_jpeg_into(src, src_len, decode_buf, needed, out_len, width, height, stride);
    if (ret != ESP_OK) {
        jpeg_free_align(decode_buf);
        *out = NULL;
        return ret;
    }

    *out = decode_buf;
    return ESP_OK;
}

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
static bool hw_jpeg_ensure_open(void) {
    if (s_hw_jpeg_disabled) {
        return false;
    }
    if (s_hw_jpeg_dec) {
        return true;
    }

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (largest < 512) {
        ESP_LOGW(TAG, "insufficient internal DMA memory for HW JPEG decoder");
        s_hw_jpeg_disabled = true;
        return false;
    }

    jpeg_decode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    if (jpeg_new_decoder_engine(&eng_cfg, &s_hw_jpeg_dec) != ESP_OK) {
        ESP_LOGW(TAG, "HW JPEG decoder init failed");
        s_hw_jpeg_dec = NULL;
        s_hw_jpeg_disabled = true;
        return false;
    }
    ESP_LOGI(TAG, "HW JPEG decoder ready (intr_priority=0)");
    return true;
}

static esp_err_t decode_with_hardware_jpeg_into(const uint8_t* src, size_t src_len, uint8_t* out_buf, size_t out_buf_size,
                                                size_t* out_len, size_t* width, size_t* height, size_t* stride,
                                                bool allow_alloc, uint8_t** out_owned) {
    if (!hw_jpeg_ensure_open()) {
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    uint8_t* bit_stream = NULL;
    uint8_t* decode_buf = NULL;
    size_t decode_buf_len = 0;
    size_t tx_buffer_size = 0;
    size_t rx_buffer_size = 0;

    jpeg_decode_memory_alloc_cfg_t tx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
    };
    jpeg_decode_memory_alloc_cfg_t rx_mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };

    bit_stream = (uint8_t*)jpeg_alloc_decoder_mem(src_len, &tx_mem_cfg, &tx_buffer_size);
    if (bit_stream == NULL || tx_buffer_size < src_len) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(bit_stream, src, src_len);

    jpeg_decode_picture_info_t header_info;
    ret = jpeg_decoder_get_info(bit_stream, src_len, &header_info);
    if (ret != ESP_OK) {
        heap_caps_free(bit_stream);
        return ret;
    }

    jpeg_decode_cfg_t decode_cfg_rgb = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
    };

    switch (header_info.sample_method) {
        case JPEG_DOWN_SAMPLING_GRAY:
        case JPEG_DOWN_SAMPLING_YUV444:
            decode_buf_len = header_info.width * header_info.height * 2;
            *stride = header_info.width * 2;
            break;
        case JPEG_DOWN_SAMPLING_YUV422:
        case JPEG_DOWN_SAMPLING_YUV420:
            decode_buf_len = ((header_info.width + 15) & ~15) * ((header_info.height + 15) & ~15) * 2;
            *stride = ((header_info.width + 15) & ~15) * 2;
            break;
        default:
            heap_caps_free(bit_stream);
            return ESP_ERR_NOT_SUPPORTED;
    }

    decode_buf = (uint8_t*)jpeg_alloc_decoder_mem(decode_buf_len, &rx_mem_cfg, &rx_buffer_size);
    if (decode_buf == NULL || rx_buffer_size < decode_buf_len) {
        heap_caps_free(bit_stream);
        return ESP_ERR_NO_MEM;
    }

    uint32_t decoded_size = 0;
    ret = jpeg_decoder_process(s_hw_jpeg_dec, &decode_cfg_rgb, bit_stream, src_len, decode_buf, decode_buf_len,
                               &decoded_size);
    heap_caps_free(bit_stream);
    if (ret != ESP_OK || decoded_size != decode_buf_len) {
        heap_caps_free(decode_buf);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
    }

    size_t visible_len = (size_t)header_info.width * (size_t)header_info.height * 2;
    if (header_info.sample_method == JPEG_DOWN_SAMPLING_GRAY) {
        uint32_t i = header_info.width * header_info.height;
        do {
            --i;
            uint8_t r = (decode_buf[i] >> 3) & 0x1F;
            uint8_t g = (decode_buf[i] >> 2) & 0x3F;
            uint16_t rgb565 = (r << 11) | (g << 5) | r;
            decode_buf[2 * i + 1] = (rgb565 >> 8) & 0xFF;
            decode_buf[2 * i] = rgb565 & 0xFF;
        } while (i != 0);
        visible_len = (size_t)header_info.width * (size_t)header_info.height * 2;
    }

    *width = header_info.width;
    *height = header_info.height;
    *out_len = visible_len;

    if (out_buf != NULL) {
        if (out_buf_size < visible_len) {
            heap_caps_free(decode_buf);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out_buf, decode_buf, visible_len);
        heap_caps_free(decode_buf);
        if (out_owned) {
            *out_owned = NULL;
        }
        return ESP_OK;
    }

    if (!allow_alloc) {
        heap_caps_free(decode_buf);
        return ESP_ERR_INVALID_ARG;
    }

    if (out_owned) {
        *out_owned = decode_buf;
    }
    return ESP_OK;
}

static esp_err_t decode_with_hardware_jpeg(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len,
                                           size_t* width, size_t* height, size_t* stride) {
    return decode_with_hardware_jpeg_into(src, src_len, NULL, 0, out_len, width, height, stride, true, out);
}
#endif

void jpeg_decoder_warmup(void) {
#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    if (hw_jpeg_ensure_open()) {
        ESP_LOGI(TAG, "HW JPEG decoder ready");
        return;
    }
#endif
    if (sw_jpeg_ensure_open()) {
        ESP_LOGI(TAG, "SW JPEG decoder ready");
    }
}

esp_err_t jpeg_to_image_into(const uint8_t* src, size_t src_len, uint8_t* out_buf, size_t out_buf_size,
                             size_t* out_len, size_t* width, size_t* height, size_t* stride) {
    if (src == NULL || src_len == 0 || out_buf == NULL || out_buf_size == 0 || out_len == NULL || width == NULL ||
        height == NULL || stride == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg_into(src, src_len, out_buf, out_buf_size, out_len, width, height, stride,
                                                   false, NULL);
    if (ret == ESP_OK) {
        return ret;
    }
#endif
    return decode_with_new_jpeg_into(src, src_len, out_buf, out_buf_size, out_len, width, height, stride);
}

esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                        size_t* height, size_t* stride) {
    if (src == NULL || src_len == 0 || out == NULL || out_len == NULL || width == NULL || height == NULL ||
        stride == NULL) {
        ESP_LOGE(TAG, "Invalid parameters");
        return ESP_ERR_INVALID_ARG;
    }

#ifdef CONFIG_XIAOZHI_ENABLE_HARDWARE_JPEG_DECODER
    esp_err_t ret = decode_with_hardware_jpeg(src, src_len, out, out_len, width, height, stride);
    if (ret == ESP_OK) {
        return ret;
    }
    ESP_LOGW(TAG, "HW JPEG decode failed, fallback to SW");
#endif
    return decode_with_new_jpeg(src, src_len, out, out_len, width, height, stride);
}
