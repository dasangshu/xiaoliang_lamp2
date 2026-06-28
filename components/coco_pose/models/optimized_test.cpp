
// 优化后的测试代码
#include "coco_pose.hpp"
#include "esp_timer.h"

void test_optimized_inference() {
    // 使用nano模型
    COCOPose *pose = new COCOPose(COCOPose::YOLO11N_POSE_224_NANO_P4);
    
    // 创建测试图像
    dl::image::img_t test_img = {
        .data = test_data,
        .width = 224,
        .height = 224,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888
    };
    
    // 预热
    for(int i = 0; i < 3; i++) {
        auto &results = pose->run(test_img);
    }
    
    // 性能测试
    uint64_t start_time = esp_timer_get_time();
    for(int i = 0; i < 10; i++) {
        auto &results = pose->run(test_img);
    }
    uint64_t end_time = esp_timer_get_time();
    
    float avg_time = (end_time - start_time) / 10000.0f; // 转换为毫秒
    ESP_LOGI("BENCHMARK", "平均推理时间: %.1f ms", avg_time);
    
    delete pose;
}
