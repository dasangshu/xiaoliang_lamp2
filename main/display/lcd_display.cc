#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "apps/app_registry.h"
#include "apps/app_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <string>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <src/misc/cache/lv_cache.h>
#include <time.h>

#include "board.h"
#include "boards/common/wifi_board.h"
#include "application.h"
#include "mjpeg_player/mjpeg_player_port.h"

#define TAG "LcdDisplay"
// Hosted Wi-Fi must use the board's ESP32-C5/SDIO profile. With that transport
// restored, MJPEG emotions can run normally again.
#define DISABLE_MJPEG_EMOTIONS 0

namespace {
constexpr uint32_t kSurfaceBg = app_ui::kCanvas;
constexpr uint32_t kSurfaceCard = app_ui::kSurface;
constexpr uint32_t kTextPrimary = app_ui::kInk;
constexpr uint32_t kTextSecondary = app_ui::kInkMuted;
constexpr uint32_t kBorderSoft = app_ui::kLine;
constexpr lv_coord_t kPagePad = app_ui::kPagePadding;
constexpr lv_coord_t kCardRadius = app_ui::kCardRadius;

void CreateProductBottomNav(lv_obj_t* parent, const lv_font_t* text_font,
                            const lv_font_t* icon_font, int active_index = 1) {
    // Product applications are launched from a dedicated app list. A second
    // four-item navigation bar consumes valuable vertical space and duplicates
    // navigation, so detail pages return through their header instead.
    (void)parent;
    (void)text_font;
    (void)icon_font;
    (void)active_index;
    return;

#if 0
    lv_obj_t* nav = lv_obj_create(parent);
    lv_obj_set_size(nav, LV_HOR_RES, 66);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(nav, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(nav, 0, 0);
    lv_obj_set_style_bg_color(nav, lv_color_hex(app_ui::kSurface), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nav, 1, 0);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(nav, lv_color_hex(app_ui::kLine), 0);
    lv_obj_set_style_pad_all(nav, 5, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char* icons[] = {FONT_AWESOME_HOUSE, FONT_AWESOME_STAR,
                           FONT_AWESOME_COMPASS, FONT_AWESOME_USER};
    const char* labels[] = {"首页", "应用", "发现", "我的"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* item = lv_obj_create(nav);
        lv_obj_set_size(item, 72, 54);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* icon = lv_label_create(item);
        lv_label_set_text(icon, icons[i]);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(i == active_index ? app_ui::kBrand : app_ui::kInkMuted), 0);
        lv_obj_t* label = lv_label_create(item);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(i == active_index ? app_ui::kBrand : app_ui::kInkMuted), 0);
    }
    lv_obj_move_foreground(nav);
#endif
}

bool FileExists(const char* path) {
    struct stat st;
    return path != nullptr && stat(path, &st) == 0;
}

bool ReadTextFile(const char* path, std::string& out) {
    out.clear();
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr) {
        return false;
    }
    char buffer[512];
    size_t read_len = 0;
    while ((read_len = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        out.append(buffer, read_len);
    }
    fclose(fp);
    return true;
}

struct MusicSceneItem {
    std::string scene;
    std::string track_name;
    std::string path;
    int track_count = 0;
    int duration_seconds = 0;
};

struct AiScenarioItem {
    const char* title;
    const char* description;
    const char* category;
    const char* icon;
    const char* command;
    const char* prompt;
};

enum class PomodoroMode {
    kFocus = 0,
    kShortBreak,
    kLongBreak,
};

enum class PomodoroState {
    kIdle = 0,
    kRunning,
    kPaused,
    kFinished,
};

struct PomodoroSession {
    PomodoroMode mode = PomodoroMode::kFocus;
    PomodoroState state = PomodoroState::kIdle;
    uint32_t total_seconds = 25 * 60;
    uint32_t remaining_seconds = 25 * 60;
    uint16_t completed_today = 0;
    uint32_t focus_seconds_today = 0;
    time_t last_tick = 0;
};

PomodoroSession g_pomodoro;
lv_obj_t* g_pomodoro_time_label = nullptr;
lv_obj_t* g_pomodoro_state_label = nullptr;
lv_obj_t* g_pomodoro_start_label = nullptr;
lv_obj_t* g_pomodoro_done_label = nullptr;
lv_obj_t* g_pomodoro_minutes_label = nullptr;
lv_obj_t* g_pomodoro_arc = nullptr;
lv_obj_t* g_pomodoro_mode_buttons[3] = {nullptr, nullptr, nullptr};
int g_eye_height_cm = 170;
lv_obj_t* g_eye_height_label = nullptr;
lv_obj_t* g_eye_seat_label = nullptr;
lv_obj_t* g_eye_desk_label = nullptr;
lv_obj_t* g_eye_height_input_layer = nullptr;
lv_obj_t* g_eye_height_textarea = nullptr;
uint8_t g_pet_daily_mask = 0;
int g_pet_growth = 0;
int g_pet_streak = 0;
lv_obj_t* g_pet_progress_label = nullptr;
lv_obj_t* g_pet_growth_label = nullptr;
lv_obj_t* g_pet_habit_cards[3] = {nullptr, nullptr, nullptr};
lv_obj_t* g_pet_habit_states[3] = {nullptr, nullptr, nullptr};

void UpdateEyeErgonomicsUi() {
    char text[32];
    if (g_eye_height_label != nullptr) {
        snprintf(text, sizeof(text), "%d cm", g_eye_height_cm);
        lv_label_set_text(g_eye_height_label, text);
    }
    if (g_eye_seat_label != nullptr) {
        snprintf(text, sizeof(text), "座椅 %d cm", (g_eye_height_cm * 25 + 50) / 100);
        lv_label_set_text(g_eye_seat_label, text);
    }
    if (g_eye_desk_label != nullptr) {
        snprintf(text, sizeof(text), "桌面 %d cm", (g_eye_height_cm * 41 + 50) / 100);
        lv_label_set_text(g_eye_desk_label, text);
    }
}

void OnEyeHeightChanged(lv_event_t* event) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    g_eye_height_cm = std::clamp(g_eye_height_cm + delta, 100, 220);
    Settings settings("eye_ui", true);
    settings.SetInt("height_cm", g_eye_height_cm);
    UpdateEyeErgonomicsUi();
}

void CloseEyeHeightInput() {
    if (g_eye_height_input_layer != nullptr) {
        lv_obj_del(g_eye_height_input_layer);
        g_eye_height_input_layer = nullptr;
        g_eye_height_textarea = nullptr;
    }
}

void OnEyeHeightKeyClicked(lv_event_t* event) {
    if (g_eye_height_textarea == nullptr) {
        return;
    }
    const char* key = static_cast<const char*>(lv_event_get_user_data(event));
    if (strcmp(key, "OK") == 0) {
        const char* value = lv_textarea_get_text(g_eye_height_textarea);
        const int entered_height = value != nullptr && value[0] != '\0' ? atoi(value) : g_eye_height_cm;
        g_eye_height_cm = std::clamp(entered_height, 100, 220);
        Settings settings("eye_ui", true);
        settings.SetInt("height_cm", g_eye_height_cm);
        CloseEyeHeightInput();
        UpdateEyeErgonomicsUi();
    } else if (strcmp(key, "DEL") == 0) {
        lv_textarea_delete_char(g_eye_height_textarea);
    } else if (strlen(lv_textarea_get_text(g_eye_height_textarea)) < 3) {
        lv_textarea_add_text(g_eye_height_textarea, key);
    }
}

void OnEyeHeightInputClicked(lv_event_t* event) {
    (void)event;
    if (g_eye_height_input_layer != nullptr) {
        return;
    }
    g_eye_height_input_layer = lv_obj_create(lv_screen_active());
    lv_obj_set_size(g_eye_height_input_layer, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(g_eye_height_input_layer, 0, 0);
    lv_obj_set_style_radius(g_eye_height_input_layer, 0, 0);
    lv_obj_set_style_bg_color(g_eye_height_input_layer, lv_color_hex(0x172033), 0);
    lv_obj_set_style_bg_opa(g_eye_height_input_layer, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_eye_height_input_layer, 0, 0);
    lv_obj_set_style_pad_all(g_eye_height_input_layer, 0, 0);
    lv_obj_clear_flag(g_eye_height_input_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(g_eye_height_input_layer);
    lv_obj_set_size(panel, 360, 150);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_radius(panel, 22, 0);
    lv_obj_set_style_bg_color(panel, lv_color_white(), 0);
    lv_obj_set_style_border_width(panel, 0, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, "输入身高（厘米）");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    g_eye_height_textarea = lv_textarea_create(panel);
    lv_obj_set_size(g_eye_height_textarea, 250, 62);
    lv_obj_align(g_eye_height_textarea, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_textarea_set_one_line(g_eye_height_textarea, true);
    lv_textarea_set_max_length(g_eye_height_textarea, 3);
    lv_textarea_set_accepted_chars(g_eye_height_textarea, "0123456789");
    char height_text[8];
    snprintf(height_text, sizeof(height_text), "%d", g_eye_height_cm);
    lv_textarea_set_placeholder_text(g_eye_height_textarea, height_text);
    lv_textarea_set_text(g_eye_height_textarea, "");
    lv_obj_set_style_text_align(g_eye_height_textarea, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* keypad = lv_obj_create(g_eye_height_input_layer);
    lv_obj_set_size(keypad, LV_HOR_RES - 24, 330);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(keypad, 22, 0);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0xF5F7F8), 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 10, 0);
    lv_obj_clear_flag(keypad, LV_OBJ_FLAG_SCROLLABLE);

    static const char* keys[] = {"1", "2", "3", "4", "5", "6",
                                 "7", "8", "9", "DEL", "0", "OK"};
    static const char* labels[] = {"1", "2", "3", "4", "5", "6",
                                   "7", "8", "9", "删除", "0", "确认"};
    constexpr int key_w = 136;
    constexpr int key_h = 68;
    constexpr int key_gap = 8;
    for (int i = 0; i < 12; ++i) {
        lv_obj_t* button = lv_obj_create(keypad);
        lv_obj_set_size(button, key_w, key_h);
        lv_obj_set_pos(button, (i % 3) * (key_w + key_gap), (i / 3) * (key_h + key_gap));
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(i == 11 ? 0x45BE72 : 0xFFFFFF), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xE1E7E4), 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, OnEyeHeightKeyClicked, LV_EVENT_CLICKED,
                            const_cast<char*>(keys[i]));
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(i == 11 ? 0xFFFFFF : 0x27322F), 0);
        lv_obj_center(label);
    }
    lv_obj_move_foreground(g_eye_height_input_layer);
}

void UpdatePetHabitUi() {
    int completed = 0;
    for (int i = 0; i < 3; ++i) {
        const bool done = (g_pet_daily_mask & (1U << i)) != 0;
        completed += done ? 1 : 0;
        if (g_pet_habit_cards[i] != nullptr) {
            lv_obj_set_style_bg_color(g_pet_habit_cards[i],
                                      lv_color_hex(done ? 0xE8F7EC : 0xFFFFFF), 0);
            lv_obj_set_style_border_color(g_pet_habit_cards[i],
                                          lv_color_hex(done ? 0x70C98A : 0xE5DDD3), 0);
        }
        if (g_pet_habit_states[i] != nullptr) {
            lv_label_set_text(g_pet_habit_states[i], done ? "已完成" : "去完成");
            lv_obj_set_style_text_color(g_pet_habit_states[i],
                                        lv_color_hex(done ? 0x2E9F62 : 0x9A7451), 0);
        }
    }
    char text[48];
    if (g_pet_progress_label != nullptr) {
        snprintf(text, sizeof(text), "今日完成 %d / 3  ·  连续 %d 天", completed, g_pet_streak);
        lv_label_set_text(g_pet_progress_label, text);
    }
    if (g_pet_growth_label != nullptr) {
        snprintf(text, sizeof(text), "成长值 %d", g_pet_growth);
        lv_label_set_text(g_pet_growth_label, text);
    }
}

void OnPetHabitClicked(lv_event_t* event) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    if (index < 0 || index >= 3 || (g_pet_daily_mask & (1U << index)) != 0) {
        return;
    }
    g_pet_daily_mask |= (1U << index);
    g_pet_growth += 10;
    Settings settings("pet_habit", true);
    settings.SetInt("daily_mask", g_pet_daily_mask);
    settings.SetInt("growth", g_pet_growth);

    if ((g_pet_daily_mask & 0x07) == 0x07) {
        const int32_t today = static_cast<int32_t>(time(nullptr) / 86400);
        const int32_t last_complete = settings.GetInt("last_done_day", -2);
        if (last_complete != today) {
            g_pet_streak = last_complete == today - 1 ? g_pet_streak + 1 : 1;
            settings.SetInt("streak", g_pet_streak);
            settings.SetInt("last_done_day", today);
            g_pet_growth += 20;
            settings.SetInt("growth", g_pet_growth);
        }
    }
    UpdatePetHabitUi();
}

uint32_t PomodoroSecondsForMode(PomodoroMode mode) {
    switch (mode) {
        case PomodoroMode::kShortBreak:
            return 5 * 60;
        case PomodoroMode::kLongBreak:
            return 15 * 60;
        case PomodoroMode::kFocus:
        default:
            return 25 * 60;
    }
}

const char* PomodoroModeName(PomodoroMode mode) {
    switch (mode) {
        case PomodoroMode::kShortBreak:
            return "短休";
        case PomodoroMode::kLongBreak:
            return "长休";
        case PomodoroMode::kFocus:
        default:
            return "专注";
    }
}

const char* PomodoroStateName(PomodoroState state) {
    switch (state) {
        case PomodoroState::kRunning:
            return "进行中";
        case PomodoroState::kPaused:
            return "已暂停";
        case PomodoroState::kFinished:
            return "已完成";
        case PomodoroState::kIdle:
        default:
            return "准备好";
    }
}

void PomodoroResetForMode(PomodoroMode mode) {
    g_pomodoro.mode = mode;
    g_pomodoro.state = PomodoroState::kIdle;
    g_pomodoro.total_seconds = PomodoroSecondsForMode(mode);
    g_pomodoro.remaining_seconds = g_pomodoro.total_seconds;
    g_pomodoro.last_tick = time(nullptr);
}

void PomodoroApplyElapsed() {
    if (g_pomodoro.state != PomodoroState::kRunning) {
        g_pomodoro.last_tick = time(nullptr);
        return;
    }

    time_t now = time(nullptr);
    if (g_pomodoro.last_tick == 0) {
        g_pomodoro.last_tick = now;
        return;
    }

    uint32_t elapsed = now > g_pomodoro.last_tick ? (uint32_t)(now - g_pomodoro.last_tick) : 0;
    if (elapsed == 0) {
        return;
    }
    g_pomodoro.last_tick = now;

    if (elapsed >= g_pomodoro.remaining_seconds) {
        if (g_pomodoro.mode == PomodoroMode::kFocus) {
            g_pomodoro.focus_seconds_today += g_pomodoro.remaining_seconds;
            g_pomodoro.completed_today++;
        }
        g_pomodoro.remaining_seconds = 0;
        g_pomodoro.state = PomodoroState::kFinished;
    } else {
        g_pomodoro.remaining_seconds -= elapsed;
        if (g_pomodoro.mode == PomodoroMode::kFocus) {
            g_pomodoro.focus_seconds_today += elapsed;
        }
    }
}

void UpdatePomodoroUi() {
    PomodoroApplyElapsed();
    char text[40];
    if (g_pomodoro_time_label != nullptr) {
        snprintf(text, sizeof(text), "%02lu:%02lu",
                 (unsigned long)(g_pomodoro.remaining_seconds / 60),
                 (unsigned long)(g_pomodoro.remaining_seconds % 60));
        lv_label_set_text(g_pomodoro_time_label, text);
    }
    if (g_pomodoro_state_label != nullptr) {
        lv_label_set_text(g_pomodoro_state_label, PomodoroStateName(g_pomodoro.state));
    }
    if (g_pomodoro_start_label != nullptr) {
        const bool running = g_pomodoro.state == PomodoroState::kRunning;
        lv_label_set_text(g_pomodoro_start_label, running ? "暂停" : "开始");
    }
    if (g_pomodoro_done_label != nullptr) {
        snprintf(text, sizeof(text), "%u / 8", g_pomodoro.completed_today);
        lv_label_set_text(g_pomodoro_done_label, text);
    }
    if (g_pomodoro_minutes_label != nullptr) {
        snprintf(text, sizeof(text), "%lu 分钟", (unsigned long)(g_pomodoro.focus_seconds_today / 60));
        lv_label_set_text(g_pomodoro_minutes_label, text);
    }
    if (g_pomodoro_arc != nullptr) {
        uint32_t total = g_pomodoro.total_seconds == 0 ? 1 : g_pomodoro.total_seconds;
        lv_arc_set_range(g_pomodoro_arc, 0, total);
        lv_arc_set_value(g_pomodoro_arc, total - g_pomodoro.remaining_seconds);
    }
    for (int i = 0; i < 3; ++i) {
        if (g_pomodoro_mode_buttons[i] == nullptr) {
            continue;
        }
        const bool active = i == static_cast<int>(g_pomodoro.mode);
        lv_obj_set_style_bg_color(g_pomodoro_mode_buttons[i],
                                  active ? lv_color_hex(app_ui::kBrand) : lv_color_hex(0xF2F5F8), 0);
        lv_obj_set_style_bg_grad_color(g_pomodoro_mode_buttons[i],
                                       active ? lv_color_hex(app_ui::kHighlight) : lv_color_hex(0xF2F5F8), 0);
        lv_obj_set_style_bg_grad_dir(g_pomodoro_mode_buttons[i],
                                     active ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_width(g_pomodoro_mode_buttons[i], active ? 1 : 0, 0);
        lv_obj_set_style_border_color(g_pomodoro_mode_buttons[i], lv_color_hex(0xA7DEB5), 0);
        lv_obj_set_style_shadow_width(g_pomodoro_mode_buttons[i], 0, 0);
        lv_obj_set_style_text_color(g_pomodoro_mode_buttons[i],
                                    active ? lv_color_white() : lv_color_hex(kTextSecondary), 0);
        if (lv_obj_get_child_cnt(g_pomodoro_mode_buttons[i]) > 0) {
            lv_obj_t* child = lv_obj_get_child(g_pomodoro_mode_buttons[i], 0);
            lv_obj_set_style_text_color(child, active ? lv_color_white() : lv_color_hex(kTextSecondary), 0);
        }
    }
}

const char* MusicSceneIcon(const std::string& scene) {
    if (scene.find("睡") != std::string::npos) {
        return FONT_AWESOME_MOON;
    }
    if (scene.find("雨") != std::string::npos) {
        return FONT_AWESOME_CLOUD_RAIN;
    }
    if (scene.find("自然") != std::string::npos) {
        return FONT_AWESOME_CLOUD_SUN;
    }
    if (scene.find("专注") != std::string::npos || scene.find("冥想") != std::string::npos) {
        return FONT_AWESOME_HEADPHONES;
    }
    return FONT_AWESOME_MUSIC;
}

std::vector<MusicSceneItem> LoadMusicScenesFromManifest(int* track_count, int* parse_status) {
    if (track_count != nullptr) {
        *track_count = 0;
    }
    if (parse_status != nullptr) {
        *parse_status = 0;
    }

    std::string json;
    if (!ReadTextFile("/sdcard/yinyuehe/manifest.json", json)) {
        if (parse_status != nullptr) {
            *parse_status = -1;
        }
        return {};
    }

    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) {
        if (parse_status != nullptr) {
            *parse_status = -2;
        }
        return {};
    }

    cJSON* tracks = cJSON_GetObjectItem(root, "tracks");
    if (tracks == nullptr) {
        tracks = cJSON_GetObjectItem(root, "items");
    }
    std::vector<MusicSceneItem> scenes;
    if (!cJSON_IsArray(tracks)) {
        cJSON_Delete(root);
        return scenes;
    }

    const int count = cJSON_GetArraySize(tracks);
    if (track_count != nullptr) {
        *track_count = count;
    }

    cJSON* track = nullptr;
    cJSON_ArrayForEach(track, tracks) {
        cJSON* name = cJSON_GetObjectItem(track, "name");
        cJSON* display_name = cJSON_GetObjectItem(track, "display_name");
        cJSON* path = cJSON_GetObjectItem(track, "sdcard_path");
        cJSON* duration = cJSON_GetObjectItem(track, "duration_seconds");
        if (!cJSON_IsString(path) || path->valuestring == nullptr) {
            continue;
        }

        MusicSceneItem item;
        if (cJSON_IsString(name) && name->valuestring != nullptr) {
            item.scene = name->valuestring;
        } else if (cJSON_IsString(display_name) && display_name->valuestring != nullptr) {
            item.scene = display_name->valuestring;
        } else {
            item.scene = "主题场景";
        }
        if (cJSON_IsString(display_name) && display_name->valuestring != nullptr) {
            item.track_name = display_name->valuestring;
        } else {
            item.track_name = item.scene;
        }
        item.path = path->valuestring;
        item.track_count = 1;
        item.duration_seconds = cJSON_IsNumber(duration) ? duration->valueint : 0;
        scenes.push_back(std::move(item));
    }
    cJSON_Delete(root);

    std::sort(scenes.begin(), scenes.end(), [](const MusicSceneItem& a, const MusicSceneItem& b) {
        return a.scene < b.scene;
    });
    return scenes;
}

