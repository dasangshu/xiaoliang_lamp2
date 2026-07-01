#include "coco_pose.hpp"
#include "esp_log.h"
#include <stdexcept>

#if CONFIG_COCO_POSE_MODEL_IN_FLASH_RODATA
extern const uint8_t coco_pose_espdl[] asm("_binary_coco_pose_espdl_start");
static const char *path = (const char *)coco_pose_espdl;
#elif CONFIG_COCO_POSE_MODEL_IN_FLASH_PARTITION
static const char *path = "coco_pose";
#else
#if !defined(CONFIG_BSP_SD_MOUNT_POINT)
#define CONFIG_BSP_SD_MOUNT_POINT "/sdcard"
#endif
#endif
namespace coco_pose {
Yolo11nPose::Yolo11nPose(const char *model_name, const char *full_path)
{
    ESP_LOGI("coco_pose", "Yolo11nPose constructor called with model_name=%s", model_name);

#if !CONFIG_COCO_POSE_MODEL_IN_SDCARD
    ESP_LOGI("coco_pose", "Loading model from flash...");

    // 🔥 FIX: 强制禁用 param_copy 以避免将模型数据复制到 PSRAM
    // 复制 3MB 数据到 PSRAM 会导致 30-60 秒的延迟或超时
    // 直接从 Flash 读取虽然稍慢，但可接受且启动速度快
    bool param_copy = false;

    ESP_LOGI("coco_pose", "Creating dl::Model instance (param_copy=%s)...", param_copy ? "true" : "false");
    m_model = new dl::Model(path,
                            model_name,
                            static_cast<fbs::model_location_type_t>(CONFIG_COCO_POSE_MODEL_LOCATION),
                            0,
                            dl::MEMORY_MANAGER_GREEDY,
                            nullptr,
                            param_copy);
    ESP_LOGI("coco_pose", "dl::Model instance created at %p", (void*)m_model);
#else
    ESP_LOGI("coco_pose", "Loading model from SD card...");
    char sd_path[256];
    if (full_path) {
        snprintf(sd_path, sizeof(sd_path), "%s", full_path);
    } else {
        snprintf(sd_path, sizeof(sd_path), "%s/%s/%s",
                 CONFIG_BSP_SD_MOUNT_POINT, CONFIG_COCO_POSE_MODEL_SDCARD_DIR, model_name);
    }
    ESP_LOGI("coco_pose", "Model path: %s", sd_path);
    m_model = new dl::Model(sd_path, static_cast<fbs::model_location_type_t>(CONFIG_COCO_POSE_MODEL_LOCATION));
    ESP_LOGI("coco_pose", "dl::Model instance created at %p", (void*)m_model);
    if (!m_model || !m_model->get_fbs_model()) {
        ESP_LOGE("coco_pose", "Model load failed: %s", sd_path);
        if (m_model) { delete m_model; m_model = nullptr; }
        throw std::runtime_error("coco_pose: model load failed");
    }
#endif

    ESP_LOGI("coco_pose", "Calling m_model->minimize()...");
    m_model->minimize();
    ESP_LOGI("coco_pose", "m_model->minimize() completed");

    if (m_model->get_inputs().size() != 1) {
        ESP_LOGE("coco_pose", "Model is not ready: expected 1 input, got %u",
                 static_cast<unsigned>(m_model->get_inputs().size()));
        delete m_model;
        m_model = nullptr;
        throw std::runtime_error("coco_pose: invalid model inputs");
    }

#if CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI("coco_pose", "Creating ImagePreprocessor for ESP32P4...");
    m_image_preprocessor =
        new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255}, DL_IMAGE_CAP_RGB565_BIG_ENDIAN);
