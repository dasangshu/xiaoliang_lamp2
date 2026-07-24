#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* app_menu_button_ = nullptr;
    lv_obj_t* app_grid_layer_ = nullptr;
    lv_obj_t* app_detail_layer_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;

    // Face canvas buffers for MJPEG playback. Triple buffering avoids rewriting
    // a buffer while the LCD driver may still be flushing the previous frame.
    lv_obj_t* face_canvas_ = nullptr;
    uint8_t* face_bufs_[3] = {nullptr, nullptr, nullptr};
    volatile uint32_t face_display_idx_ = 0;
    volatile uint32_t face_previous_idx_ = 2;
    uint32_t face_canvas_width_ = 0;
    uint32_t face_canvas_height_ = 0;
    uint32_t face_pending_write_idx_ = 0;
    bool face_canvas_active_ = false;

    void InitializeLcdThemes();
    void CreateTouchAppLauncher(lv_obj_t* screen);
    void BringTouchAppLauncherToFront();
    void ShowAppGrid();
    void HideAppGrid();
    void ShowAppDetail(const char* title, const char* subtitle, const char* const* actions, size_t action_count);
    void ShowTaskScheduler();
    void ShowPetGarden();
    void ShowFeatureDashboard(const char* title, const char* subtitle, uint32_t hero_color,
                              const char* hero_title, const char* hero_body,
                              const char* const* metrics, size_t metric_count,
                              const char* const* actions, const char* const* descriptions,
                              const char* const* icons, size_t action_count,
                              const char* resource_note);
    void ShowEyeIsland();
    void ShowMusicBox();
    void ShowAiSpeaking();
    void ShowHealthHub();
    void ShowExpressionAlbum();
    void ShowDeviceSettings();
    void ShowMoreApps();
    void ShowActionDetail(const char* action_name);
    static void OnAppLauncherClicked(lv_event_t* event);
    static void OnAppGridCloseClicked(lv_event_t* event);
    static void OnAppDetailBackClicked(lv_event_t* event);
    static void OnAppModuleClicked(lv_event_t* event);
    static void OnAppActionClicked(lv_event_t* event);
    static void OnTaskQuickTimerClicked(lv_event_t* event);
    static void OnPetSkinClicked(lv_event_t* event);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    virtual void SetFaceImage(uint8_t* rgb565, uint32_t width, uint32_t height) override;
    uint8_t* AcquireFaceDecodeBuffer(size_t min_bytes, size_t* out_size);
    void PresentFaceDecodeBuffer(uint8_t* buf, uint32_t width, uint32_t height);
    bool ShowStaticIdleFace();
    void PlayGifFromFile(const char* filepath);
    void OpenAppGrid();

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
