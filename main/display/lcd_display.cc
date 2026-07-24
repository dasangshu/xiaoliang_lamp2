#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <src/misc/cache/lv_cache.h>

#include "board.h"
#include "application.h"
#include "mjpeg_player/mjpeg_player_port.h"

#define TAG "LcdDisplay"
#define DISABLE_MJPEG_EMOTIONS 0

namespace {
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

int CountMusicTracksFromManifest() {
    std::string json;
    if (!ReadTextFile("/sdcard/yinyuehe/manifest.json", json)) {
        return -1;
    }
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) {
        return -2;
    }
    cJSON* tracks = cJSON_GetObjectItem(root, "tracks");
    if (tracks == nullptr) {
        tracks = cJSON_GetObjectItem(root, "items");
    }
    const int count = cJSON_IsArray(tracks) ? cJSON_GetArraySize(tracks) : 0;
    cJSON_Delete(root);
    return count;
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

struct XiaoliangAppModule {
    enum class Kind {
        kTask,
        kPet,
        kEyeIsland,
        kMusic,
        kAiSpeaking,
        kHealth,
        kAlbum,
        kDevice,
        kMore,
    };

    Kind kind;
    const char* icon;
    const char* title;
    const char* subtitle;
    const char* const* actions;
    size_t action_count;
};

struct QuickTimerPreset {
    int minutes;
    const char* title;
    const char* content;
};

struct PetSkinPreset {
    const char* style_id;
    const char* title;
    const char* subtitle;
    const char* directory;
};

const char* const kTaskActions[] = {
    "今日任务", "添加定时", "提醒记录",
};

const char* const kEyeIslandActions[] = {
    "护眼科普", "预约检查", "护眼报告",
};

const char* const kPetActions[] = {
    "宠物中心", "喂养", "换肤",
};

const char* const kMusicActions[] = {
    "专注场景", "睡前放松", "白噪音",
};

const char* const kAiSpeakActions[] = {
    "日常对话", "英语练习", "情景列表",
};

const char* const kHealthActions[] = {
    "坐姿提醒", "饮水提醒", "休息打卡",
};

const char* const kAlbumActions[] = {
    "表情管理", "照片预览", "动画素材",
};

const char* const kDeviceActions[] = {
    "亮度", "音量", "网络",
};

const char* const kMoreActions[] = {
    "成长档案", "系统信息", "模块扩展",
};

const XiaoliangAppModule kXiaoliangModules[] = {
    {XiaoliangAppModule::Kind::kTask, FONT_AWESOME_CLOCK, "任务", "定时任务和提醒", kTaskActions, sizeof(kTaskActions) / sizeof(kTaskActions[0])},
    {XiaoliangAppModule::Kind::kEyeIsland, FONT_AWESOME_GLASSES, "护眼岛", "科普 预约 报告", kEyeIslandActions, sizeof(kEyeIslandActions) / sizeof(kEyeIslandActions[0])},
    {XiaoliangAppModule::Kind::kPet, FONT_AWESOME_GAMEPAD, "萌宠乐园", "宠物 喂养 换肤", kPetActions, sizeof(kPetActions) / sizeof(kPetActions[0])},
    {XiaoliangAppModule::Kind::kMusic, FONT_AWESOME_MUSIC, "音乐盒", "23种场景音乐", kMusicActions, sizeof(kMusicActions) / sizeof(kMusicActions[0])},
    {XiaoliangAppModule::Kind::kAiSpeaking, FONT_AWESOME_COMMENT, "AI听说", "场景化对话", kAiSpeakActions, sizeof(kAiSpeakActions) / sizeof(kAiSpeakActions[0])},
    {XiaoliangAppModule::Kind::kHealth, FONT_AWESOME_HEART, "健康提醒", "护眼与习惯打卡", kHealthActions, sizeof(kHealthActions) / sizeof(kHealthActions[0])},
    {XiaoliangAppModule::Kind::kAlbum, FONT_AWESOME_IMAGE, "表情相册", "照片和动画素材", kAlbumActions, sizeof(kAlbumActions) / sizeof(kAlbumActions[0])},
    {XiaoliangAppModule::Kind::kDevice, FONT_AWESOME_GEAR, "设备设置", "亮度 音量 网络", kDeviceActions, sizeof(kDeviceActions) / sizeof(kDeviceActions[0])},
    {XiaoliangAppModule::Kind::kMore, FONT_AWESOME_STAR, "更多", "后续模块入口", kMoreActions, sizeof(kMoreActions) / sizeof(kMoreActions[0])},
};

const QuickTimerPreset kQuickTimerPresets[] = {
    {15, "15分钟", "护眼休息"},
    {30, "30分钟", "喝水活动"},
    {45, "45分钟", "学习检查"},
    {60, "60分钟", "远眺放松"},
};

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

const char* const kHealthMetrics[] = {"健康分 91", "饮水 3次", "休息 2次"};
const char* const kHealthDescriptions[] = {
    "聚合坐姿、护眼、饮水和休息提醒。",
    "快捷创建 30/60/90 分钟饮水提醒。",
    "展示最近 7 天健康趋势，真实数据缺失时用演示数据。",
};
const char* const kHealthIcons[] = {FONT_AWESOME_HEART, FONT_AWESOME_BELL, FONT_AWESOME_CIRCLE_CHECK};

const char* const kAlbumMetrics[] = {"状态 4个", "情绪 18个", "皮肤 1套"};
const char* const kAlbumDescriptions[] = {
    "展示 idle、listen、talk、loading 等系统动画。",
    "按开心、思考、困惑、睡眠等情绪分类。",
    "点击预览当前皮肤目录下的 MJPEG 文件。",
};
const char* const kAlbumIcons[] = {FONT_AWESOME_IMAGE, FONT_AWESOME_GAMEPAD, FONT_AWESOME_PLAY};

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
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
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
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
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

void LcdDisplay::OnAppLauncherClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    if (display != nullptr) {
        display->OpenAppGrid();
    }
}

