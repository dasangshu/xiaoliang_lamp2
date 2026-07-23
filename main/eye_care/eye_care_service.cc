#include "eye_care_service.h"

#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace {
const char* TAG = "EyeCareService";
constexpr uint32_t kSecondsPerDay = 24 * 60 * 60;

extern const char eye_care_rules_json_start[] asm("_binary_eye_care_rules_json_start");
extern const char eye_care_rules_json_end[] asm("_binary_eye_care_rules_json_end");

bool JsonBool(cJSON* obj, const char* key, bool fallback) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(item) ? item->valueint == 1 : fallback;
}

int JsonInt(cJSON* obj, const char* key, int fallback) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

std::string JsonString(cJSON* obj, const char* key, const std::string& fallback) {
    auto item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : fallback;
}

std::string ReadTextFile(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return {};
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        return {};
    }

    std::string data;
    data.resize(st.st_size);
    size_t read = fread(data.data(), 1, data.size(), f);
    fclose(f);
    data.resize(read);
    return data;
}
}  // namespace

EyeCareService::EyeCareService() {
    rules_.daily_knowledge_items = {
        "今日护眼小知识：写字时眼睛离书本保持一尺左右，能减少眼疲劳。",
        "今日护眼小知识：每连续用眼一段时间，要抬头看看远处。",
        "今日护眼小知识：握笔不要太靠近笔尖，避免挡住视线后低头歪头。",
        "今日护眼小知识：学习时桌面和环境都要有合适亮度，不要摸黑看书。",
        "今日护眼小知识：户外活动有助于放松眼睛，白天尽量多到户外走走。",
        "今日护眼小知识：不要趴着写字，眼睛和书本距离会不自觉变近。",
        "今日护眼小知识：如果经常眯眼、歪头看东西，建议尽快做视力检查。",
    };
    LoadRules();
    LoadSettings();
}

void EyeCareService::LoadSettings() {
    Settings settings("eye_care", false);
    config_.enabled = settings.GetBool("enabled", config_.enabled);
    config_.far_sight_enabled = settings.GetBool("far_sight", config_.far_sight_enabled);
    config_.daily_knowledge_enabled = settings.GetBool("daily_tip", config_.daily_knowledge_enabled);
    config_.posture_enabled = settings.GetBool("posture", config_.posture_enabled);
    config_.screening_enabled = settings.GetBool("screening", config_.screening_enabled);
    config_.grip_tip_enabled = settings.GetBool("grip_tip", config_.grip_tip_enabled);
    config_.vision_chart_tip_enabled = settings.GetBool("vision_tip", config_.vision_chart_tip_enabled);
    config_.science_video_tip_enabled = settings.GetBool("video_tip", config_.science_video_tip_enabled);
    config_.screen_video_enabled = settings.GetBool("screen_video", config_.screen_video_enabled);
    config_.rest_music_enabled = settings.GetBool("rest_music", config_.rest_music_enabled);
    config_.continuous_use_minutes = settings.GetInt("use_minutes", config_.continuous_use_minutes);
    config_.far_sight_seconds = settings.GetInt("gaze_seconds", config_.far_sight_seconds);
    config_.absence_reset_seconds = settings.GetInt("reset_seconds", config_.absence_reset_seconds);
    ClampConfig();
}

void EyeCareService::SaveSettings() const {
    Settings settings("eye_care", true);
    settings.SetBool("enabled", config_.enabled);
    settings.SetBool("far_sight", config_.far_sight_enabled);
    settings.SetBool("daily_tip", config_.daily_knowledge_enabled);
    settings.SetBool("posture", config_.posture_enabled);
    settings.SetBool("screening", config_.screening_enabled);
    settings.SetBool("grip_tip", config_.grip_tip_enabled);
    settings.SetBool("vision_tip", config_.vision_chart_tip_enabled);
    settings.SetBool("video_tip", config_.science_video_tip_enabled);
    settings.SetBool("screen_video", config_.screen_video_enabled);
    settings.SetBool("rest_music", config_.rest_music_enabled);
    settings.SetInt("use_minutes", config_.continuous_use_minutes);
    settings.SetInt("gaze_seconds", config_.far_sight_seconds);
    settings.SetInt("reset_seconds", config_.absence_reset_seconds);
}

