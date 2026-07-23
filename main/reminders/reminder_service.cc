#include "reminder_service.h"

#include "settings.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <cstdlib>

namespace {
const char* TAG = "ReminderService";
constexpr const char* kNvsKeyTasks = "tasks";
constexpr const char* kNvsKeyNextId = "next_id";
constexpr int64_t kKeepFiredSeconds = 24 * 60 * 60;

std::string ReminderTypeToString(ReminderService::ReminderType type) {
    switch (type) {
        case ReminderService::ReminderType::kRelative: return "relative";
        case ReminderService::ReminderType::kTomorrow: return "tomorrow";
        case ReminderService::ReminderType::kDate: return "date";
        case ReminderService::ReminderType::kNextWeekday: return "next_weekday";
        case ReminderService::ReminderType::kAbsolute: return "absolute";
    }
    return "absolute";
}

ReminderService::ReminderType ReminderTypeFromString(const char* value) {
    if (value == nullptr) return ReminderService::ReminderType::kAbsolute;
    std::string type(value);
    if (type == "relative") return ReminderService::ReminderType::kRelative;
    if (type == "tomorrow") return ReminderService::ReminderType::kTomorrow;
    if (type == "date") return ReminderService::ReminderType::kDate;
    if (type == "next_weekday") return ReminderService::ReminderType::kNextWeekday;
    return ReminderService::ReminderType::kAbsolute;
}

int WeekdayFromChinese(const std::string& value) {
    if (value == "一" || value == "1") return 1;
    if (value == "二" || value == "2") return 2;
    if (value == "三" || value == "3") return 3;
    if (value == "四" || value == "4") return 4;
    if (value == "五" || value == "5") return 5;
    if (value == "六" || value == "6") return 6;
    if (value == "日" || value == "天" || value == "0" || value == "7") return 0;
    return -1;
}

bool StartsWithAt(const std::string& text, size_t pos, const char* prefix) {
    return text.compare(pos, std::strlen(prefix), prefix) == 0;
}

void TrimAsciiAndPunctuation(std::string& text) {
    const char* prefixes[] = {" ", "\t", "\r", "\n", "，", ",", "。", ".", "：", ":"};
    bool changed = true;
    while (changed && !text.empty()) {
        changed = false;
        for (auto prefix : prefixes) {
            size_t len = std::strlen(prefix);
            if (text.size() >= len && text.compare(0, len, prefix) == 0) {
                text.erase(0, len);
                changed = true;
                break;
            }
        }
    }

    changed = true;
    while (changed && !text.empty()) {
        changed = false;
        for (auto suffix : prefixes) {
            size_t len = std::strlen(suffix);
            if (text.size() >= len && text.compare(text.size() - len, len, suffix) == 0) {
                text.erase(text.size() - len, len);
                changed = true;
                break;
            }
        }
    }
}

bool ReadNumber(const std::string& text, size_t& pos, int& value) {
    size_t start = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        pos++;
    }
    if (start == pos) {
        return false;
    }
    value = std::atoi(text.substr(start, pos - start).c_str());
    return true;
}

bool ConsumeToken(const std::string& text, size_t& pos, const std::vector<const char*>& tokens, std::string* matched = nullptr) {
    for (auto token : tokens) {
        size_t len = std::strlen(token);
        if (text.compare(pos, len, token) == 0) {
            pos += len;
            if (matched) {
                *matched = token;
            }
            return true;
        }
    }
    return false;
}

void SkipSpaces(std::string const& text, size_t& pos) {
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        pos++;
    }
}

bool ParseClockTime(const std::string& text, size_t& pos, const std::string& period, int default_hour, int& hour, int& minute) {
    SkipSpaces(text, pos);
    hour = default_hour;
    minute = 0;

    int parsed_hour = 0;
    if (ReadNumber(text, pos, parsed_hour)) {
        hour = parsed_hour;
        if (StartsWithAt(text, pos, "点")) {
            pos += std::strlen("点");
            int parsed_minute = 0;
            if (ReadNumber(text, pos, parsed_minute)) {
                minute = parsed_minute;
                if (StartsWithAt(text, pos, "分")) {
                    pos += std::strlen("分");
                }
            }
        } else if (StartsWithAt(text, pos, ":")) {
            pos++;
            int parsed_minute = 0;
            if (ReadNumber(text, pos, parsed_minute)) {
                minute = parsed_minute;
            }
        }
    }

    if (period == "下午" || period == "晚上") {
        if (hour < 12) hour += 12;
    } else if (period == "中午" && hour < 11) {
        hour += 12;
    }

    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

int DefaultHourForPeriod(const std::string& period) {
    if (period == "中午") return 12;
    if (period == "下午") return 15;
    if (period == "晚上") return 20;
    return 9;
}

std::string ReadPeriod(const std::string& text, size_t& pos) {
    std::string period;
    ConsumeToken(text, pos, {"上午", "下午", "晚上", "中午"}, &period);
    return period;
}

std::string TrimReminderContent(std::string text) {
    const char* prefixes[] = {"提醒我", "提醒一下我", "叫我", "让我"};
    for (auto prefix : prefixes) {
        auto pos = text.find(prefix);
        if (pos != std::string::npos) {
            text.erase(pos, std::string(prefix).size());
        }
    }
    const char* fillers[] = {"一下", "到时候", "的时候", "时"};
    for (auto filler : fillers) {
        auto pos = text.find(filler);
        if (pos != std::string::npos) {
            text.erase(pos, std::string(filler).size());
        }
    }
    TrimAsciiAndPunctuation(text);
    return text.empty() ? "提醒时间到了" : text;
}

time_t MakeLocalTime(int year, int month, int day, int hour, int minute, int second) {
    struct tm tm_time = {};
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = month - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = minute;
    tm_time.tm_sec = second;
    tm_time.tm_isdst = -1;
    return mktime(&tm_time);
}
}  // namespace

