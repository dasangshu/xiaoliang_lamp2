#pragma once

#include <cstdint>
#include <lvgl.h>

namespace app_ui {

// Shared product design tokens. App pages should consume these values instead
// of introducing one-off colors, radii and spacing.
constexpr uint32_t kCanvas = 0xFFFFFF;
constexpr uint32_t kSurface = 0xFFFFFF;
constexpr uint32_t kSurfaceElevated = 0xFFFFFF;
constexpr uint32_t kInk = 0x172033;
constexpr uint32_t kInkMuted = 0x7B8496;
constexpr uint32_t kLine = 0xE7ECF3;
constexpr uint32_t kBrand = 0x45BE72;
constexpr uint32_t kBrandDark = 0x2E9F78;
constexpr uint32_t kHighlight = 0x7ECF54;

constexpr lv_coord_t kPagePadding = 16;
constexpr lv_coord_t kCardRadius = 18;
constexpr lv_coord_t kControlRadius = 14;
constexpr lv_coord_t kSectionGap = 14;
constexpr lv_coord_t kTouchTarget = 52;

// Typography scale is based on LVGL's 256 = 100%. Xiaoliang Touch uses a
// 20px bundled Chinese font, then these roles create a consistent hierarchy.
constexpr int32_t kTextScaleCaption = 205;    // about 16px
constexpr int32_t kTextScaleBody = 230;       // about 18px
constexpr int32_t kTextScaleButton = 256;     // 20px
constexpr int32_t kTextScaleCardTitle = 270;  // about 21px
constexpr int32_t kTextScaleSection = 282;    // about 22px
constexpr int32_t kTextScalePageTitle = 307;  // about 24px
constexpr int32_t kTextScaleHeroTitle = 333;  // about 26px

inline void StyleCaption(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleCaption, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(kInkMuted), 0);
}

inline void StyleBody(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleBody, 0);
}

inline void StyleButtonLabel(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleButton, 0);
}

inline void StyleCardTitle(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleCardTitle, 0);
}

inline void StyleSectionTitle(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleSection, 0);
}

inline void StylePageTitle(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScalePageTitle, 0);
}

inline void StyleHeroTitle(lv_obj_t* label) {
    lv_obj_set_style_transform_scale(label, kTextScaleHeroTitle, 0);
}

inline void StyleCard(lv_obj_t* object, uint32_t background = kSurfaceElevated) {
    lv_obj_set_style_bg_color(object, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, kCardRadius, 0);
    lv_obj_set_style_border_width(object, 1, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(kLine), 0);
    lv_obj_set_style_shadow_width(object, 10, 0);
    lv_obj_set_style_shadow_color(object, lv_color_hex(0x15251F), 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_10, 0);
    lv_obj_set_style_shadow_ofs_y(object, 4, 0);
}

inline void StylePressable(lv_obj_t* object) {
    lv_obj_set_style_transform_scale(object, 248, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(object, LV_OPA_80, LV_STATE_PRESSED);
}

}  // namespace app_ui