void EyeCareService::ApplyConfig(const Config& config) {
    config_ = config;
    ClampConfig();
    SaveSettings();
}

void EyeCareService::ClampConfig() {
    config_.continuous_use_minutes = std::clamp<uint16_t>(config_.continuous_use_minutes, 5, 120);
    config_.far_sight_seconds = std::clamp<uint16_t>(config_.far_sight_seconds, 10, 180);
    config_.absence_reset_seconds = std::clamp<uint16_t>(config_.absence_reset_seconds, 30, 1800);
}

void EyeCareService::ResetDailyStats() {
    studying_ = false;
    presence_detected_ = false;
    rest_pending_ = false;
    daily_knowledge_sent_ = false;
    grip_tip_sent_ = false;
    vision_chart_tip_sent_ = false;
    science_video_tip_sent_ = false;
    presence_ttl_seconds_ = 0;
    today_study_seconds_ = 0;
    continuous_use_seconds_ = 0;
    absence_seconds_ = 0;
    far_sight_reminders_ = 0;
    far_sight_completed_ = 0;
    study_sessions_ = 0;
    last_far_sight_reminder_uptime_ = 0;
    last_screening_reminder_uptime_ = 0;
}

void EyeCareService::OnPresenceDetected(bool detected) {
    if (detected) {
        presence_detected_ = true;
        presence_ttl_seconds_ = rules_.presence_ttl_seconds;
        absence_seconds_ = 0;
    }
}

EyeCareService::Event EyeCareService::OnClockTick(bool device_idle) {
    uptime_seconds_++;

    if (uptime_seconds_ % kSecondsPerDay == 0) {
        ResetDailyStats();
    }

    if (!config_.enabled || !device_idle) {
        studying_ = false;
        return {};
    }

    const bool presence_active = presence_ttl_seconds_ > 0;
    if (presence_ttl_seconds_ > 0) {
        presence_ttl_seconds_--;
    }

    if (!presence_active) {
        presence_detected_ = false;
        if (absence_seconds_ < UINT32_MAX) {
            absence_seconds_++;
        }

        if (rest_pending_ && absence_seconds_ >= config_.far_sight_seconds) {
            rest_pending_ = false;
            continuous_use_seconds_ = 0;
            far_sight_completed_++;
            return MakeEvent(EventType::kFarSightCompleted, rules_.far_sight_completed);
        }

        if (absence_seconds_ >= config_.absence_reset_seconds) {
            continuous_use_seconds_ = 0;
            studying_ = false;
        }
        return {};
    }

    today_study_seconds_++;
    continuous_use_seconds_++;

    if (!studying_) {
        studying_ = true;
        study_sessions_++;
        grip_tip_sent_ = false;
        vision_chart_tip_sent_ = false;
        science_video_tip_sent_ = false;
        return MakeEvent(EventType::kStudyStarted, rules_.study_started);
    }

    if (config_.daily_knowledge_enabled &&
        !daily_knowledge_sent_ &&
        today_study_seconds_ >= rules_.study_start_knowledge_delay_seconds) {
        daily_knowledge_sent_ = true;
        RuleMessage message = rules_.daily_knowledge;
        message.text = PickDailyKnowledge();
        return {
            EventType::kDailyKnowledge,
            message.text,
            message.emotion,
            message.play_sound,
            message.display_ms,
        };
    }

    if (config_.grip_tip_enabled &&
        !grip_tip_sent_ &&
        continuous_use_seconds_ >= rules_.grip_tip_delay_seconds) {
        grip_tip_sent_ = true;
        return MakeEvent(EventType::kGripPostureTip, rules_.grip_posture_tip);
    }

    if (config_.vision_chart_tip_enabled &&
        !vision_chart_tip_sent_ &&
        continuous_use_seconds_ >= rules_.vision_chart_tip_delay_seconds) {
        vision_chart_tip_sent_ = true;
        return MakeEvent(EventType::kVisionChartTip, rules_.vision_chart_tip);
    }

    if (config_.science_video_tip_enabled &&
        !science_video_tip_sent_ &&
        continuous_use_seconds_ >= rules_.science_video_tip_delay_seconds) {
        science_video_tip_sent_ = true;
        return MakeEvent(EventType::kScienceVideoTip, rules_.science_video_tip);
    }

    if (config_.screening_enabled &&
        uptime_seconds_ >= rules_.screening_reminder_interval_seconds &&
        (last_screening_reminder_uptime_ == 0 ||
         uptime_seconds_ - last_screening_reminder_uptime_ >= rules_.screening_reminder_interval_seconds)) {
        last_screening_reminder_uptime_ = uptime_seconds_;
        return MakeEvent(EventType::kScreeningReminder, rules_.screening_reminder);
    }

    const uint32_t interval_sec = config_.continuous_use_minutes * 60U;
    const bool reminder_gap_ok =
        last_far_sight_reminder_uptime_ == 0 ||
        uptime_seconds_ - last_far_sight_reminder_uptime_ >=
            std::min(interval_sec, rules_.min_far_sight_reminder_gap_seconds);

    if (config_.far_sight_enabled &&
        continuous_use_seconds_ >= interval_sec &&
        reminder_gap_ok) {
        rest_pending_ = true;
        far_sight_reminders_++;
        last_far_sight_reminder_uptime_ = uptime_seconds_;
        auto event = MakeEvent(EventType::kFarSightReminder, rules_.far_sight_reminder);
        if (event.display_ms == 0) {
            event.display_ms = config_.far_sight_seconds * 1000U;
        }
        return event;
    }

    return {};
}