constexpr const char* kPetSettingsNamespace = "pet";
constexpr const char* kPetStyleKey = "style";
constexpr const char* kDefaultPetStyle = "jinglingshu";

std::string CurrentPetStyleName() {
    Settings settings(kPetSettingsNamespace, false);
    auto style = settings.GetString(kPetStyleKey, kDefaultPetStyle);
    return style.empty() ? kDefaultPetStyle : style;
}

bool BuildStyleMjpegPath(const char* filename, char* path, size_t path_size) {
    if (filename == nullptr || filename[0] == '\0' || path == nullptr || path_size == 0) {
        return false;
    }
    auto style = CurrentPetStyleName();
    snprintf(path, path_size, "/sdcard/style/%s/%s", style.c_str(), filename);
    return FileExists(path);
}

[[maybe_unused]] bool ResolveMjpegEmotionPath(const char* emotion, char* path, size_t path_size) {
    if (emotion == nullptr || emotion[0] == '\0' || path == nullptr || path_size == 0) {
        return false;
    }

    if (strstr(emotion, ".mjpeg") != nullptr) {
        if (emotion[0] == '/') {
            snprintf(path, path_size, "%s", emotion);
            return FileExists(path);
        }
        if (BuildStyleMjpegPath(emotion, path, path_size)) {
            return true;
        }
        return false;
    }

    char filename[80];
    snprintf(filename, sizeof(filename), "%s.mjpeg", emotion);
    if (BuildStyleMjpegPath(filename, path, path_size)) {
        return true;
    }

    if (strcmp(emotion, "confused") == 0) {
        if (BuildStyleMjpegPath("confuesed.mjpeg", path, path_size)) {
            return true;
        }
    }

    return false;
}

using XiaoliangAppModule = app_ui::AppModule;
using AppKind = app_ui::AppKind;

struct PetSkinPreset {
    const char* style_id;
    const char* title;
    const char* subtitle;
    const char* directory;
};

const auto* kXiaoliangModules = app_ui::GetAppModules();
const size_t kXiaoliangModuleCount = app_ui::GetAppModuleCount();

const XiaoliangAppModule* FindModule(AppKind kind) {
    return app_ui::FindAppModule(kind);
}

using app_ui::kDeviceActions;
using app_ui::kEyeIslandActions;
using app_ui::kHealthActions;
using app_ui::kMoreActions;

const PetSkinPreset kPetSkinPresets[] = {
    {"jinglingshu", "精灵鼠", "当前 SD 卡皮肤", "/sdcard/style/jinglingshu"},
    {"xiaotu", "小兔星球", "待安装皮肤", "/sdcard/style/xiaotu"},
    {"konglong", "小恐龙", "待安装皮肤", "/sdcard/style/konglong"},
};

const char* const kEyeMetrics[] = {"护眼分 86", "远眺 4次", "坐姿 2次"};
const char* const kEyeDescriptions[] = {
    "每天 6 条儿童护眼知识，支持翻页展示。",
    "检查周期、上次记录和预约状态先用演示数据展示。",
    "读取 EyeCareService 数据，缺失时展示今日模拟报告。",
};
const char* const kEyeIcons[] = {FONT_AWESOME_GLASSES, FONT_AWESOME_CALENDAR, FONT_AWESOME_CIRCLE_CHECK};

const char* const kMusicMetrics[] = {"推荐 6首", "睡眠 5首", "自然 9首"};
const char* const kMusicDescriptions[] = {
    "根据 manifest 场景展示推荐歌单。",
    "雨声、海浪、鸟鸣、森林风等关键词匹配。",
    "播放、暂停、上一首、下一首和循环控制入口。",
};
const char* const kMusicIcons[] = {FONT_AWESOME_STAR, FONT_AWESOME_MUSIC, FONT_AWESOME_PLAY};

const char* const kAiMetrics[] = {"场景 6个", "练习 12轮", "星星 28颗"};
const char* const kAiDescriptions[] = {
    "日常问候、英语练习、看图说话、睡前故事等场景。",
    "展示目标、推荐话术和开始对话按钮。",
    "完成后显示轮次、完成度和鼓励反馈。",
};
const char* const kAiIcons[] = {FONT_AWESOME_COMMENT, FONT_AWESOME_COMMENT_QUESTION, FONT_AWESOME_STAR};

const AiScenarioItem kAiScenarios[] = {
    {"和李白聊古诗", "诗人角色对话，聊月亮、山水和想象。", "角色对话", FONT_AWESOME_STAR,
     "李白古诗",
     "你现在扮演李白，用适合儿童理解的中文和我聊天。围绕古诗、想象、月亮、山水展开；每次回答不超过60字，并主动问我一个问题。"},
    {"和孙悟空聊天", "神话角色对话，训练表达和追问。", "角色对话", FONT_AWESOME_GAMEPAD,
     "孙悟空聊天",
     "你现在扮演孙悟空，语气活泼勇敢，但不要吓人。和我聊冒险、朋友、勇气和解决问题；每次回答不超过60字，并问我下一步想怎么做。"},
    {"伤害报警请求", "练习说清位置、情况和求助信息。", "安全求助", FONT_AWESOME_TRIANGLE_EXCLAMATION,
     "伤害报警",
     "你扮演紧急求助接线员，带我练习伤害报警请求。先问我是否安全、在哪里、发生了什么、有没有受伤；语言简短冷静，提醒真实危险要马上联系家长或当地急救。"},
    {"去音乐节", "练习询问时间、票价、路线和入场规则。", "出行娱乐", FONT_AWESOME_MUSIC,
     "去音乐节",
     "你扮演音乐节工作人员，我来练习去音乐节的对话。请先问我想看哪类演出，再引导我询问时间、票价、路线、入场规则；每次只说一两句。"},
    {"参观博物馆", "像讲解员一样介绍展品并提问。", "出行娱乐", FONT_AWESOME_IMAGE,
     "参观博物馆",
     "你扮演博物馆讲解员，带我参观博物馆。请先欢迎我并问我喜欢历史、科学还是艺术；介绍要简单生动，每次回答后问一个观察问题。"},
    {"买电脑", "练习预算、用途、配置和售后表达。", "购物服务", FONT_AWESOME_MICROCHIP_AI,
     "买电脑",
     "你扮演电脑店员，我来练习买电脑。请先问我的预算和用途，再解释学习、画画、游戏、视频会议需要关注的配置；每次回答不超过70字。"},
    {"药店买药", "练习描述症状，并加入安全提醒。", "健康生活", FONT_AWESOME_HEART,
     "药店买药",
     "你扮演药店店员，我来练习买药对话。请先问我哪里不舒服，提醒我严重症状要找医生或告诉家长；每次只说一两句，等我回答。"},
    {"博物馆买票", "练习票种、优惠、开放时间和取票。", "买票办事", FONT_AWESOME_CALENDAR,
     "博物馆买票",
     "你扮演博物馆售票员，我来练习买票。请先问我要哪一天、几个人，再引导我询问儿童票、开放时间、取票方式和注意事项。"},
    {"买电影票", "练习选影片、场次、座位和付款。", "买票办事", FONT_AWESOME_PLAY,
     "买电影票",
     "你扮演电影院售票员，我来练习买电影票。请先问我想看什么电影和时间，再引导我选择场次、座位、票数和付款方式。"},
    {"美甲", "练习预约、颜色、款式和价格。", "购物服务", FONT_AWESOME_PEN_TO_SQUARE,
     "美甲预约",
     "你扮演美甲店店员，我来练习预约美甲。请先问我想预约哪天，再聊颜色、款式、价格和护理注意事项；语气友好简短。"},
    {"护照检查", "练习边检问答，回答清楚不紧张。", "出行办事", FONT_AWESOME_GLOBE,
     "护照检查",
     "你扮演护照检查工作人员，我来练习过关问答。请依次询问旅行目的、停留时间、住在哪里、是否带违禁物品；语气正式但友好。"},
    {"乘地铁", "练习问路、买票、换乘和出站。", "出行办事", FONT_AWESOME_LOCATION_ARROW,
     "乘地铁",
     "你扮演地铁站工作人员，我来练习乘地铁。请先问我要去哪里，再教我买票、看线路、换乘和出站；每轮只问一个问题。"},
    {"看牙医", "练习描述牙痛、检查和治疗建议。", "健康生活", FONT_AWESOME_GLASSES,
     "看牙医",
     "你扮演牙医，我来练习看牙医。请先问我哪里不舒服、疼多久了，再解释检查、刷牙和少吃糖的建议；避免恐吓，每次回答简短。"},
};

const char* const kHealthMetrics[] = {"健康分 91", "饮水 3次", "休息 2次"};
const char* const kHealthDescriptions[] = {
    "聚合坐姿、护眼、饮水和休息提醒。",
    "快捷创建 30/60/90 分钟饮水提醒。",
    "展示最近 7 天健康趋势，真实数据缺失时用演示数据。",
};
const char* const kHealthIcons[] = {FONT_AWESOME_HEART, FONT_AWESOME_BELL, FONT_AWESOME_CIRCLE_CHECK};

const char* const kDeviceMetrics[] = {"亮度 80%", "音量 60%", "SD卡 检测"};
const char* const kDeviceDescriptions[] = {
    "触摸滑杆调节屏幕亮度，后续接入 Backlight。",
    "触摸滑杆调节音量，后续接入 AudioCodec。",
    "检查音乐资源、皮肤目录和系统信息。",
};
const char* const kDeviceIcons[] = {FONT_AWESOME_SUN, FONT_AWESOME_VOLUME_HIGH, FONT_AWESOME_GEAR};

const char* const kMoreMetrics[] = {"成长档案", "家长控制", "固件升级"};
const char* const kMoreDescriptions[] = {
    "预留成长记录、学习报告和荣誉徽章入口。",
    "预留使用时段、内容权限和远程管理入口。",
    "预留版本检查、OTA 升级和关于设备页面。",
};
const char* const kMoreIcons[] = {FONT_AWESOME_STAR, FONT_AWESOME_GEAR, FONT_AWESOME_CIRCLE_CHECK};
}  // namespace

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        // Two line buffers keep LVGL asynchronous without binding its task to
        // the ST7701 driver's unreliable VSYNC semaphore path.
        .buffer_size = static_cast<uint32_t>(width_ * 80),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = true,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            // The ST7701 MIPI implementation does not reliably signal the
            // refresh semaphore used by esp_lvgl_port's avoid-tearing mode.
            // When enabled, LVGL can block and stop dispatching touch events.
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    StopPomodoroTimer();
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (app_menu_button_ != nullptr) {
        lv_obj_del(app_menu_button_);
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_del(app_grid_layer_);
    }
    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    for (int i = 0; i < 3; i++) {
        if (face_bufs_[i] != nullptr) {
            heap_caps_free(face_bufs_[i]);
            face_bufs_[i] = nullptr;
        }
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void LcdDisplay::StopPomodoroTimer() {
    if (pomodoro_timer_ != nullptr) {
        lv_timer_delete(pomodoro_timer_);
        pomodoro_timer_ = nullptr;
    }
    g_pomodoro_time_label = nullptr;
    g_pomodoro_state_label = nullptr;
    g_pomodoro_start_label = nullptr;
    g_pomodoro_done_label = nullptr;
    g_pomodoro_minutes_label = nullptr;
    g_pomodoro_arc = nullptr;
    for (auto& button : g_pomodoro_mode_buttons) {
        button = nullptr;
    }
}

void LcdDisplay::OnAppLauncherClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        display->OpenAppGrid();
    }
}

void LcdDisplay::OpenAppGrid() {
    ShowAppGrid();
}

lv_obj_t* LcdDisplay::CreateAppHeader(lv_obj_t* parent, const char* title, const char* subtitle,
                                      lv_color_t back_bg_color, lv_color_t text_color, lv_color_t subtext_color) {
    (void)back_bg_color;
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, LV_HOR_RES - kPagePad * 2, 66);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_pos(back, 0, 5);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xF4F7F6), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ANGLE_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_transform_scale(back_label, 310, 0);
    lv_obj_set_style_text_color(back_label, text_color, 0);
    lv_obj_center(back_label);

    lv_obj_t* title_box = lv_obj_create(header);
    // Use the complete header width so the title is centered against the
    // physical screen, independent of the back button and right-side content.
    lv_obj_set_size(title_box, LV_HOR_RES - kPagePad * 2, 60);
    lv_obj_set_pos(title_box, 0, 3);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_left(title_box, 0, 0);
    lv_obj_set_style_pad_right(title_box, 0, 0);
    lv_obj_set_style_pad_top(title_box, 2, 0);
    lv_obj_set_style_pad_bottom(title_box, 0, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title_label = lv_label_create(title_box);
    lv_label_set_text(title_label, title);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title_label, LV_HOR_RES - kPagePad * 2 - 120);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title_label, text_font, 0);
    app_ui::StylePageTitle(title_label);
    lv_obj_set_style_text_color(title_label, text_color, 0);

    if (subtitle != nullptr && subtitle[0] != '\0') {
        lv_obj_t* subtitle_label = lv_label_create(title_box);
        lv_label_set_text(subtitle_label, subtitle);
        lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(subtitle_label, LV_HOR_RES - kPagePad * 2 - 120);
        lv_obj_set_style_text_align(subtitle_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(subtitle_label, subtext_color, 0);
        lv_obj_set_style_margin_top(subtitle_label, 2, 0);
    }

    lv_obj_t* right_space = lv_obj_create(header);
    lv_obj_set_size(right_space, 56, 56);
    lv_obj_set_pos(right_space, LV_HOR_RES - kPagePad * 2 - 56, 5);
    lv_obj_set_style_bg_opa(right_space, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_space, 0, 0);
    lv_obj_clear_flag(right_space, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(back);

    return header;
}

void LcdDisplay::OnAppGridCloseClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        display->HideAppGrid();
    }
}

void LcdDisplay::OnAppDetailBackClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        display->ShowAppGrid();
    }
}

void LcdDisplay::OnAppModuleClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* module = static_cast<const XiaoliangAppModule*>(lv_obj_get_user_data(target));
    if (display != nullptr && module != nullptr) {
        if (module->kind == XiaoliangAppModule::Kind::kTask) {
            display->ShowTaskScheduler();
        } else if (module->kind == XiaoliangAppModule::Kind::kPomodoro) {
            display->ShowPomodoroTimer();
        } else if (module->kind == XiaoliangAppModule::Kind::kPet) {
            display->ShowPetGarden();
        } else if (module->kind == XiaoliangAppModule::Kind::kEyeIsland) {
            display->ShowEyeIsland();
        } else if (module->kind == XiaoliangAppModule::Kind::kMusic) {
            display->ShowMusicBox();
        } else if (module->kind == XiaoliangAppModule::Kind::kAiSpeaking) {
            display->ShowAiSpeaking();
        } else if (module->kind == XiaoliangAppModule::Kind::kHealth) {
            display->ShowHealthHub();
        } else if (module->kind == XiaoliangAppModule::Kind::kDevice) {
            display->ShowDeviceSettings();
        } else if (module->kind == XiaoliangAppModule::Kind::kMore) {
            display->ShowMoreApps();
        }
    }
}

void LcdDisplay::OnAppActionClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    const char* action_name = "功能";
    if (target != nullptr && lv_obj_get_user_data(target) != nullptr) {
        action_name = static_cast<const char*>(lv_obj_get_user_data(target));
    } else if (target != nullptr && lv_obj_get_child_cnt(target) > 0) {
        const uint32_t label_index = lv_obj_get_child_cnt(target) >= 2 ? 1 : 0;
        lv_obj_t* label = lv_obj_get_child(target, label_index);
        if (label != nullptr) {
            action_name = lv_label_get_text(label);
        }
    }
    if (display != nullptr) {
        if (strcmp(action_name, "视力训练") == 0 || strcmp(action_name, "放松眼睛") == 0) {
            display->ShowEyeTraining();
        } else if (strcmp(action_name, "任务统计") == 0 || strcmp(action_name, "提醒记录") == 0) {
            display->ShowTaskStats();
        } else if (strcmp(action_name, "自然之声") == 0) {
            display->ShowNatureSound("海浪之滩", nullptr);
        } else {
            display->ShowActionDetail(action_name);
        }
    }
}

void LcdDisplay::OnMusicSceneClicked(lv_event_t* event) {
    auto code = lv_event_get_code(event);
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* item = target != nullptr ? static_cast<MusicSceneItem*>(lv_obj_get_user_data(target)) : nullptr;
    if (code == LV_EVENT_DELETE) {
        delete item;
        return;
    }
    if (code != LV_EVENT_CLICKED || item == nullptr) {
        return;
    }

    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        const std::string title = item->track_name;
        const std::string path = item->path;
        display->ShowNatureSound(title.c_str(), path.c_str());
    }
}

