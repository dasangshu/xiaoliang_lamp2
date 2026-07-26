#pragma once

#include <cstddef>
#include <cstdint>

namespace app_ui {

enum class AppKind {
    kTask,
    kPomodoro,
    kPet,
    kEyeIsland,
    kMusic,
    kAiSpeaking,
    kHealth,
    kDevice,
    kMore,
};

struct AppModule {
    using Kind = AppKind;
    AppKind kind;
    const char* icon;
    const char* image_asset;
    const char* title;
    const char* subtitle;
    const char* summary;
    const char* badge;
    uint32_t accent;
    uint32_t tint;
    const char* const* actions;
    size_t action_count;
};

extern const char* const kTaskActions[3];
extern const char* const kPomodoroActions[3];
extern const char* const kEyeIslandActions[3];
extern const char* const kPetActions[3];
extern const char* const kMusicActions[3];
extern const char* const kAiSpeakActions[3];
extern const char* const kHealthActions[3];
extern const char* const kDeviceActions[3];
extern const char* const kMoreActions[3];

const AppModule* GetAppModules();
size_t GetAppModuleCount();
const AppModule* FindAppModule(AppKind kind);

}  // namespace app_ui