#else
    ESP_LOGI("coco_pose", "Creating ImagePreprocessor...");
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
#endif
    ESP_LOGI("coco_pose", "ImagePreprocessor created");

    ESP_LOGI("coco_pose", "Creating PostProcessor...");
    m_postprocessor = new dl::detect::yolo11posePostProcessor(
        m_model, 0.3, 0.45, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
    ESP_LOGI("coco_pose", "PostProcessor created, Yolo11nPose construction complete");
}

} // namespace coco_pose

COCOPose::COCOPose(model_type_t model_type)
{
    ESP_LOGI("coco_pose", "COCOPose constructor called, model_type=%d", model_type);

    switch (model_type) {
    case model_type_t::YOLO11N_POSE_S8_V1:
#if CONFIG_COCO_POSE_YOLO11N_POSE_S8_V1 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        ESP_LOGI("coco_pose", "Creating YOLO11N_POSE_S8_V1 model...");
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_s8_v1.espdl");
        ESP_LOGI("coco_pose", "YOLO11N_POSE_S8_V1 model created at %p", (void*)m_model);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_s8_v1 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_S8_V2:
#if CONFIG_COCO_POSE_YOLO11N_POSE_S8_V2 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        ESP_LOGI("coco_pose", "Creating YOLO11N_POSE_S8_V2 model...");
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_s8_v2.espdl");
        ESP_LOGI("coco_pose", "YOLO11N_POSE_S8_V2 model created at %p", (void*)m_model);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_s8_v2 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_320_S8_V2:
#if CONFIG_COCO_POSE_YOLO11N_POSE_320_S8_V2 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        ESP_LOGI("coco_pose", "Creating YOLO11N_POSE_320_S8_V2 model...");
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_320_s8_v2.espdl");
        ESP_LOGI("coco_pose", "YOLO11N_POSE_320_S8_V2 model created at %p", (void*)m_model);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_320_s8_v2 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_320_P4_V3:
#if CONFIG_COCO_POSE_YOLO11N_POSE_320_P4_V3 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        ESP_LOGI("coco_pose", "Creating YOLO11N_POSE_320_P4_V3 model...");
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_320_p4_v3.espdl");
        ESP_LOGI("coco_pose", "YOLO11N_POSE_320_P4_V3 model created at %p", (void*)m_model);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_320_p4_v3 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_256_P4:
#if CONFIG_COCO_POSE_YOLO11N_POSE_256_P4 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        ESP_LOGI("coco_pose", "Creating YOLO11N_POSE_256_P4 model...");
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_256_p4.espdl");
        ESP_LOGI("coco_pose", "YOLO11N_POSE_256_P4 model created at %p", (void*)m_model);
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_256_p4 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4 || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl");
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4 is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_TRAINED:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_TRAINED || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4_trained.espdl");
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_trained is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_NEW:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_NEW || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl");
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_new is not selected in menuconfig.");
#endif
        break;
    case model_type_t::YOLO11N_POSE_224_P4_CUSTOM:
#if CONFIG_COCO_POSE_YOLO11N_POSE_224_P4_CUSTOM || CONFIG_COCO_POSE_MODEL_IN_SDCARD
        m_model = new coco_pose::Yolo11nPose("coco_pose_yolo11n_pose_224_p4.espdl");
#else
        ESP_LOGE("coco_pose", "coco_pose_yolo11n_pose_224_p4_custom is not selected in menuconfig.");
#endif
        break;
    }
}

// 通过完整文件路径构造，绕过 sdkconfig 目录配置
COCOPose::COCOPose(const char* full_path)
{
    ESP_LOGI("coco_pose", "COCOPose constructor called with full_path=%s", full_path ? full_path : "(null)");
    if (!full_path) {
        ESP_LOGE("coco_pose", "full_path is null");
        return;
    }
    // 取文件名部分作为 model_name
    const char* model_name = strrchr(full_path, '/');
    model_name = model_name ? model_name + 1 : full_path;
    m_model = new coco_pose::Yolo11nPose(model_name, full_path);
    ESP_LOGI("coco_pose", "COCOPose(full_path) model created at %p", (void*)m_model);
}
