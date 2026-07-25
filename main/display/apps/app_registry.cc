#include "app_registry.h"

#include <font_awesome.h>

namespace app_ui {

const char* const kTaskActions[] = {"今日任务", "任务统计", "添加任务"};
const char* const kPomodoroActions[] = {"开始专注", "短休息", "长休息"};
const char* const kEyeIslandActions[] = {"视力训练", "放松眼睛", "护眼报告"};
const char* const kPetActions[] = {"宠物中心", "喂养", "换肤"};
const char* const kMusicActions[] = {"自然之声", "专注学习", "助眠放松"};
const char* const kAiSpeakActions[] = {"日常对话", "英语练习", "情景列表"};
const char* const kHealthActions[] = {"坐姿提醒", "饮水提醒", "休息打卡"};
const char* const kAlbumActions[] = {"表情管理", "照片预览", "动画素材"};
const char* const kDeviceActions[] = {"亮度", "音量", "网络"};
const char* const kMoreActions[] = {"成长档案", "系统信息", "模块扩展"};

namespace {

constexpr AppModule kModules[] = {
    {AppKind::kTask, FONT_AWESOME_CLOCK, "app_task", "任务计划", "今日安排",
     "提醒、日程与下一件事", "效率", 0x16705E, 0xDDF2EB,
     kTaskActions, 3},
    {AppKind::kPomodoro, FONT_AWESOME_ALARM_CLOCK, "app_tomato", "番茄时钟", "沉浸专注",
     "专注与休息自然切换", "热门", 0xC84A36, 0xFBE5DF,
     kPomodoroActions, 3},
    {AppKind::kEyeIsland, FONT_AWESOME_GLASSES, "app_island", "护眼岛", "用眼守护",
     "护眼分、远眺和坐姿", "健康", 0x2E7655, 0xE0F1E6,
     kEyeIslandActions, 3},
    {AppKind::kPet, FONT_AWESOME_GAMEPAD, "app_pet", "宠物花园", "桌面伙伴",
     "皮肤、表情与陪伴", "趣味", 0xB8682B, 0xFBE9D5,
     kPetActions, 3},
    {AppKind::kMusic, FONT_AWESOME_MUSIC, "app_music", "音乐盒", "场景声音",
     "专注、自然与睡眠音频", "推荐", 0x35658D, 0xE2ECF5,
     kMusicActions, 3},
    {AppKind::kAiSpeaking, FONT_AWESOME_COMMENT, "app_ai", "AI听说", "情景对话",
     "角色扮演与口语训练", "AI", 0x7352A0, 0xECE4F6,
     kAiSpeakActions, 3},
    {AppKind::kHealth, FONT_AWESOME_HEART, "app_health", "健康中心", "活力节奏",
     "饮水、休息与坐姿提醒", "习惯", 0xA05C3B, 0xF6E8DF,
     kHealthActions, 3},
    {AppKind::kAlbum, FONT_AWESOME_IMAGE, "app_album", "表情相册", "动画素材",
     "管理角色表情与动画", "创意", 0x547356, 0xE6EFE5,
     kAlbumActions, 3},
    {AppKind::kDevice, FONT_AWESOME_GEAR, "app_device", "设备设置", "偏好设置",
     "亮度、音量与连接状态", "系统", 0x385668, 0xE3ECF0,
     kDeviceActions, 3},
    {AppKind::kMore, FONT_AWESOME_STAR, "app_device", "更多", "能力扩展",
     "成长档案与新模块", "探索", 0x625E70, 0xEBE9F0,
     kMoreActions, 3},
};

}  // namespace

const AppModule* GetAppModules() {
    return kModules;
}

size_t GetAppModuleCount() {
    return sizeof(kModules) / sizeof(kModules[0]);
}

const AppModule* FindAppModule(AppKind kind) {
    for (const auto& module : kModules) {
        if (module.kind == kind) {
            return &module;
        }
    }
    return nullptr;
}

}  // namespace app_ui