cJSON* EyeCareService::ToJson() const {
    cJSON* root = cJSON_CreateObject();

    cJSON* config = cJSON_CreateObject();
    cJSON_AddBoolToObject(config, "enabled", config_.enabled);
    cJSON_AddBoolToObject(config, "far_sight_enabled", config_.far_sight_enabled);
    cJSON_AddBoolToObject(config, "daily_knowledge_enabled", config_.daily_knowledge_enabled);
    cJSON_AddBoolToObject(config, "posture_enabled", config_.posture_enabled);
    cJSON_AddBoolToObject(config, "screening_enabled", config_.screening_enabled);
    cJSON_AddBoolToObject(config, "grip_tip_enabled", config_.grip_tip_enabled);
    cJSON_AddBoolToObject(config, "vision_chart_tip_enabled", config_.vision_chart_tip_enabled);
    cJSON_AddBoolToObject(config, "science_video_tip_enabled", config_.science_video_tip_enabled);
    cJSON_AddBoolToObject(config, "screen_video_enabled", config_.screen_video_enabled);
    cJSON_AddBoolToObject(config, "rest_music_enabled", config_.rest_music_enabled);
    cJSON_AddNumberToObject(config, "continuous_use_minutes", config_.continuous_use_minutes);
    cJSON_AddNumberToObject(config, "far_sight_seconds", config_.far_sight_seconds);
    cJSON_AddNumberToObject(config, "absence_reset_seconds", config_.absence_reset_seconds);
    cJSON_AddItemToObject(root, "config", config);

    cJSON* stats = cJSON_CreateObject();
    cJSON_AddBoolToObject(stats, "studying", studying_);
    cJSON_AddBoolToObject(stats, "presence_active", presence_detected_);
    cJSON_AddBoolToObject(stats, "rest_pending", rest_pending_);
    cJSON_AddNumberToObject(stats, "today_study_seconds", today_study_seconds_);
    cJSON_AddNumberToObject(stats, "continuous_use_seconds", continuous_use_seconds_);
    cJSON_AddNumberToObject(stats, "far_sight_reminders", far_sight_reminders_);
    cJSON_AddNumberToObject(stats, "far_sight_completed", far_sight_completed_);
    cJSON_AddNumberToObject(stats, "study_sessions", study_sessions_);
    cJSON_AddNumberToObject(stats, "absence_seconds", absence_seconds_);
    cJSON_AddItemToObject(root, "stats", stats);

    cJSON* rules = cJSON_CreateObject();
    cJSON_AddStringToObject(rules, "source", rules_.source.c_str());
    cJSON_AddNumberToObject(rules, "daily_knowledge_count", rules_.daily_knowledge_items.size());
    cJSON_AddNumberToObject(rules, "presence_ttl_seconds", rules_.presence_ttl_seconds);
    cJSON_AddNumberToObject(rules, "study_start_knowledge_delay_seconds", rules_.study_start_knowledge_delay_seconds);
    cJSON_AddNumberToObject(rules, "min_far_sight_reminder_gap_seconds", rules_.min_far_sight_reminder_gap_seconds);
    cJSON_AddNumberToObject(rules, "screening_reminder_interval_seconds", rules_.screening_reminder_interval_seconds);
    cJSON_AddNumberToObject(rules, "grip_tip_delay_seconds", rules_.grip_tip_delay_seconds);
    cJSON_AddNumberToObject(rules, "vision_chart_tip_delay_seconds", rules_.vision_chart_tip_delay_seconds);
    cJSON_AddNumberToObject(rules, "science_video_tip_delay_seconds", rules_.science_video_tip_delay_seconds);
    cJSON_AddItemToObject(root, "rules", rules);

    return root;
}