ReminderService::ReminderService() {
    Load();
}

bool ReminderService::AddFromText(const std::string& text, ReminderTask& out) {
    if (!ParseText(text, out)) {
        return false;
    }
    out.id = NextId();
    tasks_.push_back(out);
    Save();
    ESP_LOGI(TAG, "Add reminder from text id=%lu at=%lld content=%s",
             (unsigned long)out.id, (long long)out.remind_at, out.content.c_str());
    return true;
}

bool ReminderService::AddTask(ReminderType type, int64_t remind_at, const std::string& content, const std::string& original_text, ReminderTask& out) {
    if (remind_at <= time(nullptr) || content.empty()) {
        return false;
    }
    out = {};
    out.id = NextId();
    out.type = type;
    out.remind_at = remind_at;
    out.content = content;
    out.original_text = original_text;
    tasks_.push_back(out);
    Save();
    return true;
}

bool ReminderService::DeleteTask(uint32_t id) {
    auto before = tasks_.size();
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [id](const ReminderTask& task) {
        return task.id == id;
    }), tasks_.end());
    if (tasks_.size() == before) {
        return false;
    }
    Save();
    return true;
}

std::vector<ReminderService::DueReminder> ReminderService::PopDueReminders(int64_t now) {
    std::vector<DueReminder> due;
    for (auto& task : tasks_) {
        if (!task.fired && task.remind_at <= now) {
            task.fired = true;
            due.push_back({task.id, task.content, task.remind_at});
        }
    }
    if (!due.empty()) {
        RemoveExpiredFiredTasks(now);
        Save();
    }
    return due;
}

cJSON* ReminderService::ToJson() const {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "now", (double)time(nullptr));
    cJSON_AddNumberToObject(root, "next_id", next_id_);
    cJSON* tasks = cJSON_CreateArray();
    for (const auto& task : tasks_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", task.id);
        cJSON_AddStringToObject(item, "type", ReminderTypeToString(task.type).c_str());
        cJSON_AddNumberToObject(item, "remind_at", (double)task.remind_at);
        cJSON_AddStringToObject(item, "content", task.content.c_str());
        cJSON_AddStringToObject(item, "original_text", task.original_text.c_str());
        cJSON_AddBoolToObject(item, "fired", task.fired);
        cJSON_AddItemToArray(tasks, item);
    }
    cJSON_AddItemToObject(root, "tasks", tasks);
    return root;
}

void ReminderService::Load() {
    Settings settings("reminders", false);
    next_id_ = std::max<int32_t>(1, settings.GetInt(kNvsKeyNextId, 1));
    std::string data = settings.GetString(kNvsKeyTasks, "");
    if (data.empty()) {
        return;
    }

    cJSON* root = cJSON_Parse(data.c_str());
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse reminders from NVS");
        return;
    }

    cJSON* tasks = cJSON_GetObjectItem(root, "tasks");
    if (cJSON_IsArray(tasks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tasks) {
            ReminderTask task;
            auto id = cJSON_GetObjectItem(item, "id");
            auto type = cJSON_GetObjectItem(item, "type");
            auto remind_at = cJSON_GetObjectItem(item, "remind_at");
            auto content = cJSON_GetObjectItem(item, "content");
            auto original_text = cJSON_GetObjectItem(item, "original_text");
            auto fired = cJSON_GetObjectItem(item, "fired");
            if (!cJSON_IsNumber(id) || !cJSON_IsNumber(remind_at) || !cJSON_IsString(content)) {
                continue;
            }
            task.id = id->valueint;
            task.type = cJSON_IsString(type) ? ReminderTypeFromString(type->valuestring) : ReminderType::kAbsolute;
            task.remind_at = (int64_t)remind_at->valuedouble;
            task.content = content->valuestring;
            task.original_text = cJSON_IsString(original_text) ? original_text->valuestring : "";
            task.fired = cJSON_IsBool(fired) && fired->valueint == 1;
            tasks_.push_back(task);
            next_id_ = std::max(next_id_, task.id + 1);
        }
    }
    cJSON_Delete(root);
    RemoveExpiredFiredTasks(time(nullptr));
}