void LcdDisplay::OnAiScenarioClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* scenario = target != nullptr ? static_cast<const AiScenarioItem*>(lv_obj_get_user_data(target)) : nullptr;
    if (scenario == nullptr) {
        return;
    }

    // Send the complete scene prompt. The short command is only a display/
    // routing label and does not contain enough context to keep the dialogue
    // in the scene selected by the user.
    Application::GetInstance().StartAiScenario(scenario->title, scenario->prompt);
    if (display != nullptr) {
        display->ShowAiChat(scenario->title);
    }
}

void LcdDisplay::OnDeviceWifiClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* wifi_board = dynamic_cast<WifiBoard*>(&Board::GetInstance());
    if (wifi_board == nullptr) {
        if (display != nullptr) display->ShowNotification("当前设备不支持 Wi-Fi 配网", 2500);
        return;
    }
    if (display != nullptr) display->ShowNotification("正在启动网络配网…", 1800);
    wifi_board->EnterWifiConfigMode();
}

void LcdDisplay::OnDeviceVolumeChanged(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (slider == nullptr) return;
    const int value = lv_slider_get_value(slider);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) codec->SetOutputVolume(value);
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(slider));
    if (label != nullptr) lv_label_set_text_fmt(label, "%d%%", value);
}

void LcdDisplay::OnDeviceBrightnessChanged(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (slider == nullptr) return;
    const int value = lv_slider_get_value(slider);
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) backlight->SetBrightness(value, true);
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(slider));
    if (label != nullptr) lv_label_set_text_fmt(label, "%d%%", value);
}

void LcdDisplay::OnCareModeChanged(lv_event_t* event) {
    auto* control = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (control == nullptr) return;
    const bool enabled = lv_obj_has_state(control, LV_STATE_CHECKED);
    auto config = Application::GetInstance().GetEyeCareConfig();
    config.enabled = enabled;
    config.posture_enabled = enabled;
    Application::GetInstance().ConfigureEyeCare(config);
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        display->ShowNotification(enabled ? "关怀模式已开启" : "关怀模式已关闭", 1800);
    }
}

void LcdDisplay::ShowActionDetail(const char* action_name) {
    const char* title = action_name != nullptr ? action_name : "功能详情";
    const char* body = "功能已准备就绪";
    const char* row1 = "当前状态：正常";
    const char* row2 = "数据自动更新";
    const char* row3 = "轻触即可操作";

    if (strcmp(title, "护眼科普") == 0) {
        body = "护眼知识用儿童能理解的短句展示，每页一条，适合学习间隙快速阅读。";
        row1 = "今日知识：写字时眼睛离书本保持一尺左右";
        row2 = "翻页：上一条 / 下一条";
        row3 = "入口：护眼岛首页";
    } else if (strcmp(title, "预约检查") == 0) {
        body = "预约检查先展示演示机构、推荐周期和最近可预约时间，后续接入真实预约接口。";
        row1 = "推荐周期：每 6 个月检查一次";
        row2 = "最近可约：周六 10:30";
        row3 = "状态：待家长确认";
    } else if (strcmp(title, "护眼报告") == 0) {
        body = "护眼报告读取 EyeCareService，缺失时显示今日演示趋势。";
        row1 = "护眼分：86";
        row2 = "远眺：4 次";
        row3 = "坐姿提醒：2 次";
    } else if (strcmp(title, "专注场景") == 0 || strcmp(title, "睡前放松") == 0 || strcmp(title, "白噪音") == 0) {
        body = "音乐盒从 /sdcard/yinyuehe/manifest.json 读取曲目，播放路径统一指向 /sdcard/yinyuehe/audio。";
        row1 = "关键词：雨声、海浪、森林、专注";
        row2 = "控制：播放 / 暂停 / 上一首 / 下一首";
        row3 = "冲突：TTS 时暂停或降级处理";
    } else if (strcmp(title, "日常对话") == 0 || strcmp(title, "英语练习") == 0 || strcmp(title, "情景列表") == 0) {
        body = "AI 听说按场景配置 prompt，点击开始后进入听说流程。";
        row1 = "场景：问候、英语、看图、故事";
        row2 = "反馈：星星、轮次、鼓励语";
        row3 = "离线：展示网络提示";
    } else if (strcmp(title, "坐姿提醒") == 0 || strcmp(title, "饮水提醒") == 0 || strcmp(title, "休息打卡") == 0) {
        body = "健康提醒聚合坐姿、饮水、休息和远眺，提醒能力复用定时任务服务。";
        row1 = "今日健康分：91";
        row2 = "快捷提醒：30 / 60 / 90 分钟";
        row3 = "异常：摄像头或模型缺失时提示";
    } else if (strcmp(title, "亮度") == 0 || strcmp(title, "音量") == 0 || strcmp(title, "网络") == 0) {
        body = "设备设置提供触摸控制入口，后续接入具体硬件 setter。";
        row1 = "亮度：80%";
        row2 = "音量：60%";
        row3 = "网络：显示当前 Wi-Fi 与配网入口";
    } else if (strcmp(title, "成长档案") == 0 || strcmp(title, "系统信息") == 0 || strcmp(title, "模块扩展") == 0) {
        body = "更多模块作为后续扩展中心，保持统一入口、状态和返回体验。";
        row1 = "成长档案：学习与习惯记录";
        row2 = "系统信息：版本、板型、内存、运行时间";
        row3 = "模块扩展：按注册表新增";
    }

    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(app_detail_layer_, LV_DIR_VER);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);

    CreateAppHeader(app_detail_layer_, title, nullptr, lv_color_hex(0xE4ECE8),
                    lv_color_hex(0x15231F), lv_color_hex(0x60736B));

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hero, 116, 0);
    lv_obj_set_style_radius(hero, kCardRadius, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x1D6B5F), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 14, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* hero_text = lv_label_create(hero);
    lv_label_set_text(hero_text, body);
    lv_obj_set_width(hero_text, LV_HOR_RES - kPagePad * 2 - 32);
    lv_label_set_long_mode(hero_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_text, lv_color_white(), 0);

    const char* rows[] = {row1, row2, row3};
    for (const char* row_text : rows) {
        lv_obj_t* row = lv_obj_create(app_detail_layer_);
        lv_obj_set_size(row, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 58, 0);
        lv_obj_set_style_radius(row, kCardRadius, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(kBorderSoft), 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* row_label = lv_label_create(row);
        lv_label_set_text(row_label, row_text);
        lv_obj_set_width(row_label, LV_HOR_RES - kPagePad * 2 - 32);
        lv_label_set_long_mode(row_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(row_label, lv_color_hex(0x15231F), 0);
    }

    lv_obj_t* cta = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(cta, LV_HOR_RES - kPagePad * 2, 52);
    lv_obj_set_style_radius(cta, kCardRadius, 0);
    lv_obj_set_style_bg_color(cta, lv_color_hex(0xE5F1ED), 0);
    lv_obj_set_style_border_width(cta, 0, 0);
    lv_obj_set_style_shadow_width(cta, 0, 0);
    lv_obj_add_flag(cta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cta, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* cta_label = lv_label_create(cta);
    lv_label_set_text(cta_label, "返回");
    lv_obj_set_style_text_color(cta_label, lv_color_hex(0x1D6B5F), 0);
    lv_obj_center(cta_label);

    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::OnPomodoroStartClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    PomodoroApplyElapsed();
    if (g_pomodoro.state == PomodoroState::kRunning) {
        g_pomodoro.state = PomodoroState::kPaused;
    } else {
        if (g_pomodoro.state == PomodoroState::kFinished || g_pomodoro.remaining_seconds == 0) {
            PomodoroResetForMode(g_pomodoro.mode);
        }
        g_pomodoro.state = PomodoroState::kRunning;
        g_pomodoro.last_tick = time(nullptr);
    }
    if (display != nullptr) {
        display->ShowPomodoroTimer();
    }
}

void LcdDisplay::OnPomodoroResetClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    PomodoroResetForMode(g_pomodoro.mode);
    if (display != nullptr) {
        display->ShowPomodoroTimer();
    }
}

void LcdDisplay::OnPomodoroModeClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto mode = target != nullptr ? static_cast<PomodoroMode>((intptr_t)lv_obj_get_user_data(target)) : PomodoroMode::kFocus;
    PomodoroResetForMode(mode);
    if (display != nullptr) {
        display->ShowPomodoroTimer();
    }
}

void LcdDisplay::OnPetSkinClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* skin = target != nullptr ? static_cast<const PetSkinPreset*>(lv_obj_get_user_data(target)) : nullptr;
    if (display == nullptr || skin == nullptr) {
        return;
    }

    if (!FileExists(skin->directory)) {
        char message[96];
        snprintf(message, sizeof(message), "%s未安装：%s", skin->title, skin->directory);
        display->ShowNotification(message, 2400);
        return;
    }

    Settings settings(kPetSettingsNamespace, true);
    settings.SetString(kPetStyleKey, skin->style_id);

    char message[96];
    snprintf(message, sizeof(message), "已切换皮肤：%s", skin->title);
    display->ShowNotification(message, 2200);
    display->ShowPetGarden();
}

void LcdDisplay::OnPetSkinsOpenClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) display->ShowPetSkins();
}

void LcdDisplay::OnPetSkinsBackClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) display->ShowPetGarden();
}