const char* EyeCareService::PickDailyKnowledge() const {
    if (rules_.daily_knowledge_items.empty()) {
        return "今日护眼小知识：每连续用眼一段时间，要抬头看看远处。";
    }
    return rules_.daily_knowledge_items[
        (uptime_seconds_ / kSecondsPerDay + study_sessions_) % rules_.daily_knowledge_items.size()
    ].c_str();
}

EyeCareService::Event EyeCareService::MakeEvent(EventType type, const RuleMessage& message) const {
    return {
        type,
        message.text,
        message.emotion,
        message.play_sound,
        message.display_ms,
    };
}

void EyeCareService::LoadRules() {
    ApplyRulesJson(
        eye_care_rules_json_start,
        eye_care_rules_json_end - eye_care_rules_json_start,
        "embedded:/eye_care/eye_care_rules.json");

    auto sdcard_rules = ReadTextFile("/sdcard/eye_care_rules.json");
    if (!sdcard_rules.empty()) {
        ApplyRulesJson(sdcard_rules.data(), sdcard_rules.size(), "/sdcard/eye_care_rules.json");
    }
}

void EyeCareService::ApplyRulesJson(const char* json, size_t length, const char* source) {
    if (json == nullptr || length == 0) {
        return;
    }

    cJSON* root = cJSON_ParseWithLength(json, length);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse eye-care rules from %s", source);
        return;
    }

    auto default_config = cJSON_GetObjectItem(root, "default_config");
    if (cJSON_IsObject(default_config)) {
        config_.enabled = JsonBool(default_config, "enabled", config_.enabled);
        config_.far_sight_enabled = JsonBool(default_config, "far_sight_enabled", config_.far_sight_enabled);
        config_.daily_knowledge_enabled = JsonBool(default_config, "daily_knowledge_enabled", config_.daily_knowledge_enabled);
        config_.posture_enabled = JsonBool(default_config, "posture_enabled", config_.posture_enabled);
        config_.screening_enabled = JsonBool(default_config, "screening_enabled", config_.screening_enabled);
        config_.grip_tip_enabled = JsonBool(default_config, "grip_tip_enabled", config_.grip_tip_enabled);
        config_.vision_chart_tip_enabled = JsonBool(default_config, "vision_chart_tip_enabled", config_.vision_chart_tip_enabled);
        config_.science_video_tip_enabled = JsonBool(default_config, "science_video_tip_enabled", config_.science_video_tip_enabled);
        config_.screen_video_enabled = JsonBool(default_config, "screen_video_enabled", config_.screen_video_enabled);
        config_.rest_music_enabled = JsonBool(default_config, "rest_music_enabled", config_.rest_music_enabled);
        config_.continuous_use_minutes = JsonInt(default_config, "continuous_use_minutes", config_.continuous_use_minutes);
        config_.far_sight_seconds = JsonInt(default_config, "far_sight_seconds", config_.far_sight_seconds);
        config_.absence_reset_seconds = JsonInt(default_config, "absence_reset_seconds", config_.absence_reset_seconds);
    }

    auto timing = cJSON_GetObjectItem(root, "timing");
    if (cJSON_IsObject(timing)) {
        rules_.presence_ttl_seconds = std::clamp(JsonInt(timing, "presence_ttl_seconds", rules_.presence_ttl_seconds), 1, 30);
        rules_.study_start_knowledge_delay_seconds = std::clamp(
            JsonInt(timing, "study_start_knowledge_delay_seconds", rules_.study_start_knowledge_delay_seconds),
            0,
            3600);
        rules_.min_far_sight_reminder_gap_seconds = std::clamp(
            JsonInt(timing, "min_far_sight_reminder_gap_seconds", rules_.min_far_sight_reminder_gap_seconds),
            30,
            7200);
        rules_.screening_reminder_interval_seconds = std::clamp(
            JsonInt(timing, "screening_reminder_interval_seconds", rules_.screening_reminder_interval_seconds),
            24 * 60 * 60,
            365 * 24 * 60 * 60);
        rules_.grip_tip_delay_seconds = std::clamp(
            JsonInt(timing, "grip_tip_delay_seconds", rules_.grip_tip_delay_seconds),
            10,
            7200);
        rules_.vision_chart_tip_delay_seconds = std::clamp(
            JsonInt(timing, "vision_chart_tip_delay_seconds", rules_.vision_chart_tip_delay_seconds),
            10,
            7200);
        rules_.science_video_tip_delay_seconds = std::clamp(
            JsonInt(timing, "science_video_tip_delay_seconds", rules_.science_video_tip_delay_seconds),
            10,
            7200);
    }

    auto parse_message = [](cJSON* messages, const char* key, RuleMessage& out) {
        auto item = cJSON_GetObjectItem(messages, key);
        if (!cJSON_IsObject(item)) {
            return;
        }
        out.text = JsonString(item, "text", out.text);
        out.emotion = JsonString(item, "emotion", out.emotion);
        out.play_sound = JsonBool(item, "play_sound", out.play_sound);
        out.display_ms = std::clamp(JsonInt(item, "display_ms", out.display_ms), 0, 120000);
    };

    auto messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_IsObject(messages)) {
        parse_message(messages, "study_started", rules_.study_started);
        parse_message(messages, "far_sight_reminder", rules_.far_sight_reminder);
        parse_message(messages, "far_sight_completed", rules_.far_sight_completed);
        parse_message(messages, "daily_knowledge", rules_.daily_knowledge);
        parse_message(messages, "screening_reminder", rules_.screening_reminder);
        parse_message(messages, "grip_posture_tip", rules_.grip_posture_tip);
        parse_message(messages, "vision_chart_tip", rules_.vision_chart_tip);
        parse_message(messages, "science_video_tip", rules_.science_video_tip);
    }

    auto daily_knowledge = cJSON_GetObjectItem(root, "daily_knowledge");
    if (cJSON_IsArray(daily_knowledge)) {
        std::vector<std::string> items;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, daily_knowledge) {
            if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
                items.emplace_back(item->valuestring);
            }
        }
        if (!items.empty()) {
            rules_.daily_knowledge_items = std::move(items);
        }
    }

    rules_.source = source ? source : "unknown";
    ClampConfig();
    ESP_LOGI(TAG, "Loaded eye-care rules from %s", rules_.source.c_str());
    cJSON_Delete(root);
}
