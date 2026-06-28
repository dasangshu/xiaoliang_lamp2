#pragma once
#include "dl_detect_base.hpp"
#include "dl_pose_yolo11_postprocessor.hpp"

namespace coco_pose {
class Yolo11nPose : public dl::detect::DetectImpl {
public:
    // model_name: 文件名（从 sdkconfig 路径拼接）
    // full_path: 若非 nullptr，直接使用此完整路径（忽略 sdkconfig 配置）
    Yolo11nPose(const char *model_name, const char *full_path = nullptr);
};
} // namespace coco_pose

class COCOPose : public dl::detect::DetectWrapper {
public:
    typedef enum {
        YOLO11N_POSE_S8_V1,
        YOLO11N_POSE_S8_V2,
        YOLO11N_POSE_320_S8_V2,
        YOLO11N_POSE_320_P4_V3,
        YOLO11N_POSE_256_P4,
        YOLO11N_POSE_224_P4,
        YOLO11N_POSE_224_P4_TRAINED,
        YOLO11N_POSE_224_P4_NEW,
        YOLO11N_POSE_224_P4_CUSTOM
    } model_type_t;
    // 按类型构造（路径由 sdkconfig 决定）
    COCOPose(model_type_t model_type = static_cast<model_type_t>(CONFIG_DEFAULT_COCO_POSE_MODEL));
    // 直接用完整路径构造（绕过 sdkconfig）
    explicit COCOPose(const char* full_path);
};
