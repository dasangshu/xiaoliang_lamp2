#include "posture_detector.h"
#include "coco_pose.hpp"
#include "dl_image.hpp"
#include "../boards/common/esp_video.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <linux/videodev2.h>
#include <dirent.h>

static const char* TAG = "PostureDetector";

// ---------------------------------------------------------------------------
// RGB565 → RGB888
// ---------------------------------------------------------------------------
static void rgb565_to_rgb888(const uint8_t* src, uint8_t* dst, int w, int h) {
    for (int i = 0; i < w * h; i++) {
        uint16_t px = src[i * 2] | (src[i * 2 + 1] << 8);
        uint8_t r = (px >> 11) & 0x1F;
        uint8_t g = (px >> 5)  & 0x3F;
        uint8_t b =  px        & 0x1F;
        dst[i * 3 + 0] = (b << 3) | (b >> 2);
        dst[i * 3 + 1] = (g << 2) | (g >> 4);
        dst[i * 3 + 2] = (r << 3) | (r >> 2);
    }
}

// YUYV (YUV 4:2:2 interleaved) → RGB888
// 每4字节 [Y0 U Y1 V] 对应2个像素
static inline uint8_t clamp_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static void yuyv_to_rgb888(const uint8_t* src, uint8_t* dst, int w, int h) {
    int npixels = w * h;
    for (int i = 0; i < npixels; i += 2) {
        int y0 = src[i * 2 + 0];
        int u  = src[i * 2 + 1] - 128;
        int y1 = src[i * 2 + 2];
        int v  = src[i * 2 + 3] - 128;
        // pixel 0
        dst[i * 3 + 0] = clamp_u8(y0 + (int)(1.772f * u));
        dst[i * 3 + 1] = clamp_u8(y0 - (int)(0.344f * u) - (int)(0.714f * v));
        dst[i * 3 + 2] = clamp_u8(y0 + (int)(1.402f * v));
        // pixel 1
        dst[i * 3 + 3] = clamp_u8(y1 + (int)(1.772f * u));
        dst[i * 3 + 4] = clamp_u8(y1 - (int)(0.344f * u) - (int)(0.714f * v));
        dst[i * 3 + 5] = clamp_u8(y1 + (int)(1.402f * v));
    }
}

// ---------------------------------------------------------------------------
// 坐姿分析（关键点 → posture_type_t）
// ---------------------------------------------------------------------------
static posture_type_t analyze_posture(const std::vector<int>& kp) {
    auto get = [&](int idx) -> std::pair<float,float> {
        if (idx * 2 + 1 < (int)kp.size())
            return {(float)kp[idx*2], (float)kp[idx*2+1]};
        return {0,0};
    };
    auto valid = [](std::pair<float,float> p){ return p.first>0 && p.second>0; };

    const int NOSE=0, LEFT_EYE=1, RIGHT_EYE=2;
    const int LEFT_SHOULDER=5, RIGHT_SHOULDER=6;
    const int LEFT_WRIST=9, RIGHT_WRIST=10;
    const int LEFT_HIP=11, RIGHT_HIP=12;

    auto nose         = get(NOSE);
    auto left_eye     = get(LEFT_EYE);
    auto right_eye    = get(RIGHT_EYE);
    auto left_sh      = get(LEFT_SHOULDER);
    auto right_sh     = get(RIGHT_SHOULDER);
    auto left_wr      = get(LEFT_WRIST);
    auto right_wr     = get(RIGHT_WRIST);
    auto left_hip     = get(LEFT_HIP);
    auto right_hip    = get(RIGHT_HIP);

    if (!valid(nose) || (!valid(left_sh) && !valid(right_sh)))
        return POSTURE_UNKNOWN;

    // 头部中心
    std::pair<float,float> head = nose;
    if (valid(left_eye) && valid(right_eye)) {
        head.first  = (left_eye.first  + right_eye.first  + nose.first)  / 3.f;
        head.second = (left_eye.second + right_eye.second + nose.second) / 3.f;
    }

    // 肩膀中心
    std::pair<float,float> sh_center = {0,0};
    int sh_cnt = 0;
    if (valid(left_sh))  { sh_center.first += left_sh.first;  sh_center.second += left_sh.second;  sh_cnt++; }
    if (valid(right_sh)) { sh_center.first += right_sh.first; sh_center.second += right_sh.second; sh_cnt++; }
    sh_center.first /= sh_cnt; sh_center.second /= sh_cnt;

    // 臀部中心
    std::pair<float,float> hip = {0,0};
    int hip_cnt = 0;
    if (valid(left_hip))  { hip.first += left_hip.first;  hip.second += left_hip.second;  hip_cnt++; }
    if (valid(right_hip)) { hip.first += right_hip.first; hip.second += right_hip.second; hip_cnt++; }
    if (hip_cnt) { hip.first /= hip_cnt; hip.second /= hip_cnt; }

    float body_h = (hip_cnt > 0) ? fabsf(sh_center.second - hip.second) : 100.f;
    if (body_h < 50.f) body_h = 100.f;

    float head_sh_y = (head.second - sh_center.second) / body_h;
    float head_sh_x = fabsf(head.first - sh_center.first) / body_h;

    // 肩膀倾斜
    float tilt = 0.f;
    if (valid(left_sh) && valid(right_sh)) {
        float dy = right_sh.second - left_sh.second;
        float dx = fabsf(right_sh.first - left_sh.first);
        if (dx > 0.f) tilt = atanf(dy / dx) * 180.f / M_PI;
    }

    // 手扶头
    float min_hand_dist = 999.f;
    if (valid(left_wr)) {
        float d = sqrtf(powf(left_wr.first-head.first,2)+powf(left_wr.second-head.second,2)) / body_h;
        min_hand_dist = fminf(min_hand_dist, d);
    }
    if (valid(right_wr)) {
        float d = sqrtf(powf(right_wr.first-head.first,2)+powf(right_wr.second-head.second,2)) / body_h;
        min_hand_dist = fminf(min_hand_dist, d);
    }

    // 判断
    if (head_sh_y > 0.12f && head_sh_x > 0.08f) return POSTURE_LYING_DOWN;
    if (min_hand_dist < 0.25f && head_sh_y > -0.05f) return POSTURE_HEAD_SUPPORT;
    if (head_sh_y > 0.04f || head_sh_x > 0.06f)  return POSTURE_SLOUCHING;
    if (fabsf(tilt) > 8.f)                         return POSTURE_TILTED;
    if (head_sh_y < -0.10f)                        return POSTURE_LEAN_BACK;
    return POSTURE_NORMAL;
}