void LcdDisplay::OpenAppGrid() {
    ShowAppGrid();
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
        } else if (module->kind == XiaoliangAppModule::Kind::kAlbum) {
            display->ShowExpressionAlbum();
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
        display->ShowActionDetail(action_name);
    }
}

void LcdDisplay::ShowActionDetail(const char* action_name) {
    const char* title = action_name != nullptr ? action_name : "功能详情";
    const char* body = "该功能已接入触摸页面，真实服务未就绪时展示演示数据，后续可直接替换数据来源。";
    const char* row1 = "状态：可点击、可返回、可扩展";
    const char* row2 = "数据：真实数据优先，缺失时使用假数据";
    const char* row3 = "交互：点击后有明确反馈";

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
    } else if (strcmp(title, "表情管理") == 0 || strcmp(title, "照片预览") == 0 || strcmp(title, "动画素材") == 0) {
        body = "表情相册只预览当前皮肤目录下的统一 MJPEG 文件。";
        row1 = "目录：/sdcard/style/jinglingshu";
        row2 = "状态：idle / listen / talk / loading";
        row3 = "情绪：happy / sad / thinking / sleepy";
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
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF6F8F7), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 50, 44);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xE4ECE8), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x173B35), 0);
    lv_obj_center(back_label);

    lv_obj_t* title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, LV_HOR_RES - 96);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(title_label, text_font, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x15231F), 0);
    lv_obj_set_style_margin_left(title_label, 12, 0);

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - 24, 150);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x1D6B5F), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 16, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* hero_text = lv_label_create(hero);
    lv_label_set_text(hero_text, body);
    lv_obj_set_width(hero_text, LV_HOR_RES - 56);
    lv_label_set_long_mode(hero_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_text, lv_color_white(), 0);
    lv_obj_center(hero_text);

    const char* rows[] = {row1, row2, row3};
    for (const char* row_text : rows) {
        lv_obj_t* row = lv_obj_create(app_detail_layer_);
        lv_obj_set_size(row, LV_HOR_RES - 24, 74);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t* row_label = lv_label_create(row);
        lv_label_set_text(row_label, row_text);
        lv_obj_set_width(row_label, LV_HOR_RES - 56);
        lv_label_set_long_mode(row_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(row_label, lv_color_hex(0x15231F), 0);
        lv_obj_center(row_label);
    }

    lv_obj_t* cta = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(cta, LV_HOR_RES - 24, 52);
    lv_obj_set_style_radius(cta, 8, 0);
    lv_obj_set_style_bg_color(cta, lv_color_hex(0xE5F1ED), 0);
    lv_obj_set_style_border_width(cta, 0, 0);
    lv_obj_add_flag(cta, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cta, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* cta_label = lv_label_create(cta);
    lv_label_set_text(cta_label, "返回应用中心");
    lv_obj_set_style_text_color(cta_label, lv_color_hex(0x1D6B5F), 0);
    lv_obj_center(cta_label);

    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::OnTaskQuickTimerClicked(lv_event_t* event) {
    auto* display = static_cast<LcdDisplay*>(lv_event_get_user_data(event));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* preset = target != nullptr ? static_cast<const QuickTimerPreset*>(lv_obj_get_user_data(target)) : nullptr;
    if (display == nullptr || preset == nullptr) {
        return;
    }

    ReminderService::ReminderTask task;
    const int64_t remind_at = time(nullptr) + preset->minutes * 60;
    if (Application::GetInstance().AddReminderAt(remind_at, preset->content, task)) {
        char message[96];
        snprintf(message, sizeof(message), "已创建：%s后提醒%s", preset->title, preset->content);
        display->ShowNotification(message, 2200);
        display->ShowTaskScheduler();
    } else {
        display->ShowNotification("定时任务创建失败", 2200);
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
    display->SetEmotion("idle.mjpeg");
    display->ShowPetGarden();
}

void LcdDisplay::CreateTouchAppLauncher(lv_obj_t* screen) {
    if (screen == nullptr || app_menu_button_ != nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    app_menu_button_ = lv_obj_create(screen);
    lv_obj_set_size(app_menu_button_, 128, 50);
    lv_obj_set_style_radius(app_menu_button_, 8, 0);
    lv_obj_set_style_bg_color(app_menu_button_, lv_color_hex(0x1D6B5F), 0);
    lv_obj_set_style_bg_opa(app_menu_button_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_menu_button_, 0, 0);
    lv_obj_set_style_pad_all(app_menu_button_, 0, 0);
    lv_obj_set_style_shadow_width(app_menu_button_, 10, 0);
    lv_obj_set_style_shadow_opa(app_menu_button_, LV_OPA_30, 0);
    lv_obj_set_scrollbar_mode(app_menu_button_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(app_menu_button_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(app_menu_button_, OnAppLauncherClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* row = lv_obj_create(app_menu_button_);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, OnAppLauncherClicked, LV_EVENT_CLICKED, this);
    lv_obj_center(row);

    lv_obj_t* icon = lv_label_create(row);
    lv_label_set_text(icon, FONT_AWESOME_HOUSE);
    lv_obj_set_style_text_font(icon, icon_font, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);

    lv_obj_t* text = lv_label_create(row);
    lv_label_set_text(text, "功能");
    lv_obj_set_style_text_font(text, text_font, 0);
    lv_obj_set_style_text_color(text, lv_color_white(), 0);
    lv_obj_set_style_margin_left(text, 6, 0);

    BringTouchAppLauncherToFront();
}

void LcdDisplay::BringTouchAppLauncherToFront() {
    if (app_menu_button_ == nullptr) {
        return;
    }

    lv_obj_align(app_menu_button_, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_move_foreground(app_menu_button_);
}

void LcdDisplay::ShowAppGrid() {
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
        lv_obj_del(app_grid_layer_);
        app_grid_layer_ = nullptr;
    }

    app_grid_layer_ = lv_obj_create(screen);
    lv_obj_set_size(app_grid_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(app_grid_layer_, 0, 0);
    lv_obj_set_style_bg_color(app_grid_layer_, lv_color_hex(0xF7FAF8), 0);
    lv_obj_set_style_bg_opa(app_grid_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_grid_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_grid_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_grid_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_grid_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 52);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* title = lv_label_create(header);
    lv_label_set_text(title, "小亮应用中心");
    lv_obj_set_style_text_font(title, text_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x173B35), 0);

    lv_obj_t* close = lv_obj_create(header);
    lv_obj_set_size(close, 48, 42);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0xE4ECE8), 0);
    lv_obj_set_style_border_width(close, 0, 0);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close, OnAppGridCloseClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* close_label = lv_label_create(close);
    lv_label_set_text(close_label, FONT_AWESOME_XMARK);
    lv_obj_set_style_text_font(close_label, icon_font, 0);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0x173B35), 0);
    lv_obj_center(close_label);

    lv_obj_t* grid = lv_obj_create(app_grid_layer_);
    lv_obj_set_size(grid, LV_HOR_RES - 24, LV_VER_RES - 84);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    const lv_coord_t tile_width = (LV_HOR_RES - 24 - 20) / 3;
    const lv_coord_t tile_height = 132;
    for (const auto& module : kXiaoliangModules) {
        lv_obj_t* tile = lv_obj_create(grid);
        lv_obj_set_size(tile, tile_width, tile_height);
        lv_obj_set_style_radius(tile, 8, 0);
        lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_all(tile, 8, 0);
        lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(tile, const_cast<XiaoliangAppModule*>(&module));
        lv_obj_add_event_cb(tile, OnAppModuleClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon = lv_label_create(tile);
        lv_label_set_text(icon, module.icon);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x1D6B5F), 0);

        lv_obj_t* module_title = lv_label_create(tile);
        lv_label_set_text(module_title, module.title);
        lv_obj_set_style_text_font(module_title, text_font, 0);
        lv_obj_set_style_text_color(module_title, lv_color_hex(0x15231F), 0);
        lv_obj_set_style_margin_top(module_title, 6, 0);

        lv_obj_t* subtitle = lv_label_create(tile);
        lv_label_set_text(subtitle, module.subtitle);
        lv_obj_set_width(subtitle, tile_width - 16);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0x60736B), 0);
        lv_obj_set_style_margin_top(subtitle, 3, 0);
    }

    lv_obj_move_foreground(app_grid_layer_);
}