void LcdDisplay::CreateTouchAppLauncher(lv_obj_t* screen) {
    if (screen == nullptr || app_menu_button_ != nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto icon_font = lvgl_theme->icon_font()->font();

    app_menu_button_ = lv_obj_create(screen);
    lv_obj_set_size(app_menu_button_, 50, 50);
    lv_obj_set_style_radius(app_menu_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(app_menu_button_, lv_color_hex(0x1D6B5F), 0);
    lv_obj_set_style_bg_opa(app_menu_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_menu_button_, 0, 0);
    lv_obj_set_style_pad_all(app_menu_button_, 0, 0);
    lv_obj_set_style_shadow_width(app_menu_button_, 10, 0);
    lv_obj_set_style_shadow_opa(app_menu_button_, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(app_menu_button_, lv_color_hex(0x184E46), 0);
    lv_obj_set_style_shadow_ofs_y(app_menu_button_, 3, 0);
    lv_obj_set_scrollbar_mode(app_menu_button_, LV_SCROLLBAR_MODE_OFF);
    // The launcher is enabled only after the home content has finished
    // loading. This prevents taps from reaching an incomplete page.
    lv_obj_add_flag(app_menu_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(app_menu_button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(app_menu_button_, OnAppLauncherClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* icon = lv_label_create(app_menu_button_);
    lv_label_set_text(icon, FONT_AWESOME_GAMEPAD);
    lv_obj_set_style_text_font(icon, icon_font, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_center(icon);

    BringTouchAppLauncherToFront();
}

void LcdDisplay::BringTouchAppLauncherToFront() {
    if (app_menu_button_ == nullptr) {
        return;
    }

    const bool grid_visible = app_grid_layer_ != nullptr &&
                              !lv_obj_has_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    // Network setup must remain reachable even when the online home content
    // cannot finish loading. In Wi-Fi configuration mode the local launcher
    // is safe to expose because it does not depend on cloud resources.
    const bool configuring_wifi =
        Application::GetInstance().GetDeviceState() == kDeviceStateWifiConfiguring;
    if ((!app_launcher_ready_ && !configuring_wifi) || grid_visible || app_detail_layer_ != nullptr) {
        lv_obj_add_flag(app_menu_button_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(app_menu_button_, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    lv_obj_remove_flag(app_menu_button_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(app_menu_button_, LV_OBJ_FLAG_CLICKABLE);
    // Keep the launcher in the lower-right safe area so the home status text
    // remains fully visible across different content states.
    lv_obj_align(app_menu_button_, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
    lv_obj_move_foreground(app_menu_button_);
}

void LcdDisplay::ShowAppGrid() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    auto icon_font = theme->icon_font()->font();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_del(app_grid_layer_);
        app_grid_layer_ = nullptr;
    }

    app_grid_layer_ = lv_obj_create(lv_screen_active());
    BringTouchAppLauncherToFront();
    lv_obj_set_size(app_grid_layer_, 480, 800);
    lv_obj_set_pos(app_grid_layer_, 0, 0);
    lv_obj_set_style_radius(app_grid_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_grid_layer_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(app_grid_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_grid_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_grid_layer_, 0, 0);
    lv_obj_set_scrollbar_mode(app_grid_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_grid_layer_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* time_label = lv_label_create(app_grid_layer_);
    lv_label_set_text(time_label, "10:30");
    lv_obj_set_pos(time_label, 20, 12);
    lv_obj_set_style_text_font(time_label, text_font, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x17201E), 0);

    lv_obj_t* status = lv_label_create(app_grid_layer_);
    lv_label_set_text(status, "WiFi   100%");
    lv_obj_set_pos(status, 366, 12);
    lv_obj_set_style_text_color(status, lv_color_hex(0x17201E), 0);

    lv_obj_t* title = lv_label_create(app_grid_layer_);
    lv_label_set_text(title, "应用列表");
    lv_obj_set_style_text_font(title, text_font, 0);
    app_ui::StylePageTitle(title);
    lv_obj_set_style_text_color(title, lv_color_hex(0x101716), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    struct LauncherEntry {
        AppKind kind;
        const char* title;
        const char* asset;
        const char* fallback;
        uint32_t tint;
    };
    const LauncherEntry entries[] = {
        {AppKind::kPomodoro, "番茄时钟", "app_tomato", FONT_AWESOME_CLOCK, 0xFCE9DF},
        {AppKind::kTask, "任务计划", "app_task", FONT_AWESOME_CLOCK, 0xE7F4FC},
        {AppKind::kEyeIsland, "护眼岛", "app_island", FONT_AWESOME_GLASSES, 0xE4F4FB},
        {AppKind::kPet, "宠物花园", "app_pet", FONT_AWESOME_GAMEPAD, 0xE8F6EC},
        {AppKind::kMusic, "音乐盒", "app_music", FONT_AWESOME_MUSIC, 0xF1ECFA},
        {AppKind::kAiSpeaking, "AI听说", "app_ai", FONT_AWESOME_COMMENT, 0xEAF3FC},
        {AppKind::kHealth, "健康中心", "app_health", FONT_AWESOME_HEART, 0xFCEBE6},
        {AppKind::kDevice, "设备设置", "app_device", FONT_AWESOME_GEAR, 0xEDF1F8},
    };

    constexpr int card_w = 134;
    constexpr int card_h = 136;
    constexpr int start_x = 18;
    constexpr int start_y = 112;
    constexpr int gap_x = 13;
    constexpr int gap_y = 18;
    constexpr int image_slot_h = 100;
    // Source illustrations use a 160x160 canvas. Scale 148/256 limits their
    // largest visible dimension to about 84px while preserving aspect ratio.
    constexpr int image_scale = 148;
    auto collection = theme->emoji_collection();
    constexpr size_t entry_count = sizeof(entries) / sizeof(entries[0]);
    for (size_t i = 0; i < entry_count; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        const int remaining = static_cast<int>(entry_count) - row * 3;
        const int row_count = remaining < 3 ? remaining : 3;
        const int row_start_x = row_count < 3
            ? (LV_HOR_RES - (row_count * card_w + (row_count - 1) * gap_x)) / 2
            : start_x;
        lv_obj_t* card = lv_obj_create(app_grid_layer_);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_pos(card, row_start_x + col * (card_w + gap_x),
                       start_y + row * (card_h + gap_y));
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(entries[i].tint), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        app_ui::StylePressable(card);
        const auto* module = FindModule(entries[i].kind);
        lv_obj_set_user_data(card, const_cast<XiaoliangAppModule*>(module));
        lv_obj_add_event_cb(card, OnAppModuleClicked, LV_EVENT_CLICKED, this);

        const LvglImage* image = collection ? collection->GetEmojiImage(entries[i].asset) : nullptr;
        if (image != nullptr) {
            lv_obj_t* image_slot = lv_obj_create(card);
            lv_obj_set_size(image_slot, card_w, image_slot_h);
            lv_obj_set_pos(image_slot, 0, 4);
            lv_obj_set_style_bg_opa(image_slot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(image_slot, 0, 0);
            lv_obj_set_style_pad_all(image_slot, 0, 0);
            lv_obj_clear_flag(image_slot, LV_OBJ_FLAG_SCROLLABLE);
            // Decorative children must not consume pointer events. The whole
            // card is the single, generous touch target.
            lv_obj_clear_flag(image_slot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* icon = lv_image_create(image_slot);
            lv_image_set_src(icon, image->image_dsc());
            lv_image_set_scale(icon, image_scale);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_center(icon);
        } else {
            lv_obj_t* icon = lv_label_create(card);
            lv_label_set_text(icon, entries[i].fallback);
            lv_obj_set_style_text_font(icon, icon_font, 0);
            lv_obj_set_style_transform_scale(icon, 260, 0);
            lv_obj_set_style_text_color(icon, lv_color_hex(0x52706C), 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 34);
        }

        lv_obj_t* label = lv_label_create(card);
        lv_label_set_text(label, entries[i].title);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x26302E), 0);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    CreateProductBottomNav(app_grid_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_grid_layer_);
}

void LcdDisplay::ShowLegacyAppGrid() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_clear_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(app_grid_layer_);
        return;
    }

    app_grid_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_grid_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_grid_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_grid_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_grid_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_grid_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_grid_layer_, 0, 0);
    lv_obj_set_style_pad_row(app_grid_layer_, 0, 0);
    lv_obj_set_scrollbar_mode(app_grid_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(app_grid_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_grid_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* header = lv_obj_create(app_grid_layer_);
    lv_obj_set_size(header, LV_HOR_RES, 86);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_left(header, kPagePad, 0);
    lv_obj_set_style_pad_right(header, kPagePad, 0);
    lv_obj_set_style_pad_top(header, 18, 0);
    lv_obj_set_style_pad_bottom(header, 0, 0);
    lv_obj_set_style_shadow_width(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title_box = lv_obj_create(header);
    lv_obj_set_size(title_box, LV_HOR_RES - 116, 58);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_left(title_box, 54, 0);
    lv_obj_set_style_pad_top(title_box, 0, 0);
    lv_obj_set_style_pad_bottom(title_box, 0, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(title_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* title = lv_label_create(title_box);
    lv_label_set_text(title, "应用列表");
    lv_obj_set_style_text_font(title, text_font, 0);
    lv_obj_set_style_transform_scale(title, 220, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kTextPrimary), 0);

    lv_obj_t* edit = lv_obj_create(header);
    lv_obj_set_size(edit, 58, 38);
    lv_obj_set_style_radius(edit, 19, 0);
    lv_obj_set_style_bg_color(edit, lv_color_hex(0xF2F5FA), 0);
    lv_obj_set_style_border_width(edit, 0, 0);
    lv_obj_set_style_pad_all(edit, 0, 0);
    lv_obj_t* edit_label = lv_label_create(edit);
    lv_label_set_text(edit_label, "编辑");
    lv_obj_set_style_text_font(edit_label, text_font, 0);
    lv_obj_set_style_text_color(edit_label, lv_color_hex(kTextPrimary), 0);
    lv_obj_center(edit_label);

    lv_obj_t* content = lv_obj_create(app_grid_layer_);
    lv_obj_set_size(content, LV_HOR_RES - kPagePad * 2, LV_VER_RES - 158);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_row(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* grid = lv_obj_create(content);
    lv_obj_set_size(grid, LV_HOR_RES - kPagePad * 2, 540);
    lv_obj_set_style_min_height(grid, 0, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 14, 0);
    lv_obj_set_style_pad_column(grid, 12, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const AppKind grid_order[] = {
        AppKind::kPomodoro, AppKind::kTask, AppKind::kEyeIsland,
        AppKind::kPet, AppKind::kMusic, AppKind::kAiSpeaking,
        AppKind::kHealth, AppKind::kDevice,
    };
    const lv_coord_t tile_width = (LV_HOR_RES - kPagePad * 2 - 24) / 3;
    const lv_coord_t tile_height = 160;
    auto emoji_collection = lvgl_theme->emoji_collection();
    for (auto kind : grid_order) {
        const auto* module_ptr = FindModule(kind);
        if (module_ptr == nullptr) continue;
        const auto& module = *module_ptr;
        const uint32_t tint = module.tint;
        lv_obj_t* tile = lv_obj_create(grid);
        lv_obj_set_size(tile, tile_width, tile_height);
        app_ui::StyleCard(tile);
        lv_obj_set_style_bg_color(tile, lv_color_hex(tint), 0);
        lv_obj_set_style_border_width(tile, 0, 0);
        lv_obj_set_style_radius(tile, 18, 0);
        lv_obj_set_style_shadow_width(tile, 0, 0);
        lv_obj_set_style_pad_all(tile, 7, 0);
        app_ui::StylePressable(tile);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(tile, const_cast<XiaoliangAppModule*>(&module));
        lv_obj_add_event_cb(tile, OnAppModuleClicked, LV_EVENT_CLICKED, this);

        const LvglImage* asset = emoji_collection != nullptr
            ? emoji_collection->GetEmojiImage(module.image_asset) : nullptr;
        if (asset != nullptr) {
            lv_obj_t* image = lv_image_create(tile);
            lv_image_set_src(image, asset->image_dsc());
            lv_image_set_scale(image, 140);
            lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t* icon = lv_label_create(tile);
            lv_label_set_text(icon, module.icon);
            lv_obj_set_style_text_font(icon, icon_font, 0);
            lv_obj_set_style_transform_scale(icon, 260, 0);
            lv_obj_set_style_text_color(icon, lv_color_hex(module.accent), 0);
            lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t* module_title = lv_label_create(tile);
        lv_label_set_text(module_title, module.title);
        lv_label_set_long_mode(module_title, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(module_title, text_font, 0);
        app_ui::StyleCardTitle(module_title);
        lv_obj_set_style_text_color(module_title, lv_color_hex(kTextPrimary), 0);
        lv_obj_set_style_margin_top(module_title, -8, 0);
        lv_obj_clear_flag(module_title, LV_OBJ_FLAG_CLICKABLE);
    }

    CreateProductBottomNav(app_grid_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_grid_layer_);
}

void LcdDisplay::HideAppGrid() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_del(app_grid_layer_);
        app_grid_layer_ = nullptr;
    }
    BringTouchAppLauncherToFront();
}

void LcdDisplay::ShowAppDetail(const char* title, const char* subtitle, const char* const* actions, size_t action_count) {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF7FAF8), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);

    CreateAppHeader(app_detail_layer_, title, subtitle, lv_color_hex(0xE4ECE8),
                    lv_color_hex(0x15231F), lv_color_hex(0x60736B));

    lv_obj_t* list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list, LV_HOR_RES - kPagePad * 2, LV_VER_RES - 98);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (size_t i = 0; i < action_count; ++i) {
        lv_obj_t* action = lv_obj_create(list);
        lv_obj_set_size(action, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(action, 82, 0);
        lv_obj_set_style_radius(action, kCardRadius, 0);
        lv_obj_set_style_bg_color(action, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(action, 1, 0);
        lv_obj_set_style_border_color(action, lv_color_hex(kBorderSoft), 0);
        lv_obj_set_style_pad_left(action, 18, 0);
        lv_obj_set_style_pad_right(action, 14, 0);
        lv_obj_set_style_shadow_width(action, 0, 0);
        lv_obj_set_scrollbar_mode(action, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(action, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(action, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(action, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(action, OnAppActionClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_label_create(action);
        lv_label_set_text(icon, FONT_AWESOME_CIRCLE_CHECK);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x1D6B5F), 0);

        lv_obj_t* action_label = lv_label_create(action);
        lv_label_set_text(action_label, actions[i]);
        lv_obj_set_width(action_label, LV_HOR_RES - 128);
        lv_label_set_long_mode(action_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(action_label, text_font, 0);
        lv_obj_set_style_text_color(action_label, lv_color_hex(0x15231F), 0);
        lv_obj_set_style_margin_left(action_label, 14, 0);

        lv_obj_t* arrow = lv_label_create(action);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(arrow, icon_font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x8CA099), 0);
        lv_obj_set_style_margin_left(arrow, 0, 0);
    }

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowFeatureDashboard(const char* title, const char* subtitle, uint32_t hero_color,
                                      const char* hero_title, const char* hero_body,
                                      const char* const* metrics, size_t metric_count,
                                      const char* const* actions, const char* const* descriptions,
                                      const char* const* icons, size_t action_count,
                                      const char* resource_note) {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 10, 0);

    CreateAppHeader(app_detail_layer_, title, subtitle, lv_color_hex(0xE4ECE8),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* body = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(body, LV_HOR_RES - kPagePad * 2, LV_VER_RES - 164);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* hero = lv_obj_create(body);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hero, 128, 0);
    lv_obj_set_style_radius(hero, kCardRadius, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(hero_color), 0);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(app_ui::kBrandDark), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* hero_title_label = lv_label_create(hero);
    lv_label_set_text(hero_title_label, hero_title);
    lv_obj_set_style_text_font(hero_title_label, text_font, 0);
    app_ui::StyleHeroTitle(hero_title_label);
    lv_obj_set_style_text_color(hero_title_label, lv_color_white(), 0);

    lv_obj_t* hero_body_label = lv_label_create(hero);
    lv_label_set_text(hero_body_label, hero_body);
    lv_obj_set_width(hero_body_label, LV_HOR_RES - kPagePad * 2 - 32);
    lv_label_set_long_mode(hero_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_body_label, lv_color_hex(0xEEF7F4), 0);
    lv_obj_set_style_margin_top(hero_body_label, 6, 0);

    lv_obj_t* metric_row = lv_obj_create(body);
    lv_obj_set_size(metric_row, LV_HOR_RES - kPagePad * 2, 54);
    lv_obj_set_style_bg_opa(metric_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metric_row, 0, 0);
    lv_obj_set_style_pad_all(metric_row, 0, 0);
    lv_obj_set_style_pad_column(metric_row, 8, 0);
    lv_obj_set_flex_flow(metric_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metric_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_coord_t metric_width = (LV_HOR_RES - kPagePad * 2 - 16) / 3;
    for (size_t i = 0; i < metric_count && i < 3; ++i) {
        lv_obj_t* metric = lv_obj_create(metric_row);
        lv_obj_set_size(metric, metric_width, 48);
        app_ui::StyleCard(metric);
        lv_obj_set_style_pad_all(metric, 0, 0);
        lv_obj_t* label = lv_label_create(metric);
        lv_label_set_text(label, metrics[i]);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(hero_color), 0);
        lv_obj_center(label);
    }

    for (size_t i = 0; i < action_count; ++i) {
        lv_obj_t* card = lv_obj_create(body);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(card, 86, 0);
        app_ui::StyleCard(card);
        lv_obj_set_style_pad_left(card, 14, 0);
        lv_obj_set_style_pad_right(card, 14, 0);
        lv_obj_set_style_pad_top(card, 10, 0);
        lv_obj_set_style_pad_bottom(card, 10, 0);
        app_ui::StylePressable(card);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, const_cast<char*>(actions[i]));
        lv_obj_add_event_cb(card, OnAppActionClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 48, 48);
        lv_obj_set_style_radius(icon_box, app_ui::kControlRadius, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xE5F1ED), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, icons != nullptr ? icons[i] : FONT_AWESOME_CIRCLE_CHECK);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(hero_color), 0);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - kPagePad * 2 - 132, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* action_label = lv_label_create(text_box);
        lv_label_set_text(action_label, actions[i]);
        lv_obj_set_style_text_font(action_label, text_font, 0);
        app_ui::StyleCardTitle(action_label);
        lv_obj_set_style_text_color(action_label, lv_color_hex(kTextPrimary), 0);

        lv_obj_t* desc_label = lv_label_create(text_box);
        lv_label_set_text(desc_label, descriptions != nullptr ? descriptions[i] : "点击进入功能详情");
        app_ui::StyleBody(desc_label);
        lv_obj_set_width(desc_label, LV_HOR_RES - kPagePad * 2 - 140);
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(kTextSecondary), 0);
        lv_obj_set_style_margin_top(desc_label, 3, 0);

        lv_obj_t* arrow = lv_label_create(card);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(arrow, icon_font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x8CA099), 0);
    }

    lv_obj_t* note = lv_obj_create(body);
    lv_obj_set_size(note, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(note, 76, 0);
    lv_obj_set_style_radius(note, kCardRadius, 0);
    lv_obj_set_style_bg_color(note, lv_color_hex(0xEEF4F1), 0);
    lv_obj_set_style_border_width(note, 0, 0);
    lv_obj_set_style_pad_all(note, 12, 0);
    lv_obj_set_style_shadow_width(note, 0, 0);
    lv_obj_set_scrollbar_mode(note, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(note, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(note, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* note_label = lv_label_create(note);
    lv_label_set_text(note_label, resource_note);
    lv_obj_set_width(note_label, LV_HOR_RES - kPagePad * 2 - 30);
    lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(note_label, lv_color_hex(0x36564C), 0);

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowEyeIsland() {
    Settings eye_ui_settings("eye_ui", false);
    g_eye_height_cm = std::clamp<int>(eye_ui_settings.GetInt("height_cm", 170), 100, 220);
    cJSON* status = Application::GetInstance().GetEyeCareStatusJson();
    int score_value = 86;
    if (status != nullptr) {
        cJSON* score = cJSON_GetObjectItem(status, "health_score");
        if (cJSON_IsNumber(score)) score_value = score->valueint;
        cJSON_Delete(status);
    }

    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    auto icon_font = theme->icon_font()->font();
    if (app_detail_layer_) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);

    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_white(), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 10, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);

    CreateAppHeader(app_detail_layer_, "护眼岛", nullptr, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, 230);
    lv_obj_set_style_radius(hero, 28, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0xEFF9FF), 0);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0xE8F8EF), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 0, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    auto collection = theme->emoji_collection();
    const LvglImage* island = collection ? collection->GetEmojiImage("app_island") : nullptr;
    if (island) {
        lv_obj_t* image = lv_image_create(hero);
        lv_image_set_src(image, island->image_dsc());
        lv_image_set_scale(image, 285);
        lv_obj_align(image, LV_ALIGN_TOP_MID, 0, 2);
    }
    lv_obj_t* score_card = lv_obj_create(hero);
    lv_obj_set_size(score_card, 184, 124);
    lv_obj_align(score_card, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(score_card, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(score_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(score_card, LV_OPA_90, 0);
    lv_obj_set_style_border_width(score_card, 3, 0);
    lv_obj_set_style_border_color(score_card, lv_color_hex(0xD8ECF7), 0);
    lv_obj_set_style_pad_all(score_card, 6, 0);
    lv_obj_set_flex_flow(score_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(score_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* score_hint = lv_label_create(score_card);
    lv_label_set_text(score_hint, "今日护眼得分");
    lv_obj_set_style_text_color(score_hint, lv_color_hex(kTextSecondary), 0);
    char score_text[20];
    snprintf(score_text, sizeof(score_text), "%d 分", score_value);
    lv_obj_t* score = lv_label_create(score_card);
    lv_label_set_text(score, score_text);
    lv_obj_set_style_text_font(score, text_font, 0);
    lv_obj_set_style_transform_scale(score, 250, 0);
    lv_obj_set_style_text_color(score, lv_color_hex(0x205E5A), 0);
    lv_obj_t* praise = lv_label_create(score_card);
    lv_label_set_text(praise, "非常棒，继续保持！");
    lv_obj_set_style_text_color(praise, lv_color_hex(0x536A66), 0);

    lv_obj_t* metrics = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(metrics, LV_HOR_RES - kPagePad * 2, 86);
    lv_obj_set_style_bg_opa(metrics, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metrics, 0, 0);
    lv_obj_set_style_pad_all(metrics, 0, 0);
    lv_obj_set_style_pad_column(metrics, 8, 0);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    const char* metric_titles[] = {"用眼时长", "良好坐姿"};
    const char* metric_values[] = {"3.2 小时", "92 %"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* card = lv_obj_create(metrics);
        lv_obj_set_size(card, (LV_HOR_RES - kPagePad * 2 - 8) / 2, 82);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xFAFCFD), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xE7EDF1), 0);
        lv_obj_set_style_pad_all(card, 5, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* l1 = lv_label_create(card); lv_label_set_text(l1, metric_titles[i]);
        lv_obj_set_style_text_color(l1, lv_color_hex(kTextSecondary), 0);
        lv_obj_t* l2 = lv_label_create(card); lv_label_set_text(l2, metric_values[i]);
        lv_obj_set_style_text_font(l2, text_font, 0);
        lv_obj_set_style_text_color(l2, lv_color_hex(0x42A542), 0);
    }

    lv_obj_t* ergonomics = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(ergonomics, LV_HOR_RES - kPagePad * 2, 152);
    lv_obj_set_style_radius(ergonomics, 18, 0);
    lv_obj_set_style_bg_color(ergonomics, lv_color_hex(0xF5FAF7), 0);
    lv_obj_set_style_border_width(ergonomics, 1, 0);
    lv_obj_set_style_border_color(ergonomics, lv_color_hex(0xDDECE3), 0);
    lv_obj_set_style_pad_all(ergonomics, 12, 0);
    lv_obj_clear_flag(ergonomics, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* ergo_title = lv_label_create(ergonomics);
    lv_label_set_text(ergo_title, "桌椅高度");
    lv_obj_set_style_text_font(ergo_title, text_font, 0);
    app_ui::StyleSectionTitle(ergo_title);
    lv_obj_align(ergo_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* height_hint = lv_label_create(ergonomics);
    lv_label_set_text(height_hint, "输入身高");
    app_ui::StyleBody(height_hint);
    lv_obj_set_style_text_color(height_hint, lv_color_hex(kTextSecondary), 0);
    lv_obj_align(height_hint, LV_ALIGN_TOP_LEFT, 0, 42);

    const char* adjust_text[] = {"−", "+"};
    const int adjust_delta[] = {-1, 1};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t* button = lv_obj_create(ergonomics);
        lv_obj_set_size(button, 52, 48);
        lv_obj_align(button, LV_ALIGN_TOP_RIGHT, i == 0 ? -142 : 0, 30);
        lv_obj_set_style_radius(button, 14, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xE6F4EA), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(button, OnEyeHeightChanged, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(adjust_delta[i])));
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, adjust_text[i]);
        lv_obj_set_style_text_font(label, text_font, 0);
        app_ui::StylePageTitle(label);
        lv_obj_set_style_text_color(label, lv_color_hex(0x2E8E57), 0);
        lv_obj_center(label);
    }

    g_eye_height_label = lv_label_create(ergonomics);
    lv_obj_set_width(g_eye_height_label, 86);
    lv_obj_align(g_eye_height_label, LV_ALIGN_TOP_RIGHT, -54, 43);
    lv_obj_set_style_text_align(g_eye_height_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_eye_height_label, text_font, 0);
    app_ui::StyleCardTitle(g_eye_height_label);
    lv_obj_set_style_text_color(g_eye_height_label, lv_color_hex(kTextPrimary), 0);
    lv_obj_add_flag(g_eye_height_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_eye_height_label, OnEyeHeightInputClicked, LV_EVENT_CLICKED, nullptr);

    g_eye_seat_label = lv_label_create(ergonomics);
    lv_obj_align(g_eye_seat_label, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    lv_obj_set_style_text_color(g_eye_seat_label, lv_color_hex(0x2E8E57), 0);
    g_eye_desk_label = lv_label_create(ergonomics);
    lv_obj_align(g_eye_desk_label, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    lv_obj_set_style_text_color(g_eye_desk_label, lv_color_hex(0x2E8E57), 0);
    UpdateEyeErgonomicsUi();

    lv_obj_t* tip = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(tip, LV_HOR_RES - kPagePad * 2, 112);
    lv_obj_set_style_radius(tip, 18, 0);
    lv_obj_set_style_bg_color(tip, lv_color_hex(0xFFFDF8), 0);
    lv_obj_set_style_border_width(tip, 1, 0);
    lv_obj_set_style_border_color(tip, lv_color_hex(0xF0EEE8), 0);
    lv_obj_set_style_pad_all(tip, 14, 0);
    lv_obj_set_flex_flow(tip, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* tip_title = lv_label_create(tip); lv_label_set_text(tip_title, "坐姿小贴士");
    lv_obj_set_style_text_font(tip_title, text_font, 0);
    lv_obj_t* tip_body = lv_label_create(tip);
    lv_label_set_text(tip_body, "双脚平放，膝盖约 90°；桌面接近肘部高度，屏幕略低于视线。");
    lv_obj_set_width(tip_body, LV_HOR_RES - 64);
    lv_label_set_long_mode(tip_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(tip_body, lv_color_hex(kTextSecondary), 0);

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowEyeTraining() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    auto icon_font = theme->icon_font()->font();
    if (app_detail_layer_) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_white(), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    CreateAppHeader(app_detail_layer_, "护眼岛 - 训练", nullptr, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* tabs = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(tabs, LV_HOR_RES - kPagePad * 2, 42);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tabs, 0, 0);
    lv_obj_set_style_pad_all(tabs, 0, 0);
    lv_obj_set_style_pad_column(tabs, 6, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    const char* tab_names[] = {"视力训练", "放松眼睛", "眼保健操", "视力检测"};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t* tab = lv_obj_create(tabs);
        lv_obj_set_size(tab, (LV_HOR_RES - kPagePad * 2 - 18) / 4, 38);
        lv_obj_set_style_radius(tab, 12, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(i == 0 ? 0xE8F7E8 : 0xF6F7F8), 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_t* label = lv_label_create(tab); lv_label_set_text(label, tab_names[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(i == 0 ? 0x42A542 : 0x65726E), 0);
        lv_obj_center(label);
    }

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, 150);
    lv_obj_set_style_radius(hero, 18, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0xF1FAFF), 0);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0xEAF6FF), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(0xD9ECF8), 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_t* h1 = lv_label_create(hero); lv_label_set_text(h1, "追踪小球训练");
    lv_obj_set_style_text_font(h1, text_font, 0);
    app_ui::StylePageTitle(h1);
    lv_obj_set_style_text_color(h1, lv_color_hex(0x2C78C5), 0);
    lv_obj_t* h2 = lv_label_create(hero); lv_label_set_text(h2, "跟随眼球追踪路线，缓解眼部疲劳");
    app_ui::StyleBody(h2);
    lv_obj_set_style_text_color(h2, lv_color_hex(kTextSecondary), 0);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_obj_t* orbit = lv_arc_create(hero);
    lv_obj_set_size(orbit, 155, 76);
    lv_obj_align(orbit, LV_ALIGN_BOTTOM_RIGHT, -10, -2);
    lv_arc_set_rotation(orbit, 190);
    lv_arc_set_bg_angles(orbit, 0, 170);
    lv_arc_set_value(orbit, 74);
    lv_obj_set_style_arc_color(orbit, lv_color_hex(0xA6D8F5), LV_PART_MAIN);
    lv_obj_set_style_arc_color(orbit, lv_color_hex(0x69BF48), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(orbit, lv_color_hex(0x7ACB4E), LV_PART_KNOB);

    const char* sections[] = {"训练强度", "训练时长"};
    const char* choices[][3] = {{"初级", "中级", "高级"}, {"1分钟", "3分钟", "5分钟"}};
    for (int row = 0; row < 2; ++row) {
        lv_obj_t* label = lv_label_create(app_detail_layer_); lv_label_set_text(label, sections[row]);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_t* bar = lv_obj_create(app_detail_layer_);
        lv_obj_set_size(bar, LV_HOR_RES - kPagePad * 2, 48);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_pad_column(bar, 8, 0);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
        for (int i = 0; i < 3; ++i) {
            const bool selected = i == 1;
            lv_obj_t* choice = lv_obj_create(bar);
            lv_obj_set_size(choice, (LV_HOR_RES - kPagePad * 2 - 16) / 3, 44);
            lv_obj_set_style_radius(choice, 12, 0);
            lv_obj_set_style_bg_color(choice, lv_color_hex(selected ? 0xEEF9EA : 0xF7F8F9), 0);
            lv_obj_set_style_border_width(choice, selected ? 1 : 0, 0);
            lv_obj_set_style_border_color(choice, lv_color_hex(0xA8DA95), 0);
            lv_obj_set_style_pad_all(choice, 0, 0);
            lv_obj_t* t = lv_label_create(choice); lv_label_set_text(t, choices[row][i]);
            lv_obj_set_style_text_color(t, lv_color_hex(selected ? 0x3E9D37 : 0x606B68), 0);
            lv_obj_center(t);
        }
    }
    lv_obj_t* start = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(start, LV_HOR_RES - kPagePad * 2, 58);
    lv_obj_set_style_radius(start, 29, 0);
    lv_obj_set_style_bg_color(start, lv_color_hex(0x83D05C), 0);
    lv_obj_set_style_bg_grad_color(start, lv_color_hex(0x48B953), 0);
    lv_obj_set_style_bg_grad_dir(start, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(start, 0, 0);
    lv_obj_t* start_label = lv_label_create(start); lv_label_set_text(start_label, "开始训练");
    lv_obj_set_style_text_font(start_label, text_font, 0);
    lv_obj_set_style_text_color(start_label, lv_color_white(), 0);
    lv_obj_center(start_label);
    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowMusicBox() {
    int track_count = 0;
    int parse_status = 0;
    auto scenes = LoadMusicScenesFromManifest(&track_count, &parse_status);
    char hero_body[180];
    if (parse_status == 0 && track_count > 0) {
        snprintf(hero_body, sizeof(hero_body), "选择一个场景，开始聆听");
    } else if (parse_status == -1) {
        snprintf(hero_body, sizeof(hero_body), "未找到音乐资源");
    } else if (parse_status == -2) {
        snprintf(hero_body, sizeof(hero_body), "音乐清单格式异常");
    } else {
        snprintf(hero_body, sizeof(hero_body), "暂无可用音乐");
    }

    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);

    CreateAppHeader(app_detail_layer_, "音乐盒", nullptr, lv_color_hex(0xE4ECE8),
                    lv_color_hex(0x15231F), lv_color_hex(0x60736B));

    lv_obj_t* body = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(body, LV_HOR_RES - kPagePad * 2, LV_VER_RES - 96);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* hero = lv_obj_create(body);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hero, 176, 0);
    lv_obj_set_style_radius(hero, kCardRadius, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x7FB9A6), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto collection = lvgl_theme->emoji_collection();
    const LvglImage* music_image = collection ? collection->GetEmojiImage("app_music") : nullptr;
    if (music_image != nullptr) {
        lv_obj_t* image_slot = lv_obj_create(hero);
        lv_obj_set_size(image_slot, 120, 78);
        lv_obj_set_style_bg_opa(image_slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(image_slot, 0, 0);
        lv_obj_set_style_pad_all(image_slot, 0, 0);
        lv_obj_clear_flag(image_slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* image = lv_image_create(image_slot);
        lv_image_set_src(image, music_image->image_dsc());
        lv_image_set_scale(image, 122);
        lv_obj_center(image);
        lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(image_slot, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t* hero_title_label = lv_label_create(hero);
    lv_label_set_text(hero_title_label, "自然之声");
    lv_obj_set_style_text_font(hero_title_label, text_font, 0);
    app_ui::StyleHeroTitle(hero_title_label);
    lv_obj_set_style_text_color(hero_title_label, lv_color_white(), 0);
    lv_obj_set_width(hero_title_label, LV_HOR_RES - kPagePad * 2 - 32);
    lv_obj_set_style_text_align(hero_title_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* hero_body_label = lv_label_create(hero);
    lv_label_set_text(hero_body_label, hero_body);
    lv_obj_set_width(hero_body_label, LV_HOR_RES - kPagePad * 2 - 32);
    lv_label_set_long_mode(hero_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_body_label, lv_color_hex(0xEEF7F4), 0);
    app_ui::StyleBody(hero_body_label);
    lv_obj_set_style_text_align(hero_body_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_margin_top(hero_body_label, 6, 0);

    lv_obj_t* section = lv_label_create(body);
    lv_label_set_text(section, "选择音频");
    lv_obj_set_width(section, LV_HOR_RES - kPagePad * 2);
    lv_obj_set_style_text_font(section, text_font, 0);
    lv_obj_set_style_text_color(section, lv_color_hex(kTextPrimary), 0);
    lv_obj_set_style_text_align(section, LV_TEXT_ALIGN_CENTER, 0);

    for (const auto& scene : scenes) {
        lv_obj_t* card = lv_obj_create(body);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(card, 88, 0);
        lv_obj_set_style_radius(card, kCardRadius, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kBorderSoft), 0);
        lv_obj_set_style_pad_left(card, 14, 0);
        lv_obj_set_style_pad_right(card, 14, 0);
        lv_obj_set_style_pad_top(card, 10, 0);
        lv_obj_set_style_pad_bottom(card, 10, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, new MusicSceneItem(scene));
        lv_obj_add_event_cb(card, OnMusicSceneClicked, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(card, OnMusicSceneClicked, LV_EVENT_DELETE, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 50, 50);
        lv_obj_set_style_radius(icon_box, kCardRadius, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xE7EEF6), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, MusicSceneIcon(scene.scene));
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x335C81), 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - kPagePad * 2 - 140, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* title_label = lv_label_create(text_box);
        lv_label_set_text(title_label, scene.scene.c_str());
        lv_obj_set_style_text_font(title_label, text_font, 0);
        app_ui::StyleCardTitle(title_label);
        lv_obj_set_style_text_color(title_label, lv_color_hex(kTextPrimary), 0);
        lv_obj_set_width(title_label, LV_HOR_RES - kPagePad * 2 - 140);
        lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(title_label, LV_OBJ_FLAG_CLICKABLE);

        char desc[128];
        if (scene.duration_seconds > 0) {
            snprintf(desc, sizeof(desc), "%s | %d秒", scene.track_name.c_str(), scene.duration_seconds);
        } else {
            snprintf(desc, sizeof(desc), "%s", scene.track_name.c_str());
        }
        lv_obj_t* desc_label = lv_label_create(text_box);
        lv_label_set_text(desc_label, desc);
        app_ui::StyleBody(desc_label);
        lv_obj_set_width(desc_label, LV_HOR_RES - kPagePad * 2 - 148);
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(kTextSecondary), 0);
        lv_obj_set_style_margin_top(desc_label, 3, 0);
        lv_obj_set_style_text_align(desc_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(desc_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* play = lv_label_create(card);
        lv_label_set_text(play, FONT_AWESOME_PLAY);
        lv_obj_set_style_text_font(play, icon_font, 0);
        lv_obj_set_style_text_color(play, lv_color_hex(0x8CA099), 0);
        lv_obj_clear_flag(play, LV_OBJ_FLAG_CLICKABLE);
    }

    if (scenes.empty()) {
        lv_obj_t* note = lv_obj_create(body);
        lv_obj_set_size(note, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(note, 76, 0);
        lv_obj_set_style_radius(note, 8, 0);
        lv_obj_set_style_bg_color(note, lv_color_hex(0xEEF4F1), 0);
        lv_obj_set_style_border_width(note, 0, 0);
        lv_obj_set_style_pad_all(note, 12, 0);
        lv_obj_t* note_label = lv_label_create(note);
        lv_label_set_text(note_label, "未找到本地音乐");
        lv_obj_set_width(note_label, LV_HOR_RES - 54);
        lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(note_label, lv_color_hex(0x36564C), 0);
        lv_obj_set_style_text_align(note_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(note_label);
    }

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowNatureSound(const char* title, const char* path) {
    // Local music pages are static. Stop any residual face animation so JPEG
    // decoding cannot keep invalidating the display behind this opaque page.
    mjpeg_player_port_stop_wait(1000);
    if (path != nullptr && path[0] != '\0') {
        Application::GetInstance().GetAudioService().PlayAudioFile(path);
    }
    DisplayLockGuard lock(this);
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    if (app_detail_layer_) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_white(), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 14, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    CreateAppHeader(app_detail_layer_, "自然之声", nullptr, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* art = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(art, 300, 300);
    lv_obj_set_style_radius(art, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(art, lv_color_hex(0xEAF8FF), 0);
    lv_obj_set_style_border_width(art, 10, 0);
    lv_obj_set_style_border_color(art, lv_color_hex(0x63B85A), 0);
    lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 90);
    auto collection = theme->emoji_collection();
    const LvglImage* island = collection ? collection->GetEmojiImage("app_island") : nullptr;
    if (island) {
        lv_obj_t* image = lv_image_create(art);
        lv_image_set_src(image, island->image_dsc());
        lv_image_set_scale(image, 360);
        lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(image);
    }
    lv_obj_t* name = lv_label_create(app_detail_layer_);
    lv_label_set_text(name, title && title[0] ? title : "海浪之滩");
    lv_obj_set_style_text_font(name, text_font, 0);
    app_ui::StyleHeroTitle(name);
    lv_obj_set_style_text_color(name, lv_color_hex(kTextPrimary), 0);
    lv_obj_set_style_margin_top(name, 6, 0);
    lv_obj_set_width(name, LV_HOR_RES - kPagePad * 2);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* desc = lv_label_create(app_detail_layer_);
    lv_label_set_text(desc, "闭上眼睛，感受海浪拍打沙滩的声音");
    app_ui::StyleBody(desc);
    lv_obj_set_style_text_color(desc, lv_color_hex(kTextSecondary), 0);
    lv_obj_set_width(desc, LV_HOR_RES - kPagePad * 2);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowAiSpeaking() {
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);

    CreateAppHeader(app_detail_layer_, "AI听说", nullptr, lv_color_hex(0xE4ECE8),
                    lv_color_hex(0x15231F), lv_color_hex(0x60736B));

    lv_obj_t* body = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(body, LV_HOR_RES - kPagePad * 2, LV_VER_RES - 96);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t* hero = lv_obj_create(body);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hero, 128, 0);
    lv_obj_set_style_radius(hero, kCardRadius, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0xEAF5FF), 0);
    lv_obj_set_style_bg_grad_color(hero, lv_color_hex(0xF3F8FF), 0);
    lv_obj_set_style_bg_grad_dir(hero, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* hero_title_label = lv_label_create(hero);
    lv_label_set_text(hero_title_label, "Hi，我是小艾");
    lv_obj_set_style_text_font(hero_title_label, text_font, 0);
    app_ui::StyleHeroTitle(hero_title_label);
    lv_obj_set_style_text_color(hero_title_label, lv_color_hex(0x294D68), 0);

    lv_obj_t* hero_body_label = lv_label_create(hero);
    lv_label_set_text(hero_body_label, "今天想和我聊些什么呢？");
    app_ui::StyleBody(hero_body_label);
    lv_obj_set_width(hero_body_label, LV_HOR_RES - kPagePad * 2 - 32);
    lv_label_set_long_mode(hero_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_body_label, lv_color_hex(0x60758A), 0);
    lv_obj_set_style_margin_top(hero_body_label, 6, 0);

    lv_obj_t* metric_row = lv_obj_create(body);
    lv_obj_set_size(metric_row, LV_HOR_RES - kPagePad * 2, 54);
    lv_obj_set_style_bg_opa(metric_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metric_row, 0, 0);
    lv_obj_set_style_pad_all(metric_row, 0, 0);
    lv_obj_set_style_pad_column(metric_row, 8, 0);
    lv_obj_set_flex_flow(metric_row, LV_FLEX_FLOW_ROW);
    char scene_metric[32];
    snprintf(scene_metric, sizeof(scene_metric), "场景 %d个", static_cast<int>(sizeof(kAiScenarios) / sizeof(kAiScenarios[0])));
    const char* metrics[] = {scene_metric, "开场 引导", "持续 追问"};
    const lv_coord_t metric_width = (LV_HOR_RES - kPagePad * 2 - 16) / 3;
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t* metric = lv_obj_create(metric_row);
        lv_obj_set_size(metric, metric_width, 48);
        lv_obj_set_style_radius(metric, kCardRadius, 0);
        lv_obj_set_style_bg_color(metric, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(metric, 1, 0);
        lv_obj_set_style_border_color(metric, lv_color_hex(kBorderSoft), 0);
        lv_obj_set_style_pad_all(metric, 0, 0);
        lv_obj_set_style_shadow_width(metric, 0, 0);
        lv_obj_t* label = lv_label_create(metric);
        lv_label_set_text(label, metrics[i]);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x6A4C93), 0);
        lv_obj_center(label);
    }

    const char* current_category = nullptr;

    for (const auto& scenario : kAiScenarios) {
        if (current_category == nullptr || strcmp(current_category, scenario.category) != 0) {
            current_category = scenario.category;
            lv_obj_t* section_bar = lv_obj_create(body);
            lv_obj_set_size(section_bar, LV_HOR_RES - kPagePad * 2, 34);
            lv_obj_set_style_bg_opa(section_bar, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(section_bar, 0, 0);
            lv_obj_set_style_pad_all(section_bar, 0, 0);
            lv_obj_set_flex_flow(section_bar, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(section_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t* section = lv_label_create(section_bar);
            lv_label_set_text(section, current_category);
            lv_obj_set_style_text_font(section, text_font, 0);
            app_ui::StyleSectionTitle(section);
            lv_obj_set_style_text_color(section, lv_color_hex(kTextPrimary), 0);

            lv_obj_t* hint = lv_label_create(section_bar);
            lv_label_set_text(hint, "点击开始");
            lv_obj_set_style_text_color(hint, lv_color_hex(kTextSecondary), 0);
        }

        lv_obj_t* card = lv_obj_create(body);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(card, 88, 0);
        lv_obj_set_style_radius(card, kCardRadius, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(kBorderSoft), 0);
        lv_obj_set_style_pad_left(card, 14, 0);
        lv_obj_set_style_pad_right(card, 14, 0);
        lv_obj_set_style_pad_top(card, 10, 0);
        lv_obj_set_style_pad_bottom(card, 10, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, const_cast<AiScenarioItem*>(&scenario));
        lv_obj_add_event_cb(card, OnAiScenarioClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 50, 50);
        lv_obj_set_style_radius(icon_box, kCardRadius, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xEFE8F7), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, scenario.icon);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x6A4C93), 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - kPagePad * 2 - 140, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* title_label = lv_label_create(text_box);
        lv_label_set_text(title_label, scenario.title);
        lv_obj_set_style_text_font(title_label, text_font, 0);
        app_ui::StyleCardTitle(title_label);
        lv_obj_set_style_text_color(title_label, lv_color_hex(kTextPrimary), 0);
        lv_obj_clear_flag(title_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* desc_label = lv_label_create(text_box);
        lv_label_set_text(desc_label, scenario.description);
        app_ui::StyleBody(desc_label);
        lv_obj_set_width(desc_label, LV_HOR_RES - kPagePad * 2 - 148);
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(kTextSecondary), 0);
        lv_obj_set_style_margin_top(desc_label, 3, 0);
        lv_obj_clear_flag(desc_label, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* play = lv_label_create(card);
        lv_label_set_text(play, FONT_AWESOME_PLAY);
        lv_obj_set_style_text_font(play, icon_font, 0);
        lv_obj_set_style_text_color(play, lv_color_hex(0x8CA099), 0);
        lv_obj_clear_flag(play, LV_OBJ_FLAG_CLICKABLE);
    }

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowAiChat(const char* scenario) {
    DisplayLockGuard lock(this);
    const AiScenarioItem* selected = nullptr;
    for (const auto& item : kAiScenarios) {
        if (scenario != nullptr && strcmp(item.title, scenario) == 0) {
            selected = &item;
            break;
        }
    }
    if (app_detail_layer_) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_white(), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    CreateAppHeader(app_detail_layer_, "AI对话", scenario, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* chat = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(chat, LV_HOR_RES - kPagePad * 2, 510);
    lv_obj_set_style_bg_opa(chat, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat, 0, 0);
    lv_obj_set_style_pad_all(chat, 4, 0);
    lv_obj_set_style_pad_row(chat, 14, 0);
    lv_obj_set_flex_flow(chat, LV_FLEX_FLOW_COLUMN);
    char welcome[96];
    snprintf(welcome, sizeof(welcome), "已进入「%s」场景",
             selected != nullptr ? selected->title : (scenario != nullptr ? scenario : "AI对话"));
    const char* messages[] = {
        welcome,
        selected != nullptr ? selected->description : "场景已准备好，请开始说话。",
    };
    for (int i = 0; i < 2; ++i) {
        const bool user = false;
        lv_obj_t* bubble = lv_obj_create(chat);
        lv_obj_set_size(bubble, user ? 300 : 380, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(bubble, 64, 0);
        lv_obj_set_style_radius(bubble, 20, 0);
        lv_obj_set_style_bg_color(bubble, lv_color_hex(user ? 0xD8ECFF : 0xF2F6F8), 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_set_style_pad_all(bubble, 14, 0);
        lv_obj_set_style_align(bubble, user ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0);
        lv_obj_t* text = lv_label_create(bubble);
        lv_label_set_text(text, messages[i]);
        lv_obj_set_width(text, user ? 272 : 350);
        lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(text, lv_color_hex(0x263A46), 0);
    }

    lv_obj_t* listening = lv_label_create(app_detail_layer_);
    lv_label_set_text(listening, "正在聆听…");
    lv_obj_set_width(listening, LV_HOR_RES - kPagePad * 2);
    lv_obj_set_style_text_align(listening, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(listening, lv_color_hex(0x6D8190), 0);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowHealthHub() {
    ShowFeatureDashboard("健康", "饮水休息", 0x8B5E34,
                         "健康节奏", "聚合护眼和坐姿状态，饮水与休息提醒复用定时任务能力。",
                         kHealthMetrics, sizeof(kHealthMetrics) / sizeof(kHealthMetrics[0]),
                         kHealthActions, kHealthDescriptions, kHealthIcons,
                         sizeof(kHealthActions) / sizeof(kHealthActions[0]),
                         "坐姿模型或摄像头不可用时显示异常状态，不影响饮水和休息提醒。");
}

void LcdDisplay::ShowDeviceSettings() {
    DisplayLockGuard lock(this);
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    auto icon_font = theme->icon_font()->font();

    if (app_detail_layer_ != nullptr) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_ != nullptr) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);

    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 10, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);

    CreateAppHeader(app_detail_layer_, "设备设置", nullptr, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* body = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(body, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    auto make_card = [&](lv_coord_t height) {
        lv_obj_t* card = lv_obj_create(body);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, height);
        app_ui::StyleCard(card);
        lv_obj_set_style_pad_all(card, 14, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    };
    auto make_title = [&](lv_obj_t* parent, const char* icon_text, const char* title) {
        lv_obj_t* icon = lv_label_create(parent);
        lv_label_set_text(icon, icon_text);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(app_ui::kBrand), 0);
        lv_obj_set_pos(icon, 0, 1);
        lv_obj_t* label = lv_label_create(parent);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, text_font, 0);
        app_ui::StyleCardTitle(label);
        lv_obj_set_style_text_color(label, lv_color_hex(kTextPrimary), 0);
        lv_obj_set_pos(label, 34, 0);
    };

    lv_obj_t* wifi = make_card(88);
    lv_obj_add_flag(wifi, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi, OnDeviceWifiClicked, LV_EVENT_CLICKED, this);
    make_title(wifi, FONT_AWESOME_WIFI, "网络设置");
    lv_obj_t* wifi_desc = lv_label_create(wifi);
    lv_label_set_text(wifi_desc, "无网络时也可进入，点击启动配网");
    app_ui::StyleBody(wifi_desc);
    lv_obj_set_style_text_color(wifi_desc, lv_color_hex(kTextSecondary), 0);
    lv_obj_set_pos(wifi_desc, 34, 34);
    lv_obj_clear_flag(wifi_desc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* wifi_arrow = lv_label_create(wifi);
    lv_label_set_text(wifi_arrow, FONT_AWESOME_ANGLE_RIGHT);
    lv_obj_set_style_text_font(wifi_arrow, icon_font, 0);
    lv_obj_set_style_text_color(wifi_arrow, lv_color_hex(kTextSecondary), 0);
    lv_obj_align(wifi_arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(wifi_arrow, LV_OBJ_FLAG_CLICKABLE);

    auto make_slider_card = [&](const char* icon_text, const char* title, int value,
                                lv_event_cb_t callback) {
        lv_obj_t* card = make_card(112);
        make_title(card, icon_text, title);
        lv_obj_t* value_label = lv_label_create(card);
        lv_label_set_text_fmt(value_label, "%d%%", value);
        lv_obj_set_style_text_font(value_label, text_font, 0);
        lv_obj_set_style_text_color(value_label, lv_color_hex(app_ui::kBrand), 0);
        lv_obj_align(value_label, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_t* slider = lv_slider_create(card);
        lv_obj_set_size(slider, LV_HOR_RES - kPagePad * 2 - 28, 18);
        lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -7);
        lv_slider_set_range(slider, 5, 100);
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(slider, lv_color_hex(0xE4ECE8), LV_PART_MAIN);
        lv_obj_set_style_bg_color(slider, lv_color_hex(app_ui::kBrand), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
        lv_obj_set_style_border_color(slider, lv_color_hex(app_ui::kBrand), LV_PART_KNOB);
        lv_obj_set_user_data(slider, value_label);
        // Persist only when the finger is released to avoid excessive NVS writes.
        lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, nullptr);
    };

    auto* codec = Board::GetInstance().GetAudioCodec();
    make_slider_card(FONT_AWESOME_VOLUME_HIGH, "声音大小",
                     codec != nullptr ? codec->output_volume() : 70, OnDeviceVolumeChanged);
    auto* backlight = Board::GetInstance().GetBacklight();
    make_slider_card(FONT_AWESOME_BRIGHTNESS, "屏幕亮度",
                     backlight != nullptr ? backlight->brightness() : 80, OnDeviceBrightnessChanged);

    lv_obj_t* care = make_card(108);
    make_title(care, FONT_AWESOME_HEART, "关怀模式");
    lv_obj_t* care_desc = lv_label_create(care);
    lv_label_set_text(care_desc, "定时检测在场状态和坐姿，异常时提醒");
    app_ui::StyleBody(care_desc);
    lv_obj_set_width(care_desc, LV_HOR_RES - kPagePad * 2 - 112);
    lv_label_set_long_mode(care_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(care_desc, lv_color_hex(kTextSecondary), 0);
    lv_obj_set_pos(care_desc, 34, 35);
    lv_obj_t* care_switch = lv_switch_create(care);
    lv_obj_set_size(care_switch, 58, 32);
    lv_obj_align(care_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    const auto care_config = Application::GetInstance().GetEyeCareConfig();
    if (care_config.enabled && care_config.posture_enabled) {
        lv_obj_add_state(care_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(care_switch, OnCareModeChanged, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t* privacy = make_card(94);
    lv_obj_set_style_bg_color(privacy, lv_color_hex(0xEEF7F4), 0);
    make_title(privacy, FONT_AWESOME_LOCK, "隐私与性能");
    lv_obj_t* privacy_desc = lv_label_create(privacy);
    lv_label_set_text(privacy_desc, "关怀模式关闭时不进行后台摄像头检测；无网络时仅保留本地设置。");
    app_ui::StyleBody(privacy_desc);
    lv_obj_set_width(privacy_desc, LV_HOR_RES - kPagePad * 2 - 28);
    lv_label_set_long_mode(privacy_desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(privacy_desc, lv_color_hex(kTextSecondary), 0);
    lv_obj_set_pos(privacy_desc, 0, 34);

    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowMoreApps() {
    ShowFeatureDashboard("更多", "模块入口", 0x5F5B6B,
                         "扩展中心", "成长档案、家长控制、固件升级等模块先以可点击入口展示，后续按注册表扩展。",
                         kMoreMetrics, sizeof(kMoreMetrics) / sizeof(kMoreMetrics[0]),
                         kMoreActions, kMoreDescriptions, kMoreIcons,
                         sizeof(kMoreActions) / sizeof(kMoreActions[0]),
                         "未完成模块显示开发中状态，但入口、返回和反馈保持一致。");
}

void LcdDisplay::ShowPomodoroTimer() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();

    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    PomodoroApplyElapsed();
    g_pomodoro_time_label = nullptr;
    g_pomodoro_state_label = nullptr;
    g_pomodoro_start_label = nullptr;
    g_pomodoro_done_label = nullptr;
    g_pomodoro_minutes_label = nullptr;
    g_pomodoro_arc = nullptr;
    for (auto& button : g_pomodoro_mode_buttons) {
        button = nullptr;
    }

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 0, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* top_glow = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(top_glow, 360, 190);
    lv_obj_align(top_glow, LV_ALIGN_TOP_MID, 0, -110);
    lv_obj_set_style_radius(top_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(top_glow, lv_color_hex(0xDDF5E6), 0);
    lv_obj_set_style_bg_opa(top_glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(top_glow, 0, 0);
    lv_obj_clear_flag(top_glow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* bottom_glow = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(bottom_glow, 390, 230);
    lv_obj_align(bottom_glow, LV_ALIGN_BOTTOM_MID, 0, 135);
    lv_obj_set_style_radius(bottom_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bottom_glow, lv_color_hex(0xFFE8E1), 0);
    lv_obj_set_style_bg_opa(bottom_glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(bottom_glow, 0, 0);
    lv_obj_clear_flag(bottom_glow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* back = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(back, 56, 56);
    lv_obj_set_pos(back, 16, 18);
    lv_obj_set_style_radius(back, 18, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(kSurfaceCard), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(kBorderSoft), 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, FONT_AWESOME_ANGLE_LEFT);
    lv_obj_set_style_text_font(back_icon, icon_font, 0);
    lv_obj_set_style_transform_scale(back_icon, 310, 0);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(kTextPrimary), 0);
    lv_obj_center(back_icon);

    lv_obj_t* title = lv_label_create(app_detail_layer_);
    lv_label_set_text(title, "任务专注计时");
    lv_obj_set_width(title, 300);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, text_font, 0);
    app_ui::StylePageTitle(title);
    lv_obj_set_style_text_color(title, lv_color_hex(kTextPrimary), 0);

    lv_obj_t* mode_panel = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(mode_panel, LV_HOR_RES - 36, 54);
    lv_obj_set_pos(mode_panel, 18, 88);
    lv_obj_set_style_radius(mode_panel, 18, 0);
    lv_obj_set_style_bg_color(mode_panel, lv_color_hex(kSurfaceCard), 0);
    lv_obj_set_style_bg_opa(mode_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(mode_panel, 1, 0);
    lv_obj_set_style_border_color(mode_panel, lv_color_hex(kBorderSoft), 0);
    lv_obj_set_style_pad_all(mode_panel, 5, 0);
    lv_obj_set_style_pad_column(mode_panel, 5, 0);
    lv_obj_set_flex_flow(mode_panel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mode_panel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(mode_panel, LV_OBJ_FLAG_SCROLLABLE);

    const char* mode_names[] = {"专注", "短休", "长休"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* mode = lv_obj_create(mode_panel);
        g_pomodoro_mode_buttons[i] = mode;
        lv_obj_set_size(mode, (LV_HOR_RES - 56) / 3, 42);
        lv_obj_set_style_radius(mode, 14, 0);
        lv_obj_set_style_border_width(mode, 0, 0);
        lv_obj_set_style_pad_all(mode, 0, 0);
        lv_obj_set_style_shadow_width(mode, 0, 0);
        lv_obj_add_flag(mode, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(mode, (void*)(intptr_t)i);
        lv_obj_add_event_cb(mode, OnPomodoroModeClicked, LV_EVENT_CLICKED, this);
        lv_obj_t* label = lv_label_create(mode);
        lv_label_set_text(label, mode_names[i]);
        lv_obj_set_style_text_font(label, text_font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(kTextSecondary), 0);
        lv_obj_center(label);
    }

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - 36, 350);
    lv_obj_set_pos(hero, 18, 164);
    lv_obj_set_style_radius(hero, 28, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(kSurfaceCard), 0);
    lv_obj_set_style_bg_opa(hero, LV_OPA_90, 0);
    lv_obj_set_style_border_width(hero, 1, 0);
    lv_obj_set_style_border_color(hero, lv_color_hex(kBorderSoft), 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 0, 0);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    g_pomodoro_arc = lv_arc_create(hero);
    lv_obj_set_size(g_pomodoro_arc, 322, 322);
    lv_obj_align(g_pomodoro_arc, LV_ALIGN_TOP_MID, 0, 18);
    lv_arc_set_rotation(g_pomodoro_arc, 270);
    lv_arc_set_bg_angles(g_pomodoro_arc, 0, 360);
    lv_obj_remove_style(g_pomodoro_arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(g_pomodoro_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_pomodoro_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_pomodoro_arc, lv_color_hex(0xE8F1EA), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_pomodoro_arc, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_pomodoro_arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_pomodoro_arc, lv_color_hex(app_ui::kBrand), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_pomodoro_arc, true, LV_PART_INDICATOR);

    lv_obj_t* tomato_glow = lv_obj_create(hero);
    lv_obj_set_size(tomato_glow, 278, 278);
    lv_obj_align(tomato_glow, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_radius(tomato_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tomato_glow, lv_color_hex(0xE53325), 0);
    lv_obj_set_style_bg_opa(tomato_glow, LV_OPA_20, 0);
    lv_obj_set_style_border_width(tomato_glow, 0, 0);
    lv_obj_clear_flag(tomato_glow, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* tomato = lv_obj_create(hero);
    lv_obj_set_size(tomato, 250, 250);
    lv_obj_align(tomato, LV_ALIGN_TOP_MID, 0, 55);
    lv_obj_set_style_radius(tomato, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tomato, lv_color_hex(0xC91F18), 0);
    lv_obj_set_style_bg_grad_color(tomato, lv_color_hex(0xFF5138), 0);
    lv_obj_set_style_bg_grad_dir(tomato, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(tomato, 1, 0);
    lv_obj_set_style_border_color(tomato, lv_color_hex(0xFF6A51), 0);
    lv_obj_set_style_shadow_width(tomato, 0, 0);
    lv_obj_clear_flag(tomato, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* highlight = lv_obj_create(tomato);
    lv_obj_set_size(highlight, 120, 62);
    lv_obj_align(highlight, LV_ALIGN_TOP_LEFT, 26, 28);
    lv_obj_set_style_radius(highlight, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(highlight, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(highlight, LV_OPA_20, 0);
    lv_obj_set_style_border_width(highlight, 0, 0);
    lv_obj_set_style_transform_angle(highlight, -140, 0);
    lv_obj_clear_flag(highlight, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 5; ++i) {
        lv_obj_t* leaf = lv_obj_create(hero);
        lv_obj_set_size(leaf, 54, 20);
        lv_obj_align(leaf, LV_ALIGN_TOP_MID, (i - 2) * 19, 44 + (i % 2) * 5);
        lv_obj_set_style_radius(leaf, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(leaf, lv_color_hex(i % 2 ? 0x4A8C3E : 0x68A64E), 0);
        lv_obj_set_style_border_width(leaf, 0, 0);
        lv_obj_set_style_transform_angle(leaf, (i - 2) * 160, 0);
        lv_obj_clear_flag(leaf, LV_OBJ_FLAG_SCROLLABLE);
    }

    g_pomodoro_state_label = lv_label_create(tomato);
    lv_obj_set_style_text_font(g_pomodoro_state_label, text_font, 0);
    lv_obj_set_style_text_color(g_pomodoro_state_label, lv_color_hex(0xFFE5DF), 0);
    lv_obj_align(g_pomodoro_state_label, LV_ALIGN_CENTER, 0, -48);

    g_pomodoro_time_label = lv_label_create(tomato);
    lv_obj_set_style_text_font(g_pomodoro_time_label, text_font, 0);
    lv_obj_set_style_transform_scale(g_pomodoro_time_label, 390, 0);
    lv_obj_set_style_text_color(g_pomodoro_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_letter_space(g_pomodoro_time_label, 2, 0);
    lv_obj_align(g_pomodoro_time_label, LV_ALIGN_CENTER, 0, 2);

    lv_obj_t* mode_label = lv_label_create(tomato);
    lv_label_set_text(mode_label, PomodoroModeName(g_pomodoro.mode));
    lv_obj_set_style_text_font(mode_label, text_font, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xFFD3CA), 0);
    lv_obj_align(mode_label, LV_ALIGN_CENTER, 0, 54);

    lv_obj_t* reset = lv_obj_create(tomato);
    lv_obj_set_size(reset, 104, 38);
    lv_obj_align(reset, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(reset, 20, 0);
    lv_obj_set_style_bg_color(reset, lv_color_hex(0xA81714), 0);
    lv_obj_set_style_bg_opa(reset, LV_OPA_70, 0);
    lv_obj_set_style_border_width(reset, 1, 0);
    lv_obj_set_style_border_color(reset, lv_color_hex(0xF36A58), 0);
    lv_obj_set_style_shadow_width(reset, 0, 0);
    lv_obj_add_flag(reset, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(reset, OnPomodoroResetClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* reset_label = lv_label_create(reset);
    lv_label_set_text(reset_label, "重置");
    lv_obj_set_style_text_font(reset_label, text_font, 0);
    lv_obj_set_style_text_color(reset_label, lv_color_hex(0xFFF1ED), 0);
    lv_obj_center(reset_label);

    lv_obj_t* start = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(start, 316, 64);
    lv_obj_set_pos(start, (LV_HOR_RES - 316) / 2, 536);
    lv_obj_set_style_radius(start, 32, 0);
    lv_obj_set_style_bg_color(start, lv_color_hex(app_ui::kHighlight), 0);
    lv_obj_set_style_bg_grad_color(start, lv_color_hex(app_ui::kBrand), 0);
    lv_obj_set_style_bg_grad_dir(start, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(start, 1, 0);
    lv_obj_set_style_border_color(start, lv_color_hex(0xA8DEA9), 0);
    lv_obj_set_style_shadow_width(start, 0, 0);
    lv_obj_add_flag(start, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(start, OnPomodoroStartClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* start_row = lv_obj_create(start);
    lv_obj_set_size(start_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(start_row);
    lv_obj_set_style_bg_opa(start_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(start_row, 0, 0);
    lv_obj_set_style_pad_all(start_row, 0, 0);
    lv_obj_set_style_pad_column(start_row, 12, 0);
    lv_obj_set_flex_flow(start_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(start_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(start_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* play_icon = lv_label_create(start_row);
    lv_label_set_text(play_icon, FONT_AWESOME_PLAY);
    lv_obj_set_style_text_font(play_icon, icon_font, 0);
    lv_obj_set_style_text_color(play_icon, lv_color_white(), 0);
    g_pomodoro_start_label = lv_label_create(start_row);
    lv_obj_set_style_text_font(g_pomodoro_start_label, text_font, 0);
    app_ui::StyleButtonLabel(g_pomodoro_start_label);
    lv_obj_set_style_text_color(g_pomodoro_start_label, lv_color_white(), 0);

    lv_obj_t* stats = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(stats, LV_HOR_RES - 36, 94);
    lv_obj_set_pos(stats, 18, 628);
    lv_obj_set_style_radius(stats, 22, 0);
    lv_obj_set_style_bg_color(stats, lv_color_hex(kSurfaceCard), 0);
    lv_obj_set_style_bg_opa(stats, LV_OPA_90, 0);
    lv_obj_set_style_border_width(stats, 1, 0);
    lv_obj_set_style_border_color(stats, lv_color_hex(kBorderSoft), 0);
    lv_obj_set_style_pad_all(stats, 14, 0);
    lv_obj_set_style_shadow_width(stats, 0, 0);
    lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* stats_title = lv_label_create(stats);
    lv_label_set_text(stats_title, FONT_AWESOME_CLOCK " 今日");
    lv_obj_set_style_text_font(stats_title, text_font, 0);
    lv_obj_set_style_text_color(stats_title, lv_color_hex(kTextPrimary), 0);
    lv_obj_align(stats_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* divider = lv_obj_create(stats);
    lv_obj_set_size(divider, 1, 54);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(kBorderSoft), 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    lv_obj_t* done_title = lv_label_create(stats);
    lv_label_set_text(done_title, "完成");
    lv_obj_set_style_text_color(done_title, lv_color_hex(kTextSecondary), 0);
    lv_obj_align(done_title, LV_ALIGN_BOTTOM_LEFT, 4, -32);
    g_pomodoro_done_label = lv_label_create(stats);
    lv_obj_set_style_text_font(g_pomodoro_done_label, text_font, 0);
    lv_obj_set_style_transform_scale(g_pomodoro_done_label, 170, 0);
    lv_obj_set_style_text_color(g_pomodoro_done_label, lv_color_hex(kTextPrimary), 0);
    lv_obj_align(g_pomodoro_done_label, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    lv_obj_t* minute_title = lv_label_create(stats);
    lv_label_set_text(minute_title, "时长");
    lv_obj_set_style_text_color(minute_title, lv_color_hex(kTextSecondary), 0);
    lv_obj_align(minute_title, LV_ALIGN_BOTTOM_MID, 108, -32);
    g_pomodoro_minutes_label = lv_label_create(stats);
    lv_obj_set_style_text_font(g_pomodoro_minutes_label, text_font, 0);
    lv_obj_set_style_transform_scale(g_pomodoro_minutes_label, 170, 0);
    lv_obj_set_style_text_color(g_pomodoro_minutes_label, lv_color_hex(kTextPrimary), 0);
    lv_obj_align(g_pomodoro_minutes_label, LV_ALIGN_BOTTOM_MID, 108, 0);

    UpdatePomodoroUi();
    pomodoro_timer_ = lv_timer_create([](lv_timer_t* timer) {
        (void)timer;
        if (g_pomodoro_time_label == nullptr) {
            return;
        }
        UpdatePomodoroUi();
    }, 1000, this);

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowTaskScheduler() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(kSurfaceBg), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 10, 0);

    CreateAppHeader(app_detail_layer_, "任务计划", nullptr, lv_color_hex(0xE2EBE6),
                    lv_color_hex(0x183B34), lv_color_hex(0x60736B));

    cJSON* reminders = Application::GetInstance().GetRemindersJson();
    cJSON* tasks = reminders != nullptr ? cJSON_GetObjectItem(reminders, "tasks") : nullptr;

    lv_obj_t* list_header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list_header, LV_HOR_RES - kPagePad * 2, 46);
    lv_obj_set_style_bg_opa(list_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_header, 0, 0);
    lv_obj_set_style_pad_all(list_header, 0, 0);
    lv_obj_set_flex_flow(list_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* list_title = lv_label_create(list_header);
    lv_label_set_text(list_title, "全部任务");
    lv_obj_set_style_text_font(list_title, text_font, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(0x15231F), 0);

    lv_obj_t* add_button = lv_obj_create(list_header);
    lv_obj_set_size(add_button, 126, 38);
    lv_obj_set_style_radius(add_button, kCardRadius, 0);
    lv_obj_set_style_bg_color(add_button, lv_color_hex(0xE2EBE6), 0);
    lv_obj_set_style_border_width(add_button, 0, 0);
    lv_obj_add_flag(add_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add_button, OnAppActionClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* add_label = lv_label_create(add_button);
    lv_label_set_text(add_label, "语音添加");
    lv_obj_set_style_text_color(add_label, lv_color_hex(0x1D6B5F), 0);
    lv_obj_center(add_label);

    lv_obj_t* list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list, LV_HOR_RES - kPagePad * 2, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    int rendered = 0;
    if (cJSON_IsArray(tasks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tasks) {
            if (rendered >= 8) {
                break;
            }
            auto remind_at = cJSON_GetObjectItem(item, "remind_at");
            auto content = cJSON_GetObjectItem(item, "content");
            auto fired = cJSON_GetObjectItem(item, "fired");
            if (!cJSON_IsNumber(remind_at) || !cJSON_IsString(content)) {
                continue;
            }

            const bool is_fired = cJSON_IsBool(fired) && fired->valueint == 1;
            int64_t remind_ts = (int64_t)remind_at->valuedouble;
            struct tm tm_time = {};
            time_t remind_time_value = (time_t)remind_ts;
            localtime_r(&remind_time_value, &tm_time);
            char time_text[32];
            strftime(time_text, sizeof(time_text), "%m/%d %H:%M", &tm_time);

            lv_obj_t* row = lv_obj_create(list);
            lv_obj_set_size(row, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
            lv_obj_set_style_min_height(row, 82, 0);
            lv_obj_set_style_radius(row, kCardRadius, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(kSurfaceCard), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(kBorderSoft), 0);
            lv_obj_set_style_pad_all(row, 10, 0);
            lv_obj_set_style_shadow_width(row, 0, 0);
            lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t* state = lv_obj_create(row);
            lv_obj_set_size(state, 48, 48);
            lv_obj_set_style_radius(state, kCardRadius, 0);
            lv_obj_set_style_bg_color(state, is_fired ? lv_color_hex(0xE6EAE8) : lv_color_hex(0xDFF4EC), 0);
            lv_obj_set_style_border_width(state, 0, 0);
            lv_obj_t* state_icon = lv_label_create(state);
            lv_label_set_text(state_icon, is_fired ? FONT_AWESOME_CHECK : FONT_AWESOME_CLOCK);
            lv_obj_set_style_text_font(state_icon, icon_font, 0);
            lv_obj_set_style_text_color(state_icon, is_fired ? lv_color_hex(0x7E8B86) : lv_color_hex(0x1D6B5F), 0);
            lv_obj_center(state_icon);

            lv_obj_t* text_box = lv_obj_create(row);
            lv_obj_set_size(text_box, LV_HOR_RES - kPagePad * 2 - 112, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(text_box, 0, 0);
            lv_obj_set_style_pad_all(text_box, 0, 0);
            lv_obj_set_style_margin_left(text_box, 10, 0);
            lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

            lv_obj_t* row_title = lv_label_create(text_box);
            lv_label_set_text(row_title, content->valuestring);
            lv_obj_set_width(row_title, LV_HOR_RES - kPagePad * 2 - 122);
            lv_label_set_long_mode(row_title, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(row_title, lv_color_hex(0x15231F), 0);

            lv_obj_t* row_time = lv_label_create(text_box);
            lv_label_set_text(row_time, time_text);
            lv_obj_set_style_text_color(row_time, is_fired ? lv_color_hex(0x8CA099) : lv_color_hex(0x1D6B5F), 0);
            rendered++;
        }
    }

    if (rendered == 0) {
        lv_obj_t* empty = lv_obj_create(list);
        lv_obj_set_size(empty, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(empty, 110, 0);
        lv_obj_set_style_radius(empty, kCardRadius, 0);
        lv_obj_set_style_bg_color(empty, lv_color_hex(kSurfaceCard), 0);
        lv_obj_set_style_border_width(empty, 1, 0);
        lv_obj_set_style_border_color(empty, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_all(empty, 12, 0);
        lv_obj_set_style_shadow_width(empty, 0, 0);
        lv_obj_set_scrollbar_mode(empty, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* empty_icon = lv_label_create(empty);
        lv_label_set_text(empty_icon, FONT_AWESOME_BELL);
        lv_obj_set_style_text_font(empty_icon, icon_font, 0);
        lv_obj_set_style_text_color(empty_icon, lv_color_hex(0x1D6B5F), 0);

        lv_obj_t* empty_text = lv_label_create(empty);
        lv_label_set_text(empty_text, "暂无任务");
        lv_obj_set_style_text_font(empty_text, text_font, 0);
        lv_obj_set_style_text_color(empty_text, lv_color_hex(0x15231F), 0);

        lv_obj_t* empty_hint = lv_label_create(empty);
        lv_label_set_text(empty_hint, "可以通过语音添加新任务");
        lv_obj_set_width(empty_hint, LV_HOR_RES - 72);
        lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty_hint, lv_color_hex(0x60736B), 0);
    }

    if (reminders != nullptr) {
        cJSON_Delete(reminders);
    }
    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowTaskStats() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = theme->text_font()->font();
    auto icon_font = theme->icon_font()->font();
    if (app_detail_layer_) lv_obj_del(app_detail_layer_);
    if (app_grid_layer_) lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    app_detail_layer_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_white(), 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    CreateAppHeader(app_detail_layer_, "任务统计", nullptr, lv_color_white(),
                    lv_color_hex(kTextPrimary), lv_color_hex(kTextSecondary));

    lv_obj_t* tabs = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(tabs, 260, 42);
    lv_obj_set_style_radius(tabs, 21, 0);
    lv_obj_set_style_bg_color(tabs, lv_color_hex(0xF4F6F7), 0);
    lv_obj_set_style_border_width(tabs, 0, 0);
    lv_obj_set_style_pad_all(tabs, 4, 0);
    lv_obj_set_style_pad_column(tabs, 4, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    const char* periods[] = {"日", "周", "月"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* tab = lv_obj_create(tabs);
        lv_obj_set_size(tab, 80, 34);
        lv_obj_set_style_radius(tab, 17, 0);
        lv_obj_set_style_bg_color(tab, lv_color_hex(i == 0 ? 0xE4F5DF : 0xF4F6F7), 0);
        lv_obj_set_style_border_width(tab, 0, 0);
        lv_obj_set_style_pad_all(tab, 0, 0);
        lv_obj_t* t = lv_label_create(tab); lv_label_set_text(t, periods[i]);
        lv_obj_set_style_text_color(t, lv_color_hex(i == 0 ? 0x43A33C : 0x697571), 0);
        lv_obj_center(t);
    }
    lv_obj_t* date = lv_label_create(app_detail_layer_); lv_label_set_text(date, "2024年5月20日");
    lv_obj_set_style_text_color(date, lv_color_hex(kTextSecondary), 0);
    lv_obj_t* total = lv_label_create(app_detail_layer_); lv_label_set_text(total, "4 时 25 分钟");
    lv_obj_set_style_text_font(total, text_font, 0);
    lv_obj_set_style_transform_scale(total, 220, 0);
    lv_obj_set_style_text_color(total, lv_color_hex(kTextPrimary), 0);

    lv_obj_t* chart = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(chart, LV_HOR_RES - kPagePad * 2, 250);
    lv_obj_set_style_radius(chart, 18, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0xFBFCFC), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0xEEF0F1), 0);
    lv_obj_set_style_pad_all(chart, 16, 0);
    lv_obj_set_style_pad_column(chart, 9, 0);
    lv_obj_set_flex_flow(chart, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    const int heights[] = {28, 72, 130, 84, 164, 116, 142, 70, 112, 46};
    for (int i = 0; i < 10; ++i) {
        lv_obj_t* bar = lv_obj_create(chart);
        lv_obj_set_size(bar, 22, heights[i]);
        lv_obj_set_style_radius(bar, 7, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(i % 3 == 0 ? 0xF06C52 : 0x54BE69), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
    }

    lv_obj_t* summary = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(summary, LV_HOR_RES - kPagePad * 2, 96);
    lv_obj_set_style_bg_opa(summary, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(summary, 0, 0);
    lv_obj_set_style_pad_all(summary, 0, 0);
    lv_obj_set_style_pad_column(summary, 8, 0);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_ROW);
    const char* stat_titles[] = {"完成任务", "番茄数量", "平均专注"};
    const char* stat_values[] = {"6 个", "9 个", "28 分钟"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* card = lv_obj_create(summary);
        lv_obj_set_size(card, (LV_HOR_RES - kPagePad * 2 - 16) / 3, 92);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xF8FAFB), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xE8ECEE), 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t* a = lv_label_create(card); lv_label_set_text(a, stat_titles[i]);
        lv_obj_set_style_text_color(a, lv_color_hex(kTextSecondary), 0);
        lv_obj_t* b = lv_label_create(card); lv_label_set_text(b, stat_values[i]);
        lv_obj_set_style_text_font(b, text_font, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(kTextPrimary), 0);
    }
    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowPetGarden() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();
    const std::string current_style = CurrentPetStyleName();
    Settings habit_settings("pet_habit", true);
    const int32_t today = static_cast<int32_t>(time(nullptr) / 86400);
    const int32_t saved_day = habit_settings.GetInt("day", -1);
    if (saved_day != today) {
        habit_settings.SetInt("day", today);
        habit_settings.SetInt("daily_mask", 0);
    }
    g_pet_daily_mask = habit_settings.GetInt("daily_mask", 0) & 0x07;
    g_pet_growth = habit_settings.GetInt("growth", 0);
    g_pet_streak = habit_settings.GetInt("streak", 0);

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF6F3EE), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(app_detail_layer_, 10, 0);

    CreateAppHeader(app_detail_layer_, "宠物花园", "习惯养成", lv_color_hex(0xEFE7DC),
                    lv_color_hex(0x2B211A), lv_color_hex(0x7B6A5A));

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hero, 156, 0);
    lv_obj_set_style_radius(hero, kCardRadius, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x6E4F35), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_style_shadow_width(hero, 0, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* avatar = lv_obj_create(hero);
    lv_obj_set_size(avatar, 104, 104);
    lv_obj_set_style_radius(avatar, kCardRadius, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(0xF4D3A0), 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    auto pet_collection = lvgl_theme->emoji_collection();
    const LvglImage* pet_image = pet_collection ? pet_collection->GetEmojiImage("app_pet") : nullptr;
    if (pet_image != nullptr) {
        lv_obj_t* avatar_image = lv_image_create(avatar);
        lv_image_set_src(avatar_image, pet_image->image_dsc());
        lv_image_set_scale(avatar_image, 150);
        lv_obj_center(avatar_image);
    } else {
        lv_obj_t* avatar_icon = lv_label_create(avatar);
        lv_label_set_text(avatar_icon, FONT_AWESOME_GAMEPAD);
        lv_obj_set_style_text_font(avatar_icon, icon_font, 0);
        lv_obj_set_style_text_color(avatar_icon, lv_color_hex(0x6E4F35), 0);
        lv_obj_center(avatar_icon);
    }

    lv_obj_t* hero_text = lv_obj_create(hero);
    lv_obj_set_size(hero_text, LV_HOR_RES - kPagePad * 2 - 144, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hero_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hero_text, 0, 0);
    lv_obj_set_style_pad_all(hero_text, 0, 0);
    lv_obj_set_style_margin_left(hero_text, 14, 0);
    lv_obj_set_flex_flow(hero_text, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* pet_name = lv_label_create(hero_text);
    lv_label_set_text(pet_name, current_style == "jinglingshu" ? "精灵鼠" : current_style.c_str());
    lv_obj_set_style_text_font(pet_name, text_font, 0);
    app_ui::StylePageTitle(pet_name);
    lv_obj_set_style_text_color(pet_name, lv_color_white(), 0);

    lv_obj_t* pet_state = lv_label_create(hero_text);
    lv_label_set_text(pet_state, "陪你养成好习惯");
    lv_obj_set_style_text_color(pet_state, lv_color_hex(0xF6DDB9), 0);
    lv_obj_set_style_margin_top(pet_state, 4, 0);

    g_pet_growth_label = lv_label_create(hero_text);
    lv_obj_set_style_text_color(g_pet_growth_label, lv_color_hex(0xFFF4DF), 0);
    lv_obj_set_style_margin_top(g_pet_growth_label, 8, 0);
    g_pet_progress_label = lv_label_create(hero_text);
    lv_obj_set_width(g_pet_progress_label, LV_HOR_RES - kPagePad * 2 - 154);
    lv_label_set_long_mode(g_pet_progress_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_pet_progress_label, lv_color_hex(0xF6DDB9), 0);
    lv_obj_set_style_margin_top(g_pet_progress_label, 4, 0);

    lv_obj_t* skin_button = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(skin_button, LV_HOR_RES - kPagePad * 2, 52);
    lv_obj_set_style_radius(skin_button, 18, 0);
    lv_obj_set_style_bg_color(skin_button, lv_color_hex(0xFFF2D7), 0);
    lv_obj_set_style_border_width(skin_button, 1, 0);
    lv_obj_set_style_border_color(skin_button, lv_color_hex(0xE3C99F), 0);
    lv_obj_set_style_pad_left(skin_button, 16, 0);
    lv_obj_set_style_pad_right(skin_button, 16, 0);
    lv_obj_set_style_shadow_width(skin_button, 0, 0);
    lv_obj_clear_flag(skin_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(skin_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(skin_button, OnPetSkinsOpenClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* skin_button_label = lv_label_create(skin_button);
    lv_label_set_text(skin_button_label, FONT_AWESOME_IMAGE "  更换皮肤");
    lv_obj_set_style_text_font(skin_button_label, text_font, 0);
    lv_obj_set_style_text_color(skin_button_label, lv_color_hex(0x7A4B25), 0);
    lv_obj_center(skin_button_label);
    lv_obj_clear_flag(skin_button_label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* habit_title = lv_label_create(app_detail_layer_);
    lv_label_set_text(habit_title, "今日习惯");
    lv_obj_set_style_text_font(habit_title, text_font, 0);
    app_ui::StyleSectionTitle(habit_title);
    lv_obj_set_style_text_color(habit_title, lv_color_hex(0x2B211A), 0);

    const char* habit_names[] = {"喝水", "专注 25 分钟", "睡前整理"};
    const char* habit_desc[] = {"完成一次主动饮水", "安静完成一轮专注", "整理桌面，准备休息"};
    const char* habit_icons[] = {FONT_AWESOME_HEART, FONT_AWESOME_CLOCK, FONT_AWESOME_STAR};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* card = lv_obj_create(app_detail_layer_);
        g_pet_habit_cards[i] = card;
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, 82);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xE5DDD3), 0);
        lv_obj_set_style_pad_left(card, 14, 0);
        lv_obj_set_style_pad_right(card, 14, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, OnPetHabitClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));

        lv_obj_t* icon = lv_label_create(card);
        lv_label_set_text(icon, habit_icons[i]);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xA66D3F), 0);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, 250, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 14, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_t* name = lv_label_create(text_box);
        lv_label_set_text(name, habit_names[i]);
        app_ui::StyleCardTitle(name);
        lv_obj_set_style_text_color(name, lv_color_hex(0x2B211A), 0);
        lv_obj_t* desc = lv_label_create(text_box);
        lv_label_set_text(desc, habit_desc[i]);
        app_ui::StyleCaption(desc);

        g_pet_habit_states[i] = lv_label_create(card);
        lv_obj_set_style_margin_left(g_pet_habit_states[i], 8, 0);
    }
    UpdatePetHabitUi();

#if 0

    lv_obj_t* section_title = lv_label_create(app_detail_layer_);
    lv_label_set_text(section_title, "皮肤");
    lv_obj_set_style_text_font(section_title, text_font, 0);
    lv_obj_set_style_text_color(section_title, lv_color_hex(0x2B211A), 0);
    lv_obj_set_style_margin_top(section_title, 12, 0);

    lv_obj_t* skin_list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(skin_list, LV_HOR_RES - kPagePad * 2, 206);
    lv_obj_set_style_bg_opa(skin_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(skin_list, 0, 0);
    lv_obj_set_style_pad_all(skin_list, 0, 0);
    lv_obj_set_style_pad_row(skin_list, 10, 0);
    lv_obj_set_flex_flow(skin_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(skin_list, LV_DIR_VER);

    for (const auto& skin : kPetSkinPresets) {
        const bool active = current_style == skin.style_id;
        const bool installed = FileExists(skin.directory);
        lv_obj_t* card = lv_obj_create(skin_list);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(card, 88, 0);
        lv_obj_set_style_radius(card, kCardRadius, 0);
        lv_obj_set_style_bg_color(card, active ? lv_color_hex(0xFFF2D7) : (installed ? lv_color_white() : lv_color_hex(0xF1ECE5)), 0);
        lv_obj_set_style_border_width(card, active ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, active ? lv_color_hex(0xC88932) : lv_color_hex(0xE3D8CA), 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, const_cast<PetSkinPreset*>(&skin));
        lv_obj_add_event_cb(card, OnPetSkinClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 54, 54);
        lv_obj_set_style_radius(icon_box, kCardRadius, 0);
        lv_obj_set_style_bg_color(icon_box, active ? lv_color_hex(0xC88932) : lv_color_hex(0xEFE7DC), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, active ? FONT_AWESOME_CHECK : (installed ? FONT_AWESOME_GAMEPAD : FONT_AWESOME_CLOCK));
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, active ? lv_color_white() : lv_color_hex(0x6E4F35), 0);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - kPagePad * 2 - 120, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* skin_title = lv_label_create(text_box);
        lv_label_set_text(skin_title, skin.title);
        lv_obj_set_style_text_font(skin_title, text_font, 0);
        app_ui::StyleCardTitle(skin_title);
        lv_obj_set_style_text_color(skin_title, lv_color_hex(0x2B211A), 0);

        lv_obj_t* skin_subtitle = lv_label_create(text_box);
        char skin_subtitle_text[128];
        snprintf(skin_subtitle_text, sizeof(skin_subtitle_text), "%s", installed ? "可用" : "待安装");
        lv_label_set_text(skin_subtitle, skin_subtitle_text);
        lv_obj_set_width(skin_subtitle, LV_HOR_RES - kPagePad * 2 - 132);
        lv_label_set_long_mode(skin_subtitle, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(skin_subtitle, lv_color_hex(0x7B6A5A), 0);
    }

    lv_obj_t* hint = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hint, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(hint, 88, 0);
    lv_obj_set_style_radius(hint, kCardRadius, 0);
    lv_obj_set_style_bg_color(hint, lv_color_hex(kSurfaceCard), 0);
    lv_obj_set_style_border_width(hint, 1, 0);
    lv_obj_set_style_border_color(hint, lv_color_hex(0xE3D8CA), 0);
    lv_obj_set_style_pad_all(hint, 12, 0);
    lv_obj_set_style_shadow_width(hint, 0, 0);
    lv_obj_set_scrollbar_mode(hint, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hint, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* hint_text = lv_label_create(hint);
    lv_label_set_text(hint_text, "新增皮肤时保持 MJPEG 文件名一致，只新增目录，例如 /sdcard/style/xiaotu/idle.mjpeg。");
    lv_obj_set_width(hint_text, LV_HOR_RES - 54);
    lv_label_set_long_mode(hint_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint_text, lv_color_hex(0x57483A), 0);
#endif

    CreateProductBottomNav(app_detail_layer_, text_font, icon_font);
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowPetSkins() {
    DisplayLockGuard lock(this);
    StopPomodoroTimer();
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    lv_obj_t* screen = lv_screen_active();
    const std::string current_style = CurrentPetStyleName();

    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_add_flag(app_grid_layer_, LV_OBJ_FLAG_HIDDEN);
    }

    app_detail_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_detail_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_detail_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF6F3EE), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, kPagePad, 0);
    lv_obj_set_style_pad_row(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(app_detail_layer_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(app_detail_layer_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(app_detail_layer_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* header = CreateAppHeader(app_detail_layer_, "更换皮肤", nullptr, lv_color_hex(0xEFE7DC),
                                       lv_color_hex(0x2B211A), lv_color_hex(0x7B6A5A));
    lv_obj_t* back = lv_obj_get_child(header, 0);
    if (back != nullptr) {
        lv_obj_remove_event_cb(back, OnAppDetailBackClicked);
        lv_obj_add_event_cb(back, OnPetSkinsBackClicked, LV_EVENT_CLICKED, this);
    }

    lv_obj_t* hint = lv_label_create(app_detail_layer_);
    lv_label_set_text(hint, "选择已安装的宠物皮肤");
    lv_obj_set_style_text_font(hint, text_font, 0);
    app_ui::StyleCaption(hint);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x7B6A5A), 0);

    lv_obj_t* list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list, LV_HOR_RES - kPagePad * 2, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    size_t installed_count = 0;
    for (const auto& skin : kPetSkinPresets) {
        if (!FileExists(skin.directory)) {
            continue;
        }
        ++installed_count;
        const bool active = current_style == skin.style_id;
        lv_obj_t* card = lv_obj_create(list);
        lv_obj_set_size(card, LV_HOR_RES - kPagePad * 2, 94);
        lv_obj_set_style_radius(card, 20, 0);
        lv_obj_set_style_bg_color(card, active ? lv_color_hex(0xFFF2D7) : lv_color_white(), 0);
        lv_obj_set_style_border_width(card, active ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, active ? lv_color_hex(0xC88932) : lv_color_hex(0xE3D8CA), 0);
        lv_obj_set_style_pad_left(card, 14, 0);
        lv_obj_set_style_pad_right(card, 14, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_user_data(card, const_cast<PetSkinPreset*>(&skin));
        lv_obj_add_event_cb(card, OnPetSkinClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 58, 58);
        lv_obj_set_style_radius(icon_box, 18, 0);
        lv_obj_set_style_bg_color(icon_box, active ? lv_color_hex(0xC88932) : lv_color_hex(0xF3E6D5), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_set_style_pad_all(icon_box, 0, 0);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, active ? FONT_AWESOME_CHECK : FONT_AWESOME_GAMEPAD);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, active ? lv_color_white() : lv_color_hex(0x7A4B25), 0);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, 286, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 14, 0);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(text_box, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_t* title = lv_label_create(text_box);
        lv_label_set_text(title, skin.title);
        lv_obj_set_style_text_font(title, text_font, 0);
        app_ui::StyleCardTitle(title);
        lv_obj_set_style_text_color(title, lv_color_hex(0x2B211A), 0);
        lv_obj_t* status = lv_label_create(text_box);
        lv_label_set_text(status, active ? "当前使用" : "点击切换");
        app_ui::StyleCaption(status);
        lv_obj_set_style_text_color(status, active ? lv_color_hex(0xA56524) : lv_color_hex(0x7B6A5A), 0);
    }

    if (installed_count == 0) {
        lv_obj_t* empty = lv_label_create(list);
        lv_label_set_text(empty, "暂无可用皮肤");
        lv_obj_set_style_text_font(empty, text_font, 0);
        app_ui::StyleCardTitle(empty);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x7B6A5A), 0);
        lv_obj_set_style_margin_top(empty, 36, 0);
        lv_obj_set_align(empty, LV_ALIGN_CENTER);
    }

    lv_obj_move_foreground(app_detail_layer_);
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left health indicator. Keep network_label_ alive but hidden for future use.
    lv_obj_t* left_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(left_icons, 128, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_icons, 0, 0);
    lv_obj_set_style_pad_all(left_icons, 0, 0);
    lv_obj_set_flex_flow(left_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(left_icons);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    health_icon_label_ = lv_label_create(left_icons);
    lv_label_set_text(health_icon_label_, FONT_AWESOME_HEART);
    lv_obj_set_width(health_icon_label_, 36);
    lv_label_set_long_mode(health_icon_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(health_icon_label_, icon_font, 0);
    lv_obj_set_style_text_align(health_icon_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(health_icon_label_, lvgl_theme->text_color(), 0);

    health_label_ = lv_label_create(left_icons);
    lv_label_set_text(health_label_, "100");
    lv_obj_set_width(health_label_, 68);
    lv_label_set_long_mode(health_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(health_label_, text_font, 0);
    lv_obj_set_style_text_align(health_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(health_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(health_label_, lvgl_theme->spacing(1), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(mute_label_, lvgl_theme->spacing(2), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.48);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.48);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    face_canvas_ = lv_canvas_create(screen);
    lv_obj_align(face_canvas_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
    face_canvas_active_ = false;

#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    CreateTouchAppLauncher(screen);
#endif
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
        }
        return;
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered fallback emotion label while chat content is visible.
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Keep the fallback emotion label hidden; MJPEG canvas is the primary face output.
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(1), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left health indicator. Keep network_label_ alive but hidden for future use.
    lv_obj_t* left_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(left_icons, 128, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_icons, 0, 0);
    lv_obj_set_style_pad_all(left_icons, 0, 0);
    lv_obj_set_flex_flow(left_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(left_icons);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);

    health_icon_label_ = lv_label_create(left_icons);
    lv_label_set_text(health_icon_label_, FONT_AWESOME_HEART);
    lv_obj_set_width(health_icon_label_, 36);
    lv_label_set_long_mode(health_icon_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(health_icon_label_, icon_font, 0);
    lv_obj_set_style_text_align(health_icon_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(health_icon_label_, lvgl_theme->text_color(), 0);

    health_label_ = lv_label_create(left_icons);
    lv_label_set_text(health_label_, "100");
    lv_obj_set_width(health_label_, 68);
    lv_label_set_long_mode(health_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(health_label_, text_font, 0);
    lv_obj_set_style_text_align(health_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(health_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(health_label_, lvgl_theme->spacing(1), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(mute_label_, lvgl_theme->spacing(2), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.48);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.48);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8) - 160);
#else
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
#endif
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
#else
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
#endif
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_align(chat_message_label_, LV_ALIGN_LEFT_MID, lvgl_theme->spacing(4), 0);
#else
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
#endif
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8) - 160);
#else
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
#endif
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
#else
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
#endif
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    lv_obj_align(chat_message_label_, LV_ALIGN_LEFT_MID, lvgl_theme->spacing(4), 0);
#else
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
#endif

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    // face canvas for MJPEG playback (hidden until first frame arrives)
    face_canvas_ = lv_canvas_create(screen);
    lv_obj_align(face_canvas_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
    face_canvas_active_ = false;

#if CONFIG_BOARD_TYPE_XIAOLIANG_TOUCH
    CreateTouchAppLauncher(screen);
#endif
}

uint8_t* LcdDisplay::AcquireFaceDecodeBuffer(size_t min_bytes, size_t* out_size) {
    if (!setup_ui_called_ || out_size == nullptr) {
        return nullptr;
    }

    if (!Lock(30)) {
        return nullptr;
    }

    uint32_t width = face_canvas_width_ > 0 ? face_canvas_width_ : 480;
    uint32_t height = face_canvas_height_ > 0 ? face_canvas_height_ : 800;
    size_t needed_size = (size_t)width * height * 2;
    if (needed_size < min_bytes) {
        needed_size = min_bytes;
    }

    bool need_realloc = (face_bufs_[0] == nullptr || face_bufs_[1] == nullptr || face_bufs_[2] == nullptr ||
                         needed_size > (size_t)face_canvas_width_ * face_canvas_height_ * 2);
    if (need_realloc) {
        for (int i = 0; i < 3; i++) {
            if (face_bufs_[i]) {
                heap_caps_free(face_bufs_[i]);
                face_bufs_[i] = nullptr;
            }
        }
        for (int i = 0; i < 3; i++) {
            face_bufs_[i] = (uint8_t*)heap_caps_malloc(needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!face_bufs_[i]) {
                for (int j = 0; j <= i; j++) {
                    if (face_bufs_[j]) {
                        heap_caps_free(face_bufs_[j]);
                        face_bufs_[j] = nullptr;
                    }
                }
                Unlock();
                return nullptr;
            }
        }
        if (face_canvas_width_ == 0 || face_canvas_height_ == 0) {
            face_canvas_width_ = width;
            face_canvas_height_ = height;
        }
        face_display_idx_ = 0;
        face_previous_idx_ = 2;
    }

    uint32_t write_idx = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (i != face_display_idx_ && i != face_previous_idx_) {
            write_idx = i;
            break;
        }
    }

    *out_size = needed_size;
    uint8_t* buffer = face_bufs_[write_idx];
    face_pending_write_idx_ = write_idx;
    Unlock();
    return buffer;
}

void LcdDisplay::PresentFaceDecodeBuffer(uint8_t* buf, uint32_t width, uint32_t height) {
    if (!setup_ui_called_ || buf == nullptr || width == 0 || height == 0) {
        return;
    }

    if (!Lock(30)) {
        return;
    }

    uint32_t write_idx = face_pending_write_idx_;
    if (buf != face_bufs_[write_idx]) {
        for (uint32_t i = 0; i < 3; i++) {
            if (face_bufs_[i] == buf) {
                write_idx = i;
                break;
            }
        }
    }

    if (!face_canvas_active_ && face_canvas_ != nullptr) {
        face_canvas_active_ = true;
        if (emoji_box_)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_label_) lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        if (emoji_image_) lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
        if (top_bar_)    lv_obj_move_foreground(top_bar_);
        if (status_bar_) lv_obj_move_foreground(status_bar_);
        if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
        BringTouchAppLauncherToFront();
    }

    face_canvas_width_ = width;
    face_canvas_height_ = height;

    if (face_canvas_ && face_bufs_[write_idx]) {
        lv_canvas_set_buffer(face_canvas_, face_bufs_[write_idx], width, height, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(face_canvas_);
        face_previous_idx_ = face_display_idx_;
        face_display_idx_ = write_idx;
    }

    Unlock();
}

void LcdDisplay::SetFaceImage(uint8_t *rgb565, uint32_t width, uint32_t height) {
    if (!setup_ui_called_ || rgb565 == nullptr || width == 0 || height == 0) {
        return;
    }

    size_t out_size = 0;
    uint8_t* target = AcquireFaceDecodeBuffer(width * height * 2, &out_size);
    if (target == nullptr) {
        return;
    }
    memcpy(target, rgb565, width * height * 2);
    PresentFaceDecodeBuffer(target, width, height);
}

bool LcdDisplay::ShowStaticIdleFace() {
    bool player_stopped = (mjpeg_player_port_stop_wait(2000) == ESP_OK);

    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (preview_image_) {
        esp_timer_stop(preview_timer_);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
    }

    if (face_canvas_ && face_canvas_active_) {
        lv_obj_remove_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(face_canvas_);
    } else {
        if (face_canvas_) {
            lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
            face_canvas_active_ = false;
            lv_obj_invalidate(face_canvas_);
        }
        if (emoji_box_) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_box_);
        }

        auto emoji_collection = lvgl_theme->emoji_collection();
        auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage("neutral") : nullptr;
        if (emoji_image_ && image != nullptr) {
            lv_image_set_src(emoji_image_, image->image_dsc());
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_image_);
            if (emoji_label_) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (emoji_label_) {
            lv_obj_set_style_text_font(emoji_label_, lvgl_theme->text_font()->font(), 0);
            lv_label_set_text(emoji_label_, "IDLE");
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(emoji_label_);
            if (emoji_image_) {
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    if (container_) {
        lv_obj_invalidate(container_);
    }
    if (emoji_box_) {
        lv_obj_invalidate(emoji_box_);
    }
    lv_obj_invalidate(lv_screen_active());
    if (top_bar_)    lv_obj_move_foreground(top_bar_);
    if (status_bar_) lv_obj_move_foreground(status_bar_);
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
    BringTouchAppLauncherToFront();
    return player_stopped;
}

void LcdDisplay::PlayGifFromFile(const char* filepath) {
    ESP_LOGI(TAG, "PlayGifFromFile: %s", filepath);
    // Not implemented: LvglGif only supports lv_img_dsc_t (embedded assets).
    // MJPEG playback via SetFaceImage is the preferred path for SD card animations.
    ESP_LOGW(TAG, "PlayGifFromFile not supported (use MJPEG player instead)");
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() was called but label not created)", role, content);
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (app_menu_button_ != nullptr) {
        BringTouchAppLauncherToFront();
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }
    if (emotion == nullptr) {
        emotion = "neutral";
    }
    const bool is_loading = strstr(emotion, "loading") != nullptr;
    if (is_loading) {
        DisplayLockGuard lock(this);
        app_launcher_ready_ = false;
        BringTouchAppLauncherToFront();
    }

#if !DISABLE_MJPEG_EMOTIONS
    // MJPEG animations use the current pet style directory, e.g.
    // /sdcard/style/jinglingshu/happy.mjpeg. File names stay stable across skins.
    char mjpeg_path[64];
    if (ResolveMjpegEmotionPath(emotion, mjpeg_path, sizeof(mjpeg_path))) {
        {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            if (emoji_box_) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_label_) {
                lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_image_) {
                lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            }
            if (face_canvas_) {
                lv_obj_remove_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
                if (top_bar_)    lv_obj_move_foreground(top_bar_);
                if (status_bar_) lv_obj_move_foreground(status_bar_);
                if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
                BringTouchAppLauncherToFront();
            }
            face_canvas_active_ = true;
        }

        ESP_LOGI(TAG, "SetEmotion -> MJPEG: %s", mjpeg_path);
        const esp_err_t play_result = mjpeg_player_port_play_file(mjpeg_path);
        if (!is_loading && play_result == ESP_OK) {
            DisplayLockGuard lock(this);
            app_launcher_ready_ = true;
            BringTouchAppLauncherToFront();
        }
        return;
    }
#endif

    mjpeg_player_port_stop();

    const char* static_emotion = emotion;
    if (strstr(emotion, ".mjpeg") != nullptr) {
        if (strstr(emotion, "talk") != nullptr) {
            static_emotion = "happy";
        } else if (strstr(emotion, "listen") != nullptr) {
            static_emotion = "thinking";
        } else if (strstr(emotion, "loading") != nullptr) {
            static_emotion = "neutral";
        } else if (strstr(emotion, "sad") != nullptr) {
            static_emotion = "sad";
        } else if (strstr(emotion, "loving") != nullptr) {
            static_emotion = "loving";
        } else if (strstr(emotion, "confident") != nullptr) {
            static_emotion = "happy";
        } else {
            static_emotion = "neutral";
        }
    }

    // Stop any running GIF animation
    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        // Hide image before destroying GIF controller to prevent LVGL from
        // accessing freed image data during rendering between lock scopes
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    {
        DisplayLockGuard lock(this);
        if (face_canvas_) {
            lv_obj_add_flag(face_canvas_, LV_OBJ_FLAG_HIDDEN);
            face_canvas_active_ = false;
        }
        if (emoji_box_) {
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(static_emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(static_emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            app_launcher_ready_ = !is_loading;
            BringTouchAppLauncherToFront();
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", static_emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    app_launcher_ready_ = !is_loading;
    BringTouchAppLauncherToFront();

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(static_emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        if (health_icon_label_) lv_obj_set_style_text_font(health_icon_label_, large_icon_font, 0);
        if (health_label_) lv_obj_set_style_text_font(health_label_, text_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        if (health_icon_label_) lv_obj_set_style_text_font(health_icon_label_, icon_font, 0);
        if (health_label_) lv_obj_set_style_text_font(health_label_, text_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    if (health_icon_label_) lv_obj_set_style_text_color(health_icon_label_, lvgl_theme->text_color(), 0);
    if (health_label_) lv_obj_set_style_text_color(health_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    // Update bottom bar background color with 50% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