void ReminderService::Save() const {
    cJSON* root = ToJson();
    char* json_str = cJSON_PrintUnformatted(root);
    Settings settings("reminders", true);
    settings.SetString(kNvsKeyTasks, json_str ? json_str : "{}");
    settings.SetInt(kNvsKeyNextId, next_id_);
    cJSON_free(json_str);
    cJSON_Delete(root);
}

uint32_t ReminderService::NextId() {
    return next_id_++;
}

void ReminderService::RemoveExpiredFiredTasks(int64_t now) {
    tasks_.erase(std::remove_if(tasks_.begin(), tasks_.end(), [now](const ReminderTask& task) {
        return task.fired && now - task.remind_at > kKeepFiredSeconds;
    }), tasks_.end());
}

bool ReminderService::ParseText(const std::string& text, ReminderTask& out) const {
    const time_t now = time(nullptr);
    struct tm local_now = {};
    localtime_r(&now, &local_now);

    size_t pos = 0;
    int value = 0;
    if (ReadNumber(text, pos, value)) {
        std::string unit;
        if (ConsumeToken(text, pos, {"分钟", "分", "个小时", "小时", "天", "日"}, &unit)) {
            if (StartsWithAt(text, pos, "后")) {
                pos += std::strlen("后");
            }
            int64_t seconds = value * 60;
            if (unit == "个小时" || unit == "小时") {
                seconds = value * 60 * 60;
            } else if (unit == "天" || unit == "日") {
                seconds = value * 24 * 60 * 60;
            }

            out = {};
            out.type = ReminderType::kRelative;
            out.remind_at = now + seconds;
            out.original_text = text;
            out.content = TrimReminderContent(text.substr(pos));
            return out.remind_at > now;
        }
    }

    if (StartsWithAt(text, 0, "明天")) {
        pos = std::strlen("明天");
        std::string period = ReadPeriod(text, pos);
        int hour = DefaultHourForPeriod(period);
        int minute = 0;
        if (!ParseClockTime(text, pos, period, hour, hour, minute)) {
            return false;
        }
        local_now.tm_mday += 1;
        out = {};
        out.type = ReminderType::kTomorrow;
        out.remind_at = MakeLocalTime(local_now.tm_year + 1900, local_now.tm_mon + 1, local_now.tm_mday, hour, minute, 0);
        out.original_text = text;
        out.content = TrimReminderContent(text.substr(pos));
        return out.remind_at > now;
    }

    pos = 0;
    int month = 0;
    int day = 0;
    if (ReadNumber(text, pos, month) && StartsWithAt(text, pos, "月")) {
        pos += std::strlen("月");
        if (!ReadNumber(text, pos, day)) {
            return false;
        }
        if (StartsWithAt(text, pos, "日") || StartsWithAt(text, pos, "号")) {
            pos += std::strlen("日");
        } else {
            return false;
        }
        std::string period = ReadPeriod(text, pos);
        int hour = DefaultHourForPeriod(period);
        int minute = 0;
        if (!ParseClockTime(text, pos, period, hour, hour, minute)) {
            return false;
        }
        int year = local_now.tm_year + 1900;
        time_t target = MakeLocalTime(year, month, day, hour, minute, 0);
        if (target <= now) {
            target = MakeLocalTime(year + 1, month, day, hour, minute, 0);
        }
        out = {};
        out.type = ReminderType::kDate;
        out.remind_at = target;
        out.original_text = text;
        out.content = TrimReminderContent(text.substr(pos));
        return out.remind_at > now;
    }

    if (StartsWithAt(text, 0, "下周")) {
        pos = std::strlen("下周");
        std::string weekday;
        if (!ConsumeToken(text, pos, {"一", "二", "三", "四", "五", "六", "日", "天", "0", "1", "2", "3", "4", "5", "6", "7"}, &weekday)) {
            return false;
        }
        int target_wday = WeekdayFromChinese(weekday);
        if (target_wday < 0) {
            return false;
        }
        int days_until_next_week = 7 - local_now.tm_wday + target_wday;
        if (days_until_next_week <= 0) days_until_next_week += 7;
        std::string period = ReadPeriod(text, pos);
        int hour = DefaultHourForPeriod(period);
        int minute = 0;
        if (!ParseClockTime(text, pos, period, hour, hour, minute)) {
            return false;
        }
        local_now.tm_mday += days_until_next_week;
        out = {};
        out.type = ReminderType::kNextWeekday;
        out.remind_at = MakeLocalTime(local_now.tm_year + 1900, local_now.tm_mon + 1, local_now.tm_mday, hour, minute, 0);
        out.original_text = text;
        out.content = TrimReminderContent(text.substr(pos));
        return out.remind_at > now;
    }

    return false;
}
