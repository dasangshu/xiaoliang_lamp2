#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <cJSON.h>

class EyeCareService {
public:
    enum class EventType {
        kNone,
        kStudyStarted,
        kFarSightReminder,
        kFarSightCompleted,
        kDailyKnowledge,
        kScreeningReminder,
        kGripPostureTip,
        kVisionChartTip,
        kScienceVideoTip,
    };

    struct Event {
        EventType type = EventType::kNone;
        std::string message;
        std::string emotion;
        bool play_sound = false;
        uint32_t display_ms = 0;
    };

    struct Config {
        bool enabled = true;
        bool far_sight_enabled = true;
        bool daily_knowledge_enabled = true;
        bool posture_enabled = true;
        bool screening_enabled = true;
        bool grip_tip_enabled = true;
        bool vision_chart_tip_enabled = true;
        bool science_video_tip_enabled = true;
        bool screen_video_enabled = true;
        bool rest_music_enabled = false;
        uint16_t continuous_use_minutes = 20;
        uint16_t far_sight_seconds = 20;
        uint16_t absence_reset_seconds = 180;
    };

    EyeCareService();

    void LoadSettings();
    void SaveSettings() const;
    void ResetDailyStats();
    void OnPresenceDetected(bool detected);
    Event OnClockTick(bool device_idle);

    cJSON* ToJson() const;
    const Config& config() const { return config_; }
    void ApplyConfig(const Config& config);

private:
    struct RuleMessage {
        std::string text;
        std::string emotion;
        bool play_sound = false;
        uint32_t display_ms = 3000;
    };

    struct Rules {
        uint8_t presence_ttl_seconds = 5;
        uint32_t study_start_knowledge_delay_seconds = 10;
        uint32_t min_far_sight_reminder_gap_seconds = 5 * 60;
        uint32_t screening_reminder_interval_seconds = 30 * 24 * 60 * 60;
        uint32_t grip_tip_delay_seconds = 60;
        uint32_t vision_chart_tip_delay_seconds = 120;
        uint32_t science_video_tip_delay_seconds = 180;
        RuleMessage study_started = {
            "开始学习啦，记得坐直，用眼一段时间后我会提醒你远眺",
            "glass.mjpeg",
            false,
            3000,
        };
        RuleMessage far_sight_reminder = {
            "已经连续用眼一段时间啦，请抬头看6米外，远眺20秒",
            "relaxed.mjpeg",
            true,
            20 * 1000,
        };
        RuleMessage far_sight_completed = {
            "远眺休息完成，继续学习时记得保持坐姿",
            "relaxed.mjpeg",
            false,
            2500,
        };
        RuleMessage daily_knowledge = {
            "",
            "thinking.mjpeg",
            false,
            6000,
        };
        RuleMessage screening_reminder = {
            "本月建议安排一次视力筛查，关注左右眼视力变化",
            "thinking.mjpeg",
            false,
            6000,
        };
        RuleMessage grip_posture_tip = {
            "握笔不要太靠近笔尖，避免挡住视线后低头歪头",
            "bye.mjpeg",
            false,
            5000,
        };
        RuleMessage vision_chart_tip = {
            "可以在家做一次视力表方向识别练习，结果仅作家庭参考",
            "thinking.mjpeg",
            false,
            6000,
        };
        RuleMessage science_video_tip = {
            "看一个护眼小动效：坐直、抬头、看远处",
            "baojiancao.mjpeg",
            false,
            5000,
        };
        std::vector<std::string> daily_knowledge_items;
        std::string source = "builtin-fallback";
    };

    const char* PickDailyKnowledge() const;
    Event MakeEvent(EventType type, const RuleMessage& message) const;
    void LoadRules();
    void ApplyRulesJson(const char* json, size_t length, const char* source);
    void ClampConfig();

    Config config_;
    Rules rules_;
    bool studying_ = false;
    bool presence_detected_ = false;
    bool rest_pending_ = false;
    bool daily_knowledge_sent_ = false;
    bool grip_tip_sent_ = false;
    bool vision_chart_tip_sent_ = false;
    bool science_video_tip_sent_ = false;
    uint8_t presence_ttl_seconds_ = 0;

    uint32_t today_study_seconds_ = 0;
    uint32_t continuous_use_seconds_ = 0;
    uint32_t absence_seconds_ = 0;
    uint32_t far_sight_reminders_ = 0;
    uint32_t far_sight_completed_ = 0;
    uint32_t study_sessions_ = 0;
    uint32_t uptime_seconds_ = 0;
    uint32_t last_far_sight_reminder_uptime_ = 0;
    uint32_t last_screening_reminder_uptime_ = 0;
};
