#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <functional>
#include <cstdint>

// 坐姿类型
typedef enum {
    POSTURE_NORMAL = 0,   // 正常坐姿
    POSTURE_LYING_DOWN,   // 趴桌
    POSTURE_HEAD_SUPPORT, // 撑头
    POSTURE_SLOUCHING,    // 弯腰驼背
    POSTURE_LEAN_BACK,    // 后仰
    POSTURE_TILTED,       // 身体倾斜
    POSTURE_UNKNOWN       // 未知
} posture_type_t;

static const char* const POSTURE_NAMES[] = {
    "正常坐姿", "趴桌", "撑头", "弯腰驼背", "后仰", "身体倾斜", "未知状态"
};

// 检测结果
struct posture_result_t {
    bool        detected       = false;
    int         person_count   = 0;
    float       confidence     = 0.f;
    posture_type_t posture_type = POSTURE_UNKNOWN;
    char        status_text[128] = {};
};

// 回调类型：坐姿检测完成后调用（在检测任务上下文）
using PostureCallback = std::function<void(const posture_result_t&)>;

class COCOPose;  // forward declare

class PostureDetector {
public:
    PostureDetector();
    ~PostureDetector();

    // 启动/停止检测
    // camera 必须是 EspVideo*（或任何具有 Capture/GetFrame* 接口的实例）
    bool Start(void* camera, PostureCallback cb);
    void Stop();

    bool IsRunning() const { return running_; }

private:
    static void ModelLoaderTask(void* arg);
    static void CameraTask(void* arg);
    static void DetectTask(void* arg);

    // 内部帧缓冲（单帧）
    struct FrameSlot {
        uint8_t* data   = nullptr;
        uint32_t width  = 0;
        uint32_t height = 0;
        bool     valid  = false;
    };

    volatile bool   running_          = false;
    volatile bool   model_loaded_     = false;
    volatile bool   model_loading_    = false;
    volatile bool   loader_running_   = false;  // 防止并发启动两个加载任务

    void*           camera_           = nullptr;
    PostureCallback callback_;

    COCOPose*       model_            = nullptr;

    // 图像缓冲（PSRAM）
    uint8_t*        rgb888_buf_       = nullptr;  // 摄像头原始转RGB888
    uint8_t*        resized_buf_      = nullptr;  // 缩放到模型输入尺寸

    // 任务句柄
    TaskHandle_t    camera_task_      = nullptr;
    TaskHandle_t    detect_task_      = nullptr;
    TaskHandle_t    loader_task_      = nullptr;

    // 帧传递队列（detect_queue 只持有一个槽，overwrite）
    QueueHandle_t   detect_queue_     = nullptr;

    // 模型加载信号量
    SemaphoreHandle_t model_sem_      = nullptr;

    static constexpr int TARGET_W = 256;
    static constexpr int TARGET_H = 256;
};