// ---------------------------------------------------------------------------
// PostureDetector
// ---------------------------------------------------------------------------
PostureDetector::PostureDetector() {}

PostureDetector::~PostureDetector() {
    Stop();
}

bool PostureDetector::Start(void* camera, PostureCallback cb) {
    if (running_) return true;
    if (!camera) {
        ESP_LOGE(TAG, "camera is null");
        return false;
    }

    camera_   = camera;
    callback_ = cb;

    // 分配 PSRAM 缓冲
    if (!rgb888_buf_)
        rgb888_buf_  = (uint8_t*)heap_caps_malloc(640 * 480 * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resized_buf_)
        resized_buf_ = (uint8_t*)heap_caps_malloc(TARGET_W * TARGET_H * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb888_buf_ || !resized_buf_) {
        ESP_LOGE(TAG, "buffer alloc failed");
        heap_caps_free(rgb888_buf_);  rgb888_buf_  = nullptr;
        heap_caps_free(resized_buf_); resized_buf_ = nullptr;
        return false;
    }

    detect_queue_ = xQueueCreate(1, sizeof(FrameSlot));
    model_sem_    = xSemaphoreCreateBinary();
    if (!detect_queue_ || !model_sem_) {
        ESP_LOGE(TAG, "queue/sem alloc failed");
        return false;
    }

    running_ = true;

    // 启动加载任务：仅当模型未就绪且没有加载任务在跑时才创建
    if (model_loaded_) {
        // 模型已在 PSRAM，直接释放信号量让检测任务开始
        xSemaphoreGive(model_sem_);
    } else if (!loader_running_) {
        loader_running_ = true;
        // ModelLoaderTask 必须用内部 SRAM 栈：SDMMC DMA 不能访问 PSRAM，
        // FbsLoader 在栈上分配的读取缓冲区必须在 DMA 可达的内存里
        xTaskCreate(ModelLoaderTask, "pose_loader", 16384, this, 1, &loader_task_);
    }
    // 若 loader_running_==true 且 model_loaded_==false：上次的加载任务还在跑，
    // 它完成时会 xSemaphoreGive(self->model_sem_)，检测任务自然会收到

    // 任务栈分配到 PSRAM：DetectTask 需要 40KB，放内部 SRAM 会在模型加载期间导致分配失败
    BaseType_t r1 = xTaskCreateWithCaps(DetectTask, "pose_detect", 40960, this, 1, &detect_task_,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    BaseType_t r2 = xTaskCreateWithCaps(CameraTask, "pose_camera", 8192,  this, 1, &camera_task_,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败 detect=%d camera=%d", (int)r1, (int)r2);
    }

    return true;
}

void PostureDetector::Stop() {
    running_ = false;
    // 注意：不重置 loader_running_，loader 任务完成时会自己清除
    // 注意：不重置 model_loading_，loader 任务完成时会自己清除

    // 给任务最多 300ms 自行退出
    vTaskDelay(pdMS_TO_TICKS(300));

    if (camera_task_) { vTaskDelete(camera_task_); camera_task_ = nullptr; }
    if (detect_task_) { vTaskDelete(detect_task_); detect_task_ = nullptr; }
    loader_task_ = nullptr;  // loader 自行退出，不强杀

    if (detect_queue_) { vQueueDelete(detect_queue_); detect_queue_ = nullptr; }
    if (model_sem_)    { vSemaphoreDelete(model_sem_); model_sem_ = nullptr; }

    // 保留 rgb888_buf_ / resized_buf_ 和已加载的 model_，下次 Start() 直接复用
}

// ---------------------------------------------------------------------------
// ModelLoaderTask
// ---------------------------------------------------------------------------
void PostureDetector::ModelLoaderTask(void* arg) {
    PostureDetector* self = (PostureDetector*)arg;
    ESP_LOGI(TAG, "模型加载任务启动");

    // 模型已加载（Stop 保留了模型），直接跳过加载流程（此分支正常不应触发，已在 Start() 提前处理）
    if (self->model_loaded_ && self->model_) {
        ESP_LOGI(TAG, "模型已在内存中，跳过加载");
        self->loader_running_ = false;
        if (self->model_sem_) xSemaphoreGive(self->model_sem_);
        vTaskDelete(NULL);
        return;
    }

    self->model_loading_ = true;

    // 候选路径：先尝试 sdkconfig 配置路径，再依次尝试回退路径
    static const char* const CANDIDATE_PATHS[] = {
        "/sdcard/models/p4/coco_pose_yolo11n_pose_256_p4.espdl",
        "/sdcard/models/coco_pose_yolo11n_pose_256_p4.espdl",
        "/sdcard/models/p4/coco_pose_yolo11n_pose_256_p4_v2.espdl",
        nullptr
    };

    // 找到第一个可以打开的文件
    const char* model_file = nullptr;
    for (int i = 0; CANDIDATE_PATHS[i] != nullptr && model_file == nullptr; i++) {
        FILE* f = fopen(CANDIDATE_PATHS[i], "rb");
        if (f) {
            fclose(f);
            model_file = CANDIDATE_PATHS[i];
            ESP_LOGI(TAG, "找到模型文件: %s", model_file);
        } else {
            ESP_LOGD(TAG, "不存在: %s", CANDIDATE_PATHS[i]);
        }
    }

    if (!model_file) {
        // 列举 /sdcard/models/ 帮助定位
        DIR* d = opendir("/sdcard/models");
        if (d) {
            ESP_LOGI(TAG, "=== /sdcard/models/ 目录内容 ===");
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                ESP_LOGI(TAG, "  [%s] %s", ent->d_type == DT_DIR ? "DIR" : "FILE", ent->d_name);
                if (ent->d_type == DT_DIR && ent->d_name[0] != '.') {
                    char sub[300];
                    snprintf(sub, sizeof(sub), "/sdcard/models/%s", ent->d_name);
                    DIR* sd = opendir(sub);
                    if (sd) {
                        struct dirent* se;
                        while ((se = readdir(sd)) != nullptr)
                            ESP_LOGI(TAG, "    [%s] %s", se->d_type == DT_DIR ? "DIR" : "FILE", se->d_name);
                        closedir(sd);
                    }
                }
            }
            closedir(d);
        } else {
            ESP_LOGE(TAG, "/sdcard/models/ 目录不存在！请在 SD 卡根目录创建 models/p4/ 并放入模型文件");
        }
        self->model_loading_ = false;
        self->loader_running_ = false;
        vTaskDelete(NULL);
        return;
    }

    try {
        self->model_ = new COCOPose(model_file);
    } catch (...) {
        ESP_LOGE(TAG, "模型加载异常");
        self->model_loading_ = false;
        self->loader_running_ = false;
        vTaskDelete(NULL);
        return;
    }

    // COCOPose 加载失败时内部 m_model 会置 null，检查是否成功
    if (!self->model_) {
        ESP_LOGE(TAG, "模型创建返回 nullptr");
        self->model_loading_ = false;
        self->loader_running_ = false;
        vTaskDelete(NULL);
        return;
    }

    self->model_loaded_  = true;
    self->model_loading_ = false;
    self->loader_running_ = false;
    ESP_LOGI(TAG, "模型加载完成，开始检测");

    if (self->model_sem_) xSemaphoreGive(self->model_sem_);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// CameraTask：采集 → 转 RGB888 → 缩放 → 送入队列（1Hz）
// ---------------------------------------------------------------------------
void PostureDetector::CameraTask(void* arg) {
    PostureDetector* self = (PostureDetector*)arg;
    EspVideo* cam = (EspVideo*)self->camera_;

    TickType_t last_send = 0;
    const TickType_t INTERVAL = pdMS_TO_TICKS(1000);  // 1fps 送检测

    while (true) {

        if (!self->running_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!cam->Capture()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        uint8_t* data   = cam->GetFrameData();
        uint32_t width  = cam->GetFrameWidth();
        uint32_t height = cam->GetFrameHeight();
        uint32_t fmt    = cam->GetFrameFormat();

        if (!data || width == 0 || height == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 只以 1fps 送检测
        TickType_t now = xTaskGetTickCount();
        if (now - last_send < INTERVAL) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        last_send = now;

        // 格式转换 → rgb888_buf_
        if (fmt == V4L2_PIX_FMT_RGB565) {
            if (width * height * 3 <= 640 * 480 * 3)
                rgb565_to_rgb888(data, self->rgb888_buf_, (int)width, (int)height);
        } else if (fmt == V4L2_PIX_FMT_YUYV) {
            // YUYV: 每像素2字节，640x480=614400字节
            if (width * height * 2 <= 640 * 480 * 2)
                yuyv_to_rgb888(data, self->rgb888_buf_, (int)width, (int)height);
        } else if (fmt == V4L2_PIX_FMT_RGB24) {
            if (width * height * 3 <= 640 * 480 * 3)
                memcpy(self->rgb888_buf_, data, width * height * 3);
        } else {
            static uint32_t logged_fmt = 0;
            if (fmt != logged_fmt) {
                ESP_LOGW(TAG, "不支持的帧格式 0x%08lx", (unsigned long)fmt);
                logged_fmt = fmt;
            }
            continue;
        }

        // 缩放到 TARGET_W x TARGET_H
        if (width == TARGET_W && height == TARGET_H) {
            memcpy(self->resized_buf_, self->rgb888_buf_, TARGET_W * TARGET_H * 3);
        } else {
            dl::image::img_t src = { self->rgb888_buf_, (uint16_t)width,  (uint16_t)height,  dl::image::DL_IMAGE_PIX_TYPE_RGB888 };
            dl::image::img_t dst = { self->resized_buf_, TARGET_W, TARGET_H, dl::image::DL_IMAGE_PIX_TYPE_RGB888 };
            dl::image::resize(src, dst, dl::image::DL_IMAGE_INTERPOLATE_BILINEAR);
        }

        FrameSlot slot = { self->resized_buf_, (uint32_t)TARGET_W, (uint32_t)TARGET_H, true };
        if (self->detect_queue_) xQueueOverwrite(self->detect_queue_, &slot);

        vTaskDelay(pdMS_TO_TICKS(30));
    }

    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// DetectTask：从队列取帧 → 推理 → 分析 → 回调
// ---------------------------------------------------------------------------
void PostureDetector::DetectTask(void* arg) {
    PostureDetector* self = (PostureDetector*)arg;

    // 等待模型加载（最多 120s）
    ESP_LOGI(TAG, "等待模型加载...");
    if (self->model_sem_) {
        xSemaphoreTake(self->model_sem_, pdMS_TO_TICKS(120000));
    }
    if (!self->model_loaded_ || !self->model_) {
        ESP_LOGE(TAG, "模型未就绪，检测任务退出");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "开始姿态检测");

    FrameSlot slot;
    while (true) {

        if (!self->running_) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xQueueReceive(self->detect_queue_, &slot, pdMS_TO_TICKS(200)) != pdTRUE || !slot.valid)
            continue;

        // 内存安全检查
        if (esp_get_free_heap_size() < 100000) {
            ESP_LOGW(TAG, "内存不足，跳过检测");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 验证图像不全黑
        uint32_t sum = 0;
        for (int i = 0; i < 300; i++) sum += slot.data[i * 3];
        if (sum / 300 < 5) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        dl::image::img_t img = { slot.data, (uint16_t)slot.width, (uint16_t)slot.height, dl::image::DL_IMAGE_PIX_TYPE_RGB888 };

        std::list<dl::detect::result_t> results;
        try {
            taskYIELD();
            results = self->model_->run(img);
        } catch (...) {
            ESP_LOGE(TAG, "推理异常");
            continue;
        }

        posture_result_t out = {};
        if (!results.empty()) {
            const auto& person = results.front();
            out.detected     = (person.score > 0.3f);
            out.person_count = (int)results.size();
            out.confidence   = person.score;

            if (out.detected) {
                std::vector<int> kp(person.keypoint.begin(), person.keypoint.end());
                out.posture_type = analyze_posture(kp);
                snprintf(out.status_text, sizeof(out.status_text),
                         "%s (%.0f%%)", POSTURE_NAMES[out.posture_type], person.score * 100);
                ESP_LOGI(TAG, "检测结果: %s", out.status_text);
            }
        }

        if (self->callback_) self->callback_(out);

        // 检测完给 CPU 让出时间
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vTaskDelete(NULL);
}