void LcdDisplay::HideAppGrid() {
    DisplayLockGuard lock(this);
    if (app_detail_layer_ != nullptr) {
        lv_obj_del(app_detail_layer_);
        app_detail_layer_ = nullptr;
    }
    if (app_grid_layer_ != nullptr) {
        lv_obj_del(app_grid_layer_);
        app_grid_layer_ = nullptr;
    }
}

void LcdDisplay::ShowAppDetail(const char* title, const char* subtitle, const char* const* actions, size_t action_count) {
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
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF7FAF8), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 50, 44);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xE4ECE8), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x173B35), 0);
    lv_obj_center(back_label);

    lv_obj_t* title_box = lv_obj_create(header);
    lv_obj_set_size(title_box, LV_HOR_RES - 90, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_style_margin_left(title_box, 10, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title_label = lv_label_create(title_box);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, text_font, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x15231F), 0);

    lv_obj_t* subtitle_label = lv_label_create(title_box);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_width(subtitle_label, LV_HOR_RES - 90);
    lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0x60736B), 0);

    lv_obj_t* list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list, LV_HOR_RES - 24, LV_VER_RES - 90);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (size_t i = 0; i < action_count; ++i) {
        lv_obj_t* action = lv_obj_create(list);
        lv_obj_set_size(action, LV_HOR_RES - 24, 72);
        lv_obj_set_style_radius(action, 8, 0);
        lv_obj_set_style_bg_color(action, lv_color_white(), 0);
        lv_obj_set_style_border_width(action, 1, 0);
        lv_obj_set_style_border_color(action, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_left(action, 18, 0);
        lv_obj_set_style_pad_right(action, 14, 0);
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
        lv_label_set_long_mode(action_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(action_label, text_font, 0);
        lv_obj_set_style_text_color(action_label, lv_color_hex(0x15231F), 0);
        lv_obj_set_style_margin_left(action_label, 14, 0);

        lv_obj_t* arrow = lv_label_create(action);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(arrow, icon_font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x8CA099), 0);
        lv_obj_set_style_margin_left(arrow, 0, 0);
    }

    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowFeatureDashboard(const char* title, const char* subtitle, uint32_t hero_color,
                                      const char* hero_title, const char* hero_body,
                                      const char* const* metrics, size_t metric_count,
                                      const char* const* actions, const char* const* descriptions,
                                      const char* const* icons, size_t action_count,
                                      const char* resource_note) {
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
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF6F8F7), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 50, 44);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xE4ECE8), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x173B35), 0);
    lv_obj_center(back_label);

    lv_obj_t* title_box = lv_obj_create(header);
    lv_obj_set_size(title_box, LV_HOR_RES - 90, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_style_margin_left(title_box, 10, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title_label = lv_label_create(title_box);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, text_font, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x15231F), 0);

    lv_obj_t* subtitle_label = lv_label_create(title_box);
    lv_label_set_text(subtitle_label, subtitle);
    lv_obj_set_width(subtitle_label, LV_HOR_RES - 90);
    lv_label_set_long_mode(subtitle_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(subtitle_label, lv_color_hex(0x60736B), 0);

    lv_obj_t* body = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(body, LV_HOR_RES - 24, LV_VER_RES - 82);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    lv_obj_t* hero = lv_obj_create(body);
    lv_obj_set_size(hero, LV_HOR_RES - 24, 118);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(hero_color), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 14, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* hero_title_label = lv_label_create(hero);
    lv_label_set_text(hero_title_label, hero_title);
    lv_obj_set_style_text_font(hero_title_label, text_font, 0);
    lv_obj_set_style_text_color(hero_title_label, lv_color_white(), 0);

    lv_obj_t* hero_body_label = lv_label_create(hero);
    lv_label_set_text(hero_body_label, hero_body);
    lv_obj_set_width(hero_body_label, LV_HOR_RES - 56);
    lv_label_set_long_mode(hero_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hero_body_label, lv_color_hex(0xEEF7F4), 0);
    lv_obj_set_style_margin_top(hero_body_label, 8, 0);

    lv_obj_t* metric_row = lv_obj_create(body);
    lv_obj_set_size(metric_row, LV_HOR_RES - 24, 50);
    lv_obj_set_style_bg_opa(metric_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metric_row, 0, 0);
    lv_obj_set_style_pad_all(metric_row, 0, 0);
    lv_obj_set_style_pad_column(metric_row, 8, 0);
    lv_obj_set_flex_flow(metric_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metric_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const lv_coord_t metric_width = (LV_HOR_RES - 24 - 16) / 3;
    for (size_t i = 0; i < metric_count && i < 3; ++i) {
        lv_obj_t* metric = lv_obj_create(metric_row);
        lv_obj_set_size(metric, metric_width, 44);
        lv_obj_set_style_radius(metric, 8, 0);
        lv_obj_set_style_bg_color(metric, lv_color_white(), 0);
        lv_obj_set_style_border_width(metric, 1, 0);
        lv_obj_set_style_border_color(metric, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_all(metric, 0, 0);
        lv_obj_t* label = lv_label_create(metric);
        lv_label_set_text(label, metrics[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(0x1D6B5F), 0);
        lv_obj_center(label);
    }

    lv_obj_t* action_title = lv_label_create(body);
    lv_label_set_text(action_title, "功能");
    lv_obj_set_style_text_font(action_title, text_font, 0);
    lv_obj_set_style_text_color(action_title, lv_color_hex(0x15231F), 0);

    for (size_t i = 0; i < action_count; ++i) {
        lv_obj_t* card = lv_obj_create(body);
        lv_obj_set_size(card, LV_HOR_RES - 24, 78);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_left(card, 12, 0);
        lv_obj_set_style_pad_right(card, 12, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, const_cast<char*>(actions[i]));
        lv_obj_add_event_cb(card, OnAppActionClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 48, 48);
        lv_obj_set_style_radius(icon_box, 8, 0);
        lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xE5F1ED), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, icons != nullptr ? icons[i] : FONT_AWESOME_CIRCLE_CHECK);
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(0x1D6B5F), 0);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - 132, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* action_label = lv_label_create(text_box);
        lv_label_set_text(action_label, actions[i]);
        lv_obj_set_style_text_font(action_label, text_font, 0);
        lv_obj_set_style_text_color(action_label, lv_color_hex(0x15231F), 0);

        lv_obj_t* desc_label = lv_label_create(text_box);
        lv_label_set_text(desc_label, descriptions != nullptr ? descriptions[i] : "点击进入功能详情");
        lv_obj_set_width(desc_label, LV_HOR_RES - 140);
        lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(0x60736B), 0);
        lv_obj_set_style_margin_top(desc_label, 3, 0);

        lv_obj_t* arrow = lv_label_create(card);
        lv_label_set_text(arrow, FONT_AWESOME_ANGLE_RIGHT);
        lv_obj_set_style_text_font(arrow, icon_font, 0);
        lv_obj_set_style_text_color(arrow, lv_color_hex(0x8CA099), 0);
    }

    lv_obj_t* note = lv_obj_create(body);
    lv_obj_set_size(note, LV_HOR_RES - 24, 86);
    lv_obj_set_style_radius(note, 8, 0);
    lv_obj_set_style_bg_color(note, lv_color_hex(0xEEF4F1), 0);
    lv_obj_set_style_border_width(note, 0, 0);
    lv_obj_set_style_pad_all(note, 12, 0);
    lv_obj_set_scrollbar_mode(note, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* note_label = lv_label_create(note);
    lv_label_set_text(note_label, resource_note);
    lv_obj_set_width(note_label, LV_HOR_RES - 54);
    lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(note_label, lv_color_hex(0x36564C), 0);
    lv_obj_center(note_label);

    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowEyeIsland() {
    cJSON* status = Application::GetInstance().GetEyeCareStatusJson();
    char hero_body[180];
    if (status != nullptr) {
        cJSON* score = cJSON_GetObjectItem(status, "health_score");
        snprintf(hero_body, sizeof(hero_body), "今日护眼报告已接入服务。当前分数 %d，继续保持远眺、坐姿和学习节奏。", cJSON_IsNumber(score) ? score->valueint : 86);
        cJSON_Delete(status);
    } else {
        snprintf(hero_body, sizeof(hero_body), "今日暂无完整数据，先展示演示报告：护眼分 86，远眺 4 次，坐姿提醒 2 次。");
    }
    ShowFeatureDashboard("护眼岛", "护眼科普、预约、报告", 0x1D6B5F,
                         "今日护眼计划", hero_body,
                         kEyeMetrics, sizeof(kEyeMetrics) / sizeof(kEyeMetrics[0]),
                         kEyeIslandActions, kEyeDescriptions, kEyeIcons,
                         sizeof(kEyeIslandActions) / sizeof(kEyeIslandActions[0]),
                         "数据来源：EyeCareService；缺失时使用演示数据。预约能力预留给后续接口接入。");
}

void LcdDisplay::ShowMusicBox() {
    int track_count = CountMusicTracksFromManifest();
    char hero_body[180];
    char track_metric[32];
    if (track_count > 0) {
        snprintf(hero_body, sizeof(hero_body), "已读取 /sdcard/yinyuehe/manifest.json，当前可展示 %d 首场景音乐。", track_count);
        snprintf(track_metric, sizeof(track_metric), "曲目 %d首", track_count);
    } else if (track_count == -1) {
        snprintf(hero_body, sizeof(hero_body), "未找到 /sdcard/yinyuehe/manifest.json，请确认 yinyuehe 已放到 SD 卡根目录。");
        snprintf(track_metric, sizeof(track_metric), "清单 缺失");
    } else if (track_count == -2) {
        snprintf(hero_body, sizeof(hero_body), "音乐清单 JSON 解析失败，请检查 manifest.json 格式。");
        snprintf(track_metric, sizeof(track_metric), "清单 异常");
    } else {
        snprintf(hero_body, sizeof(hero_body), "音乐清单存在但没有曲目，先展示推荐、睡眠和自然场景入口。");
        snprintf(track_metric, sizeof(track_metric), "曲目 0首");
    }
    const char* metrics[] = {track_metric, kMusicMetrics[1], kMusicMetrics[2]};
    ShowFeatureDashboard("音乐盒", "SD 卡场景音乐", 0x335C81,
                         "场景音乐", hero_body,
                         metrics, sizeof(metrics) / sizeof(metrics[0]),
                         kMusicActions, kMusicDescriptions, kMusicIcons,
                         sizeof(kMusicActions) / sizeof(kMusicActions[0]),
                         "播放路径统一为 /sdcard/yinyuehe/audio/<storage_name>。播放控制页已预留 TTS 冲突处理入口。");
}

void LcdDisplay::ShowAiSpeaking() {
    ShowFeatureDashboard("AI听说", "场景化对话训练", 0x6A4C93,
                         "今天练一个场景", "选择场景后进入听说训练，设备会切到聆听流程；无网络时展示离线提示。",
                         kAiMetrics, sizeof(kAiMetrics) / sizeof(kAiMetrics[0]),
                         kAiSpeakActions, kAiDescriptions, kAiIcons,
                         sizeof(kAiSpeakActions) / sizeof(kAiSpeakActions[0]),
                         "场景 prompt 已按独立数据结构规划，后续可由后台下发或按模块跳转。");
}

void LcdDisplay::ShowHealthHub() {
    ShowFeatureDashboard("健康提醒", "坐姿、饮水、休息、远眺", 0x8B5E34,
                         "今日健康节奏", "聚合护眼和坐姿状态，饮水与休息提醒复用定时任务能力。",
                         kHealthMetrics, sizeof(kHealthMetrics) / sizeof(kHealthMetrics[0]),
                         kHealthActions, kHealthDescriptions, kHealthIcons,
                         sizeof(kHealthActions) / sizeof(kHealthActions[0]),
                         "坐姿模型或摄像头不可用时显示异常状态，不影响饮水和休息提醒。");
}

void LcdDisplay::ShowExpressionAlbum() {
    char hero_body[180];
    auto style = CurrentPetStyleName();
    snprintf(hero_body, sizeof(hero_body), "当前皮肤：%s。表情预览只从 /sdcard/style/%s/<filename>.mjpeg 读取。", style.c_str(), style.c_str());
    ShowFeatureDashboard("表情相册", "宠物动画和素材预览", 0x4F6F52,
                         "当前皮肤表情", hero_body,
                         kAlbumMetrics, sizeof(kAlbumMetrics) / sizeof(kAlbumMetrics[0]),
                         kAlbumActions, kAlbumDescriptions, kAlbumIcons,
                         sizeof(kAlbumActions) / sizeof(kAlbumActions[0]),
                         "用于验证 idle、listen、talk、happy、sad 等统一文件名。旧 /sdcard/<文件名>.mjpeg 不再访问。");
}

void LcdDisplay::ShowDeviceSettings() {
    ShowFeatureDashboard("设备设置", "屏幕、音量、网络、资源诊断", 0x2F4858,
                         "设备控制中心", "提供亮度、音量、网络和 SD 卡资源诊断入口，适合触摸屏现场调试和产测。",
                         kDeviceMetrics, sizeof(kDeviceMetrics) / sizeof(kDeviceMetrics[0]),
                         kDeviceActions, kDeviceDescriptions, kDeviceIcons,
                         sizeof(kDeviceActions) / sizeof(kDeviceActions[0]),
                         "资源诊断会检查 /sdcard/yinyuehe、/sdcard/style/jinglingshu 和必要 MJPEG 文件。");
}

void LcdDisplay::ShowMoreApps() {
    ShowFeatureDashboard("更多", "后续模块入口", 0x5F5B6B,
                         "扩展中心", "成长档案、家长控制、固件升级等模块先以可点击入口展示，后续按注册表扩展。",
                         kMoreMetrics, sizeof(kMoreMetrics) / sizeof(kMoreMetrics[0]),
                         kMoreActions, kMoreDescriptions, kMoreIcons,
                         sizeof(kMoreActions) / sizeof(kMoreActions[0]),
                         "未完成模块显示开发中状态，但入口、返回和反馈保持一致。");
}

void LcdDisplay::ShowTaskScheduler() {
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
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF4F7F5), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 54);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 50, 44);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xE2EBE6), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x183B34), 0);
    lv_obj_center(back_label);

    lv_obj_t* title_box = lv_obj_create(header);
    lv_obj_set_size(title_box, LV_HOR_RES - 90, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_style_margin_left(title_box, 10, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(title_box);
    lv_label_set_text(title, "定时任务");
    lv_obj_set_style_text_font(title, text_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x15231F), 0);

    lv_obj_t* subtitle = lv_label_create(title_box);
    lv_label_set_text(subtitle, "提醒、习惯、学习节奏");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x60736B), 0);

    cJSON* reminders = Application::GetInstance().GetRemindersJson();
    cJSON* tasks = reminders != nullptr ? cJSON_GetObjectItem(reminders, "tasks") : nullptr;
    const time_t now = time(nullptr);
    int pending_count = 0;
    int fired_count = 0;
    int64_t next_time = 0;
    const char* next_content = nullptr;

    if (cJSON_IsArray(tasks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tasks) {
            auto remind_at = cJSON_GetObjectItem(item, "remind_at");
            auto content = cJSON_GetObjectItem(item, "content");
            auto fired = cJSON_GetObjectItem(item, "fired");
            const bool is_fired = cJSON_IsBool(fired) && fired->valueint == 1;
            if (is_fired) {
                fired_count++;
                continue;
            }
            pending_count++;
            if (cJSON_IsNumber(remind_at) && cJSON_IsString(content)) {
                int64_t t = (int64_t)remind_at->valuedouble;
                if (t > now && (next_time == 0 || t < next_time)) {
                    next_time = t;
                    next_content = content->valuestring;
                }
            }
        }
    }

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - 24, 116);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x1D6B5F), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 14, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* hero_title = lv_label_create(hero);
    lv_label_set_text(hero_title, "下一次提醒");
    lv_obj_set_style_text_color(hero_title, lv_color_hex(0xBDE7DA), 0);

    char next_line[96];
    if (next_time > 0) {
        struct tm tm_time = {};
        time_t next_time_value = (time_t)next_time;
        localtime_r(&next_time_value, &tm_time);
        char time_text[24];
        strftime(time_text, sizeof(time_text), "%H:%M", &tm_time);
        snprintf(next_line, sizeof(next_line), "%s  %s", time_text, next_content != nullptr ? next_content : "提醒");
    } else {
        snprintf(next_line, sizeof(next_line), "暂无待执行任务");
    }
    lv_obj_t* next_label = lv_label_create(hero);
    lv_label_set_text(next_label, next_line);
    lv_obj_set_width(next_label, LV_HOR_RES - 62);
    lv_label_set_long_mode(next_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(next_label, text_font, 0);
    lv_obj_set_style_text_color(next_label, lv_color_white(), 0);
    lv_obj_set_style_margin_top(next_label, 6, 0);

    lv_obj_t* metrics = lv_obj_create(hero);
    lv_obj_set_size(metrics, LV_HOR_RES - 52, 30);
    lv_obj_set_style_bg_opa(metrics, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(metrics, 0, 0);
    lv_obj_set_style_pad_all(metrics, 0, 0);
    lv_obj_set_style_margin_top(metrics, 8, 0);
    lv_obj_set_flex_flow(metrics, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(metrics, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char pending_text[24];
    char fired_text[24];
    snprintf(pending_text, sizeof(pending_text), "待执行 %d", pending_count);
    snprintf(fired_text, sizeof(fired_text), "已完成 %d", fired_count);
    const char* metric_labels[] = {pending_text, fired_text, "语音可创建"};
    for (auto label_text : metric_labels) {
        lv_obj_t* chip = lv_obj_create(metrics);
        lv_obj_set_size(chip, 120, 28);
        lv_obj_set_style_radius(chip, 8, 0);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x2B8072), 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_set_style_pad_all(chip, 0, 0);
        lv_obj_t* chip_label = lv_label_create(chip);
        lv_label_set_text(chip_label, label_text);
        lv_obj_set_style_text_color(chip_label, lv_color_white(), 0);
        lv_obj_center(chip_label);
    }

    lv_obj_t* quick_title = lv_label_create(app_detail_layer_);
    lv_label_set_text(quick_title, "快捷定时");
    lv_obj_set_style_text_font(quick_title, text_font, 0);
    lv_obj_set_style_text_color(quick_title, lv_color_hex(0x15231F), 0);
    lv_obj_set_style_margin_top(quick_title, 10, 0);

    lv_obj_t* quick_row = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(quick_row, LV_HOR_RES - 24, 66);
    lv_obj_set_style_bg_opa(quick_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(quick_row, 0, 0);
    lv_obj_set_style_pad_all(quick_row, 0, 0);
    lv_obj_set_style_pad_column(quick_row, 8, 0);
    lv_obj_set_flex_flow(quick_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(quick_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (const auto& preset : kQuickTimerPresets) {
        lv_obj_t* quick = lv_obj_create(quick_row);
        lv_obj_set_size(quick, 105, 58);
        lv_obj_set_style_radius(quick, 8, 0);
        lv_obj_set_style_bg_color(quick, lv_color_white(), 0);
        lv_obj_set_style_border_width(quick, 1, 0);
        lv_obj_set_style_border_color(quick, lv_color_hex(0xCFE0D8), 0);
        lv_obj_set_style_pad_all(quick, 4, 0);
        lv_obj_set_scrollbar_mode(quick, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(quick, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(quick, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(quick, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(quick, const_cast<QuickTimerPreset*>(&preset));
        lv_obj_add_event_cb(quick, OnTaskQuickTimerClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* time_label = lv_label_create(quick);
        lv_label_set_text(time_label, preset.title);
        lv_obj_set_style_text_font(time_label, text_font, 0);
        lv_obj_set_style_text_color(time_label, lv_color_hex(0x1D6B5F), 0);

        lv_obj_t* desc_label = lv_label_create(quick);
        lv_label_set_text(desc_label, preset.content);
        lv_obj_set_style_text_color(desc_label, lv_color_hex(0x60736B), 0);
    }

    lv_obj_t* list_header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list_header, LV_HOR_RES - 24, 42);
    lv_obj_set_style_bg_opa(list_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_header, 0, 0);
    lv_obj_set_style_pad_all(list_header, 0, 0);
    lv_obj_set_flex_flow(list_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(list_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* list_title = lv_label_create(list_header);
    lv_label_set_text(list_title, "任务列表");
    lv_obj_set_style_text_font(list_title, text_font, 0);
    lv_obj_set_style_text_color(list_title, lv_color_hex(0x15231F), 0);

    lv_obj_t* add_button = lv_obj_create(list_header);
    lv_obj_set_size(add_button, 126, 38);
    lv_obj_set_style_radius(add_button, 8, 0);
    lv_obj_set_style_bg_color(add_button, lv_color_hex(0xE2EBE6), 0);
    lv_obj_set_style_border_width(add_button, 0, 0);
    lv_obj_add_flag(add_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add_button, OnAppActionClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* add_label = lv_label_create(add_button);
    lv_label_set_text(add_label, "语音添加");
    lv_obj_set_style_text_color(add_label, lv_color_hex(0x1D6B5F), 0);
    lv_obj_center(add_label);

    lv_obj_t* list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(list, LV_HOR_RES - 24, 290);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    int rendered = 0;
    if (cJSON_IsArray(tasks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, tasks) {
            if (rendered >= 5) {
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
            lv_obj_set_size(row, LV_HOR_RES - 24, 72);
            lv_obj_set_style_radius(row, 8, 0);
            lv_obj_set_style_bg_color(row, lv_color_white(), 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(0xD9E4DF), 0);
            lv_obj_set_style_pad_all(row, 10, 0);
            lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

            lv_obj_t* state = lv_obj_create(row);
            lv_obj_set_size(state, 48, 48);
            lv_obj_set_style_radius(state, 8, 0);
            lv_obj_set_style_bg_color(state, is_fired ? lv_color_hex(0xE6EAE8) : lv_color_hex(0xDFF4EC), 0);
            lv_obj_set_style_border_width(state, 0, 0);
            lv_obj_t* state_icon = lv_label_create(state);
            lv_label_set_text(state_icon, is_fired ? FONT_AWESOME_CHECK : FONT_AWESOME_CLOCK);
            lv_obj_set_style_text_font(state_icon, icon_font, 0);
            lv_obj_set_style_text_color(state_icon, is_fired ? lv_color_hex(0x7E8B86) : lv_color_hex(0x1D6B5F), 0);
            lv_obj_center(state_icon);

            lv_obj_t* text_box = lv_obj_create(row);
            lv_obj_set_size(text_box, LV_HOR_RES - 122, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(text_box, 0, 0);
            lv_obj_set_style_pad_all(text_box, 0, 0);
            lv_obj_set_style_margin_left(text_box, 10, 0);
            lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

            lv_obj_t* row_title = lv_label_create(text_box);
            lv_label_set_text(row_title, content->valuestring);
            lv_obj_set_width(row_title, LV_HOR_RES - 132);
            lv_label_set_long_mode(row_title, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_color(row_title, lv_color_hex(0x15231F), 0);

            lv_obj_t* row_time = lv_label_create(text_box);
            lv_label_set_text(row_time, time_text);
            lv_obj_set_style_text_color(row_time, is_fired ? lv_color_hex(0x8CA099) : lv_color_hex(0x1D6B5F), 0);
            rendered++;
        }
    }

    if (rendered == 0) {
        lv_obj_t* empty = lv_obj_create(list);
        lv_obj_set_size(empty, LV_HOR_RES - 24, 110);
        lv_obj_set_style_radius(empty, 8, 0);
        lv_obj_set_style_bg_color(empty, lv_color_white(), 0);
        lv_obj_set_style_border_width(empty, 1, 0);
        lv_obj_set_style_border_color(empty, lv_color_hex(0xD9E4DF), 0);
        lv_obj_set_style_pad_all(empty, 12, 0);
        lv_obj_set_scrollbar_mode(empty, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* empty_icon = lv_label_create(empty);
        lv_label_set_text(empty_icon, FONT_AWESOME_BELL);
        lv_obj_set_style_text_font(empty_icon, icon_font, 0);
        lv_obj_set_style_text_color(empty_icon, lv_color_hex(0x1D6B5F), 0);

        lv_obj_t* empty_text = lv_label_create(empty);
        lv_label_set_text(empty_text, "还没有定时任务");
        lv_obj_set_style_text_font(empty_text, text_font, 0);
        lv_obj_set_style_text_color(empty_text, lv_color_hex(0x15231F), 0);

        lv_obj_t* empty_hint = lv_label_create(empty);
        lv_label_set_text(empty_hint, "点快捷定时，或说“30分钟后提醒我喝水”");
        lv_obj_set_width(empty_hint, LV_HOR_RES - 72);
        lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty_hint, lv_color_hex(0x60736B), 0);
    }

    if (reminders != nullptr) {
        cJSON_Delete(reminders);
    }
    lv_obj_move_foreground(app_detail_layer_);
}

void LcdDisplay::ShowPetGarden() {
    DisplayLockGuard lock(this);
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
    lv_obj_set_style_bg_color(app_detail_layer_, lv_color_hex(0xF7F4EF), 0);
    lv_obj_set_style_bg_opa(app_detail_layer_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_detail_layer_, 0, 0);
    lv_obj_set_style_pad_all(app_detail_layer_, 12, 0);
    lv_obj_set_scrollbar_mode(app_detail_layer_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* header = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(header, LV_HOR_RES - 24, 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* back = lv_obj_create(header);
    lv_obj_set_size(back, 50, 44);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xEFE7DC), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, OnAppDetailBackClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* back_label = lv_label_create(back);
    lv_label_set_text(back_label, FONT_AWESOME_ARROW_LEFT);
    lv_obj_set_style_text_font(back_label, icon_font, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0x43362A), 0);
    lv_obj_center(back_label);

    lv_obj_t* title_box = lv_obj_create(header);
    lv_obj_set_size(title_box, LV_HOR_RES - 90, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(title_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_box, 0, 0);
    lv_obj_set_style_pad_all(title_box, 0, 0);
    lv_obj_set_style_margin_left(title_box, 10, 0);
    lv_obj_set_flex_flow(title_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* title = lv_label_create(title_box);
    lv_label_set_text(title, "萌宠乐园");
    lv_obj_set_style_text_font(title, text_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x2B211A), 0);

    lv_obj_t* subtitle = lv_label_create(title_box);
    lv_label_set_text(subtitle, "宠物中心、喂养与皮肤切换");
    lv_obj_set_width(subtitle, LV_HOR_RES - 90);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x7B6A5A), 0);

    lv_obj_t* hero = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hero, LV_HOR_RES - 24, 142);
    lv_obj_set_style_radius(hero, 8, 0);
    lv_obj_set_style_bg_color(hero, lv_color_hex(0x6E4F35), 0);
    lv_obj_set_style_border_width(hero, 0, 0);
    lv_obj_set_style_pad_all(hero, 14, 0);
    lv_obj_set_scrollbar_mode(hero, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* avatar = lv_obj_create(hero);
    lv_obj_set_size(avatar, 94, 94);
    lv_obj_set_style_radius(avatar, 8, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(0xF4D3A0), 0);
    lv_obj_set_style_border_width(avatar, 0, 0);
    lv_obj_t* avatar_icon = lv_label_create(avatar);
    lv_label_set_text(avatar_icon, FONT_AWESOME_GAMEPAD);
    lv_obj_set_style_text_font(avatar_icon, icon_font, 0);
    lv_obj_set_style_text_color(avatar_icon, lv_color_hex(0x6E4F35), 0);
    lv_obj_center(avatar_icon);

    lv_obj_t* hero_text = lv_obj_create(hero);
    lv_obj_set_size(hero_text, LV_HOR_RES - 150, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(hero_text, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hero_text, 0, 0);
    lv_obj_set_style_pad_all(hero_text, 0, 0);
    lv_obj_set_style_margin_left(hero_text, 14, 0);
    lv_obj_set_flex_flow(hero_text, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* pet_name = lv_label_create(hero_text);
    lv_label_set_text(pet_name, current_style == "jinglingshu" ? "精灵鼠" : current_style.c_str());
    lv_obj_set_style_text_font(pet_name, text_font, 0);
    lv_obj_set_style_text_color(pet_name, lv_color_white(), 0);

    lv_obj_t* pet_state = lv_label_create(hero_text);
    lv_label_set_text(pet_state, "当前皮肤目录");
    lv_obj_set_style_text_color(pet_state, lv_color_hex(0xF6DDB9), 0);
    lv_obj_set_style_margin_top(pet_state, 4, 0);

    char style_path[96];
    snprintf(style_path, sizeof(style_path), "/sdcard/style/%s", current_style.c_str());
    lv_obj_t* path_label = lv_label_create(hero_text);
    lv_label_set_text(path_label, style_path);
    lv_obj_set_width(path_label, LV_HOR_RES - 160);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(path_label, lv_color_hex(0xF6DDB9), 0);
    lv_obj_set_style_margin_top(path_label, 8, 0);

    lv_obj_t* section_title = lv_label_create(app_detail_layer_);
    lv_label_set_text(section_title, "皮肤");
    lv_obj_set_style_text_font(section_title, text_font, 0);
    lv_obj_set_style_text_color(section_title, lv_color_hex(0x2B211A), 0);
    lv_obj_set_style_margin_top(section_title, 12, 0);

    lv_obj_t* skin_list = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(skin_list, LV_HOR_RES - 24, 168);
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
        lv_obj_set_size(card, LV_HOR_RES - 24, 74);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_bg_color(card, active ? lv_color_hex(0xFFF2D7) : (installed ? lv_color_white() : lv_color_hex(0xF1ECE5)), 0);
        lv_obj_set_style_border_width(card, active ? 2 : 1, 0);
        lv_obj_set_style_border_color(card, active ? lv_color_hex(0xC88932) : lv_color_hex(0xE3D8CA), 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(card, const_cast<PetSkinPreset*>(&skin));
        lv_obj_add_event_cb(card, OnPetSkinClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* icon_box = lv_obj_create(card);
        lv_obj_set_size(icon_box, 48, 48);
        lv_obj_set_style_radius(icon_box, 8, 0);
        lv_obj_set_style_bg_color(icon_box, active ? lv_color_hex(0xC88932) : lv_color_hex(0xEFE7DC), 0);
        lv_obj_set_style_border_width(icon_box, 0, 0);
        lv_obj_t* icon = lv_label_create(icon_box);
        lv_label_set_text(icon, active ? FONT_AWESOME_CHECK : (installed ? FONT_AWESOME_GAMEPAD : FONT_AWESOME_CLOCK));
        lv_obj_set_style_text_font(icon, icon_font, 0);
        lv_obj_set_style_text_color(icon, active ? lv_color_white() : lv_color_hex(0x6E4F35), 0);
        lv_obj_center(icon);

        lv_obj_t* text_box = lv_obj_create(card);
        lv_obj_set_size(text_box, LV_HOR_RES - 112, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(text_box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(text_box, 0, 0);
        lv_obj_set_style_pad_all(text_box, 0, 0);
        lv_obj_set_style_margin_left(text_box, 12, 0);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);

        lv_obj_t* skin_title = lv_label_create(text_box);
        lv_label_set_text(skin_title, skin.title);
        lv_obj_set_style_text_font(skin_title, text_font, 0);
        lv_obj_set_style_text_color(skin_title, lv_color_hex(0x2B211A), 0);

        lv_obj_t* skin_subtitle = lv_label_create(text_box);
        char skin_subtitle_text[128];
        snprintf(skin_subtitle_text, sizeof(skin_subtitle_text), "%s  %s", installed ? "可用" : "待安装", skin.directory);
        lv_label_set_text(skin_subtitle, skin_subtitle_text);
        lv_obj_set_width(skin_subtitle, LV_HOR_RES - 124);
        lv_label_set_long_mode(skin_subtitle, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(skin_subtitle, lv_color_hex(0x7B6A5A), 0);
    }

    lv_obj_t* hint = lv_obj_create(app_detail_layer_);
    lv_obj_set_size(hint, LV_HOR_RES - 24, 96);
    lv_obj_set_style_radius(hint, 8, 0);
    lv_obj_set_style_bg_color(hint, lv_color_white(), 0);
    lv_obj_set_style_border_width(hint, 1, 0);
    lv_obj_set_style_border_color(hint, lv_color_hex(0xE3D8CA), 0);
    lv_obj_set_style_pad_all(hint, 12, 0);
    lv_obj_set_scrollbar_mode(hint, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* hint_text = lv_label_create(hint);
    lv_label_set_text(hint_text, "新增皮肤时保持 MJPEG 文件名一致，只新增目录，例如 /sdcard/style/xiaotu/idle.mjpeg。");
    lv_obj_set_width(hint_text, LV_HOR_RES - 54);
    lv_label_set_long_mode(hint_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(hint_text, lv_color_hex(0x57483A), 0);
    lv_obj_center(hint_text);

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
    bool player_stopped = (mjpeg_player_port_stop_wait(800) == ESP_OK);

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
        mjpeg_player_port_play_file(mjpeg_path);
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
