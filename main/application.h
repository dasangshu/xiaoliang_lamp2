#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <cJSON.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "posture/posture_detector.h"
#include "eye_care/eye_care_service.h"
#include "reminders/reminder_service.h"

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_STARTUP_TIMEOUT      (1 << 13)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState(const char* source = "unknown");

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();
    void StartAiScenario(const std::string& title, const std::string& command);

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }
    cJSON* GetEyeCareStatusJson();
    void ConfigureEyeCare(const EyeCareService::Config& config);
    void ResetEyeCareDailyStats();
    EyeCareService::Config GetEyeCareConfig() const { return eye_care_service_.config(); }
    bool AddReminderFromText(const std::string& text, ReminderService::ReminderTask& out);
    bool AddReminderAt(int64_t remind_at, const std::string& content, ReminderService::ReminderTask& out);
    bool DeleteReminder(uint32_t id);
    cJSON* GetRemindersJson();
    
    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;
    EyeCareService eye_care_service_;
    ReminderService reminder_service_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool continue_listening_after_tts_ = false;
    std::string pending_ai_scenario_command_;
    const char* pending_toggle_source_ = "unknown";
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;
    uint32_t tts_session_id_ = 0;
    bool tts_audio_active_ = false;
    TickType_t last_llm_emotion_tick_ = 0;
    uint32_t idle_emotion_session_id_ = 0;
    TickType_t idle_mjpeg_started_tick_ = 0;
    bool idle_mjpeg_active_ = false;
    TickType_t last_wake_reject_log_tick_ = 0;
    uint32_t rejected_playback_wake_count_ = 0;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleStartupTimeoutEvent();
    void HandleWakeWordDetectedEvent();
    void ShowIdleEmotion();
    void FinishIdleEmotionPreroll(uint32_t session_id);
    void StopIdleMjpegIfNeeded(bool force = false);
    void PrepareVoiceInteraction();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void ContinueAiScenario(const std::string& title, const std::string& command);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);

    // 坐姿检测
    std::unique_ptr<PostureDetector> posture_detector_;
    TickType_t posture_last_alert_tick_ = 0;
    static constexpr uint32_t POSTURE_ALERT_INTERVAL_MS = 30 * 1000;  // 演示模式：两次语音提醒最短间隔30秒
    int posture_health_score_ = 100;
    bool posture_bad_active_ = false;
    bool posture_light_prompt_sent_ = false;
    bool posture_voice_prompt_sent_ = false;
    posture_type_t posture_active_type_ = POSTURE_UNKNOWN;
    TickType_t posture_bad_start_tick_ = 0;
    TickType_t posture_good_start_tick_ = 0;
    TickType_t posture_last_health_tick_ = 0;
    TickType_t posture_last_corrected_reward_tick_ = 0;
    TickType_t posture_last_good_reward_tick_ = 0;
    static constexpr uint32_t POSTURE_LIGHT_PROMPT_MS = 5 * 1000;
    static constexpr uint32_t POSTURE_VOICE_PROMPT_MS = 12 * 1000;
    static constexpr uint32_t POSTURE_CORRECTED_REWARD_INTERVAL_MS = 30 * 1000;
    static constexpr uint32_t POSTURE_GOOD_STREAK_MS = 3 * 60 * 1000;

    // 进入 idle 的时刻，用于延迟后台坐姿检测启动
    TickType_t idle_entered_tick_ = 0;
    bool posture_start_pending_ = false;
    static constexpr uint32_t POSTURE_IDLE_DELAY_MS = 10 * 1000;  // 演示模式：idle 稳定10秒后启动坐姿检测
    void StartPostureDetection();
    void StopPostureDetection();
    void OnPostureResult(const posture_result_t& result);
    void AdjustPostureHealthScore(int delta);
    void SchedulePostureFeedback(const char* message, const char* emotion, const std::string_view& sound, uint32_t restore_delay_ms);
    void HandleEyeCareClockTick();
    void ShowEyeCareFeedback(const EyeCareService::Event& event);
    void HandleReminderClockTick();
    void ShowReminder(const ReminderService::DueReminder& reminder);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
