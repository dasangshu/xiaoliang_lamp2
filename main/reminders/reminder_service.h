#pragma once

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <vector>

class ReminderService {
public:
    enum class ReminderType {
        kRelative,
        kTomorrow,
        kDate,
        kNextWeekday,
        kAbsolute,
    };

    struct ReminderTask {
        uint32_t id = 0;
        ReminderType type = ReminderType::kAbsolute;
        int64_t remind_at = 0;
        std::string content;
        std::string original_text;
        bool fired = false;
    };

    struct DueReminder {
        uint32_t id = 0;
        std::string content;
        int64_t remind_at = 0;
    };

    ReminderService();

    bool AddFromText(const std::string& text, ReminderTask& out);
    bool AddTask(ReminderType type, int64_t remind_at, const std::string& content, const std::string& original_text, ReminderTask& out);
    bool DeleteTask(uint32_t id);
    std::vector<DueReminder> PopDueReminders(int64_t now);
    cJSON* ToJson() const;

private:
    void Load();
    void Save() const;
    uint32_t NextId();
    void RemoveExpiredFiredTasks(int64_t now);
    bool ParseText(const std::string& text, ReminderTask& out) const;

    std::vector<ReminderTask> tasks_;
    uint32_t next_id_ = 1;
};
