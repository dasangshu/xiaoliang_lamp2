#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"
#include "mjpeg_player/mjpeg_player_port.h"
#include "boards/common/esp_video.h"
#include "display/lcd_display.h"

#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <sys/stat.h>

#define TAG "Application"
#define ENABLE_AUTO_POSTURE_DETECTION 1

namespace {
constexpr uint32_t kIdleMjpegPrerollMs = 1000;
constexpr uint32_t kLlmEmotionMinDisplayMs = 2500;

bool SdcardFileExists(const char* path) {
    struct stat st;
    return path != nullptr && stat(path, &st) == 0;
}

std::string ResolveLlmEmotionMjpeg(const std::string& emotion) {
    if (!emotion.empty() && emotion.find(".mjpeg") == std::string::npos) {
        return emotion + ".mjpeg";
    }
    return emotion;
}

const char* PostureAdviceText(posture_type_t type) {
    switch (type) {
        case POSTURE_LYING_DOWN:
            return "趴桌对颈椎压力很大，先坐直一下";
        case POSTURE_HEAD_SUPPORT:
            return "不要一直撑头，肩颈放松，坐回椅背";
        case POSTURE_SLOUCHING:
            return "低头弯腰有一会儿了，抬头看前方";
        case POSTURE_LEAN_BACK:
            return "身体后仰太多了，坐回桌前保持稳定";
        case POSTURE_TILTED:
            return "身体有点倾斜，双肩放平，慢慢坐正";
        default:
            return "坐姿有点不标准，调整一下就好";
    }
}

struct PostureFeedbackRestoreContext {
    Application* app = nullptr;
    uint32_t delay_ms = 3000;
};

}  // namespace


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    mjpeg_player_port_deinit();
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    bool ok = state_machine_.TransitionTo(state);
    if (ok && state == kDeviceStateIdle) {
        idle_entered_tick_ = xTaskGetTickCount();
    }
    return ok;
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Initialize MJPEG player for face animation playback
    mjpeg_player_port_config_t mjpeg_config = {
        .buffer_size = 300 * 1024,  // enough for larger 480x800 MJPEG JPEG frames
        .core_id = -1,     // let the scheduler place MJPEG; do not pin heavy JPEG decode to CPU0
        .use_psram = true,
        .task_priority = 1,    // keep MJPEG below audio and wake-word paths
        .target_fps = 8        // talk/bye are raised to 15fps per file in mjpeg_player_port
    };
    mjpeg_player_port_init(&mjpeg_config);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    display->SetHealthScore(100);
    display->SetEmotion("loading.mjpeg");
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    auto* startup_event_group = event_group_;
    xTaskCreate([](void* arg) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        xEventGroupSetBits(static_cast<EventGroupHandle_t>(arg), MAIN_EVENT_STARTUP_TIMEOUT);
        vTaskDelete(NULL);
    }, "startup_guard", 3072, startup_event_group, 1, nullptr);

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_STARTUP_TIMEOUT |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STARTUP_TIMEOUT) {
            HandleStartupTimeoutEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
            StopIdleMjpegIfNeeded(false);
            HandleEyeCareClockTick();
            HandleReminderClockTick();

#if ENABLE_AUTO_POSTURE_DETECTION
            if (posture_start_pending_ && GetDeviceState() == kDeviceStateIdle) {
                TickType_t now = xTaskGetTickCount();
                if ((now - idle_entered_tick_) * portTICK_PERIOD_MS >= POSTURE_IDLE_DELAY_MS) {
                    posture_start_pending_ = false;
                    StartPostureDetection();
                }
            }
#endif
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::HandleStartupTimeoutEvent() {
    auto state = GetDeviceState();
    if (state != kDeviceStateStarting && state != kDeviceStateActivating) {
        return;
    }

    ESP_LOGW(TAG, "Startup did not reach idle in time (state=%d), enabling local wake-word idle", (int)state);
    if (!protocol_) {
        InitializeProtocol();
    }
    SetDeviceState(kDeviceStateIdle);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("loading.mjpeg");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 2;
    int retry_count = 0;
    int retry_delay = 3; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay = 5;
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_ && ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_ && ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        Settings websocket_settings("websocket", false);
        Settings mqtt_settings("mqtt", false);
        if (!websocket_settings.GetString("url").empty()) {
            ESP_LOGW(TAG, "No OTA protocol config, using saved WebSocket settings");
            protocol_ = std::make_unique<WebsocketProtocol>();
        } else {
            ESP_LOGW(TAG, "No OTA protocol config, using saved MQTT settings");
            protocol_ = std::make_unique<MqttProtocol>();
        }
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    tts_session_id_++;
                    last_llm_emotion_tick_ = 0;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        struct TtsDrainContext {
                            Application* app;
                            uint32_t session_id;
                        };
                        auto* ctx = new TtsDrainContext{this, tts_session_id_};
                        xTaskCreate([](void* arg) {
                            auto* ctx = static_cast<TtsDrainContext*>(arg);
                            auto* app = ctx->app;
                            uint32_t wait_session_id = ctx->session_id;
                            delete ctx;
                            app->audio_service_.WaitForPlaybackQueueEmpty();
                            TickType_t last_emotion_tick = app->last_llm_emotion_tick_;
                            if (last_emotion_tick != 0) {
                                TickType_t elapsed = xTaskGetTickCount() - last_emotion_tick;
                                TickType_t min_display_ticks = pdMS_TO_TICKS(kLlmEmotionMinDisplayMs);
                                if (elapsed < min_display_ticks) {
                                    vTaskDelay(min_display_ticks - elapsed);
                                }
                            }
                            app->Schedule([app, wait_session_id]() {
                                if (app->tts_session_id_ != wait_session_id ||
                                    app->GetDeviceState() != kDeviceStateSpeaking) {
                                    return;
                                }
                                if (app->listening_mode_ == kListeningModeManualStop) {
                                    app->SetDeviceState(kDeviceStateIdle);
                                } else {
                                    app->SetDeviceState(kDeviceStateListening);
                                }
                            });
                            vTaskDelete(NULL);
                        }, "tts_drain", 4096, ctx, 2, nullptr);
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    auto state = GetDeviceState();
                    if (state == kDeviceStateListening) {
                        ESP_LOGI(TAG, "Ignore LLM emotion '%s' while listening", emotion_str.c_str());
                        return;
                    }
                    if (emotion_str == "laughing" || emotion_str == "laughing.mjpeg") {
                        ESP_LOGI(TAG, "Keep talk.mjpeg for LLM emotion '%s'", emotion_str.c_str());
                        return;
                    }
                    auto display_emotion = ResolveLlmEmotionMjpeg(emotion_str);
                    if (display_emotion != emotion_str) {
                        ESP_LOGI(TAG, "LLM emotion '%s' -> '%s'", emotion_str.c_str(), display_emotion.c_str());
                    }
                    last_llm_emotion_tick_ = xTaskGetTickCount();
                    display->SetEmotion(display_emotion.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        ShowIdleEmotion();
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState(const char* source) {
    pending_toggle_source_ = source ? source : "unknown";
    ESP_LOGI(TAG, "ToggleChatState requested: source=%s state=%d",
             pending_toggle_source_, (int)GetDeviceState());
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    const char* source = pending_toggle_source_;
    pending_toggle_source_ = "unknown";
    ESP_LOGI(TAG, "Handle toggle chat: source=%s state=%d", source, (int)state);
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        PrepareVoiceInteraction();
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGW(TAG, "Open audio channel failed, returning to idle");
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    ESP_LOGI(TAG, "Handle start listening event: state=%d", (int)state);
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        PrepareVoiceInteraction();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    constexpr uint32_t kWakeRejectAfterPlaybackMs = 900;
    auto state_before_prepare = GetDeviceState();
    if ((state_before_prepare == kDeviceStateIdle ||
         state_before_prepare == kDeviceStateListening ||
         state_before_prepare == kDeviceStateSpeaking) &&
        audio_service_.IsOutputActiveOrRecentlyActive(kWakeRejectAfterPlaybackMs)) {
        ++rejected_playback_wake_count_;
        auto now = xTaskGetTickCount();
        if ((now - last_wake_reject_log_tick_) * portTICK_PERIOD_MS > 3000) {
            ESP_LOGW(TAG, "Ignore wake word during/recent playback (state=%d, count=%lu)",
                     (int)state_before_prepare, (unsigned long)rejected_playback_wake_count_);
            last_wake_reject_log_tick_ = now;
        }
        if (state_before_prepare == kDeviceStateIdle && !audio_service_.IsWakeWordRunning()) {
            audio_service_.EnableWakeWordDetection(true);
        }
        return;
    }

    PrepareVoiceInteraction();

    if (!protocol_) {
        ESP_LOGW(TAG, "Wake word detected before protocol init, initializing protocol");
        InitializeProtocol();
        if (!protocol_) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ShowIdleEmotion() {
    auto display = Board::GetInstance().GetDisplay();
    uint32_t session_id = ++idle_emotion_session_id_;
    audio_service_.EnableWakeWordDetection(false);
    idle_mjpeg_started_tick_ = xTaskGetTickCount();
    idle_mjpeg_active_ = true;
    display->SetEmotion("idle.mjpeg");

    struct IdleEmotionContext {
        Application* app;
        uint32_t session_id;
    };
    auto* ctx = new IdleEmotionContext{this, session_id};
    BaseType_t task_ok = xTaskCreate([](void* arg) {
        auto* ctx = static_cast<IdleEmotionContext*>(arg);
        Application* app = ctx->app;
        uint32_t session_id = ctx->session_id;
        delete ctx;

        vTaskDelay(pdMS_TO_TICKS(kIdleMjpegPrerollMs));
        app->Schedule([app, session_id]() {
            app->FinishIdleEmotionPreroll(session_id);
        });
        vTaskDelete(NULL);
    }, "idle_mjpeg_guard", 2048, ctx, 1, nullptr);

    if (task_ok != pdPASS) {
        delete ctx;
        ESP_LOGW(TAG, "Failed to create idle MJPEG guard task");
        FinishIdleEmotionPreroll(session_id);
    }
}

void Application::FinishIdleEmotionPreroll(uint32_t session_id) {
    if (idle_emotion_session_id_ != session_id || GetDeviceState() != kDeviceStateIdle) {
        return;
    }

    StopIdleMjpegIfNeeded(true);
    audio_service_.EnableWakeWordDetection(true);
}

void Application::StopIdleMjpegIfNeeded(bool force) {
    if (!idle_mjpeg_active_) {
        return;
    }
    if (!force) {
        if (GetDeviceState() != kDeviceStateIdle) {
            idle_mjpeg_active_ = false;
            return;
        }
        TickType_t now = xTaskGetTickCount();
        if ((now - idle_mjpeg_started_tick_) * portTICK_PERIOD_MS < kIdleMjpegPrerollMs) {
            return;
        }
    }

    idle_mjpeg_active_ = false;
    auto display = Board::GetInstance().GetDisplay();
    if (auto lcd_display = dynamic_cast<LcdDisplay*>(display)) {
        (void)lcd_display->ShowStaticIdleFace();
    } else {
        mjpeg_player_port_stop_wait(800);
        display->SetEmotion("neutral");
    }
}

void Application::PrepareVoiceInteraction() {
    ++idle_emotion_session_id_;
    idle_mjpeg_active_ = false;
    posture_start_pending_ = false;
    posture_light_prompt_sent_ = false;
    posture_voice_prompt_sent_ = false;
    if (posture_detector_) {
        posture_detector_->RequestStop();
    }
    mjpeg_player_port_stop_wait(800);
    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            ESP_LOGW(TAG, "Open audio channel failed after wake word, returning to idle");
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);

    // Set flag to play popup sound after state changes to listening
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
        case kDeviceStateAudioTesting:
        case kDeviceStateFatalError:
            posture_start_pending_ = false;
            StopPostureDetection();
            break;
        case kDeviceStateStarting:
            display->SetEmotion("loading.mjpeg");
            posture_start_pending_ = false;
            StopPostureDetection();
            break;
        case kDeviceStateIdle: {
            display->ClearChatMessages();  // Clear messages first
            audio_service_.EnableVoiceProcessing(false);
            ShowIdleEmotion();
            display->UpdateStatusBar(true);
            posture_start_pending_ = ENABLE_AUTO_POSTURE_DETECTION;
            break;
        }
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("listen.mjpeg");
            display->SetChatMessage("system", "");
            posture_start_pending_ = false;
            StopPostureDetection();
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("listen.mjpeg");
            posture_start_pending_ = false;
            StopPostureDetection();

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            display->SetEmotion("talk.mjpeg");
            posture_start_pending_ = false;
            StopPostureDetection();

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            posture_start_pending_ = false;
            StopPostureDetection();
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        PrepareVoiceInteraction();
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

// ---------------------------------------------------------------------------
// 坐姿检测集成
// ---------------------------------------------------------------------------

void Application::StartPostureDetection() {
    if (!eye_care_service_.config().posture_enabled) {
        posture_start_pending_ = false;
        return;
    }

    if (GetDeviceState() != kDeviceStateIdle ||
        !audio_service_.IsWakeWordRunning() ||
        !audio_service_.IsIdle()) {
        ESP_LOGW("Application", "Skip posture detection start while voice path is busy");
        posture_start_pending_ = true;
        return;
    }

    auto camera = Board::GetInstance().GetCamera();
    if (!camera) return;

    auto* esp_video = dynamic_cast<EspVideo*>(camera);
    if (!esp_video) return;

    if (!posture_detector_) {
        posture_detector_ = std::make_unique<PostureDetector>();
    }

    if (posture_detector_->IsRunning()) return;

    // Keep posture detection invisible: stop the idle MJPEG decoder to free CPU/PSRAM,
    // but do not change the visible emotion/status or show camera frames.
    mjpeg_player_port_stop_wait(800);

    posture_detector_->Start(esp_video, [this](const posture_result_t& result) {
        OnPostureResult(result);
    });
    ESP_LOGI("Application", "坐姿检测已启动");
}

void Application::StopPostureDetection() {
    if (posture_detector_) {
        posture_detector_->Stop();
        ESP_LOGI("Application", "坐姿检测已停止");
    }
}

void Application::OnPostureResult(const posture_result_t& result) {
    eye_care_service_.OnPresenceDetected(result.detected);

    if (!result.detected || result.posture_type == POSTURE_UNKNOWN) {
        if (posture_bad_active_) {
            ESP_LOGI("Application", "坐姿检测无人或无效，清除异常状态");
        }
        posture_bad_active_ = false;
        posture_light_prompt_sent_ = false;
        posture_voice_prompt_sent_ = false;
        posture_active_type_ = POSTURE_UNKNOWN;
        posture_bad_start_tick_ = 0;
        posture_good_start_tick_ = 0;
        Schedule([]() {
            if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
            }
        });
        return;
    }

    TickType_t now = xTaskGetTickCount();

    if (result.posture_type == POSTURE_NORMAL) {
        if (posture_good_start_tick_ == 0) {
            posture_good_start_tick_ = now;
        }

        if (posture_bad_active_) {
            bool should_reward = (posture_light_prompt_sent_ || posture_voice_prompt_sent_) &&
                (now - posture_last_corrected_reward_tick_ >= pdMS_TO_TICKS(POSTURE_CORRECTED_REWARD_INTERVAL_MS));
            posture_bad_active_ = false;
            posture_light_prompt_sent_ = false;
            posture_voice_prompt_sent_ = false;
            posture_active_type_ = POSTURE_UNKNOWN;
            posture_bad_start_tick_ = 0;
            ESP_LOGI("Application", "坐姿已纠正");
            AdjustPostureHealthScore(2);
            Schedule([]() {
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
            });
            if (should_reward) {
                posture_last_corrected_reward_tick_ = now;
                SchedulePostureFeedback("很好，坐姿恢复啦，继续保持", "loving.mjpeg", "", 1800);
            }
            return;
        }

        if (now - posture_last_health_tick_ >= pdMS_TO_TICKS(60 * 1000)) {
            posture_last_health_tick_ = now;
            AdjustPostureHealthScore(1);
        }

        if (now - posture_good_start_tick_ >= pdMS_TO_TICKS(POSTURE_GOOD_STREAK_MS) &&
            now - posture_last_good_reward_tick_ >= pdMS_TO_TICKS(POSTURE_GOOD_STREAK_MS)) {
            posture_last_good_reward_tick_ = now;
            AdjustPostureHealthScore(3);
            SchedulePostureFeedback("已保持良好坐姿 25 分钟，健康值提升", "confident.mjpeg", "", 1800);
        }
        return;
    }

    posture_good_start_tick_ = 0;

    if (!posture_bad_active_ || posture_active_type_ != result.posture_type) {
        posture_bad_active_ = true;
        posture_light_prompt_sent_ = false;
        posture_voice_prompt_sent_ = false;
        posture_active_type_ = result.posture_type;
        posture_bad_start_tick_ = now;
        ESP_LOGI("Application", "坐姿异常开始: %s", result.status_text);
        return;
    }

    TickType_t bad_duration = now - posture_bad_start_tick_;

    if (!posture_light_prompt_sent_ && bad_duration >= pdMS_TO_TICKS(POSTURE_LIGHT_PROMPT_MS)) {
        posture_light_prompt_sent_ = true;
        AdjustPostureHealthScore(-1);
        ESP_LOGI("Application", "坐姿轻提醒: %s", result.status_text);
        const char* advice = PostureAdviceText(result.posture_type);
        Schedule([this, advice]() {
            if (GetDeviceState() != kDeviceStateIdle) return;
            Board::GetInstance().GetDisplay()->SetChatMessage("system", advice);
            xTaskCreate([](void* arg) {
                auto* app = static_cast<Application*>(arg);
                vTaskDelay(pdMS_TO_TICKS(4000));
                app->Schedule([app]() {
                    if (app->GetDeviceState() == kDeviceStateIdle) {
                        Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
                    }
                });
                vTaskDelete(NULL);
            }, "posture_msg_clear", 2048, this, 1, nullptr);
        });
        return;
    }

    if (!posture_voice_prompt_sent_ && bad_duration >= pdMS_TO_TICKS(POSTURE_VOICE_PROMPT_MS)) {
        if (now - posture_last_alert_tick_ < pdMS_TO_TICKS(POSTURE_ALERT_INTERVAL_MS)) {
            return;
        }
        posture_voice_prompt_sent_ = true;
        posture_last_alert_tick_ = now;
        AdjustPostureHealthScore(-3);
        ESP_LOGI("Application", "坐姿语音提醒: %s", result.status_text);
        const char* advice = PostureAdviceText(result.posture_type);
        const auto& sound = (result.posture_type == POSTURE_SLOUCHING || result.posture_type == POSTURE_LYING_DOWN)
            ? Lang::Sounds::OGG_POSE2
            : Lang::Sounds::OGG_POSE1;
        SchedulePostureFeedback(advice, "bye.mjpeg", sound, 3000);
        return;
    }
}

void Application::AdjustPostureHealthScore(int delta) {
    Schedule([this, delta]() {
        posture_health_score_ += delta;
        if (posture_health_score_ < 0) {
            posture_health_score_ = 0;
        } else if (posture_health_score_ > 100) {
            posture_health_score_ = 100;
        }
        Board::GetInstance().GetDisplay()->SetHealthScore(posture_health_score_);
    });
}

void Application::SchedulePostureFeedback(const char* message, const char* emotion, const std::string_view& sound, uint32_t restore_delay_ms) {
    std::string message_copy = message ? message : "";
    std::string emotion_copy = emotion ? emotion : "idle.mjpeg";
    std::string sound_copy(sound);

    Schedule([this, message_copy, emotion_copy, sound_copy, restore_delay_ms]() {
        if (GetDeviceState() != kDeviceStateIdle) return;
        posture_start_pending_ = false;
        StopPostureDetection();

        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::WARNING);
        if (audio_service_.IsWakeWordRunning()) {
            ESP_LOGW(TAG, "Skip posture MJPEG while wake word detection is running");
        } else {
            display->SetEmotion(emotion_copy.c_str());
        }
        display->SetChatMessage("system", message_copy.c_str());
        if (!sound_copy.empty() && audio_service_.IsIdle()) {
            audio_service_.PlaySound(std::string_view(sound_copy.data(), sound_copy.size()));
        } else if (!sound_copy.empty()) {
            ESP_LOGW(TAG, "Skip posture prompt sound while audio service is busy");
        }

        auto* ctx = new PostureFeedbackRestoreContext{this, restore_delay_ms};
        BaseType_t task_ok = xTaskCreate([](void* p) {
            auto* ctx = static_cast<PostureFeedbackRestoreContext*>(p);
            Application* app = ctx->app;
            uint32_t delay_ms = ctx->delay_ms;
            delete ctx;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            app->Schedule([app]() {
                if (app->GetDeviceState() == kDeviceStateIdle) {
                    app->DismissAlert();
                    app->posture_start_pending_ = true;
                }
            });
            vTaskDelete(NULL);
        }, "posture_pet", 2048, ctx, 1, nullptr);
        if (task_ok != pdPASS) {
            delete ctx;
            ESP_LOGW(TAG, "Failed to create posture feedback restore task");
            DismissAlert();
            posture_start_pending_ = true;
        }
    });
}

void Application::HandleEyeCareClockTick() {
    auto event = eye_care_service_.OnClockTick(GetDeviceState() == kDeviceStateIdle);
    if (event.type != EyeCareService::EventType::kNone) {
        ShowEyeCareFeedback(event);
    }
}

void Application::ShowEyeCareFeedback(const EyeCareService::Event& event) {
    if (GetDeviceState() != kDeviceStateIdle || event.message.empty()) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    const uint32_t clear_delay_ms = event.display_ms == 0 ? 3000 : event.display_ms;
    display->ShowNotification(event.message.c_str(), clear_delay_ms);
    display->SetChatMessage("system", event.message.c_str());

    if (!event.emotion.empty() &&
        eye_care_service_.config().screen_video_enabled &&
        !audio_service_.IsWakeWordRunning()) {
        display->SetEmotion(event.emotion.c_str());
    }

    if (event.play_sound && audio_service_.IsIdle()) {
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }

    xTaskCreate([](void* arg) {
        auto* delay_ms = static_cast<uint32_t*>(arg);
        uint32_t wait_ms = *delay_ms;
        delete delay_ms;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
            }
        });
        vTaskDelete(NULL);
    }, "eye_care_msg", 2048, new uint32_t(clear_delay_ms), 1, nullptr);
}

cJSON* Application::GetEyeCareStatusJson() {
    return eye_care_service_.ToJson();
}

void Application::ConfigureEyeCare(const EyeCareService::Config& config) {
    eye_care_service_.ApplyConfig(config);
    if (!eye_care_service_.config().enabled || !eye_care_service_.config().posture_enabled) {
        posture_start_pending_ = false;
        StopPostureDetection();
    } else if (GetDeviceState() == kDeviceStateIdle) {
        posture_start_pending_ = true;
    }
}

void Application::ResetEyeCareDailyStats() {
    eye_care_service_.ResetDailyStats();
}

void Application::HandleReminderClockTick() {
    auto due = reminder_service_.PopDueReminders(time(nullptr));
    for (const auto& reminder : due) {
        ShowReminder(reminder);
    }
}

void Application::ShowReminder(const ReminderService::DueReminder& reminder) {
    std::string message = "提醒：" + reminder.content;
    Schedule([this, message]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(message.c_str(), 10000);
        display->SetChatMessage("system", message.c_str());
        if (audio_service_.IsIdle()) {
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
        }

        xTaskCreate([](void* arg) {
            auto* app = static_cast<Application*>(arg);
            vTaskDelay(pdMS_TO_TICKS(10000));
            app->Schedule([app]() {
                if (app->GetDeviceState() == kDeviceStateIdle) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");
                }
            });
            vTaskDelete(NULL);
        }, "reminder_clear", 2048, this, 1, nullptr);
    });
}

bool Application::AddReminderFromText(const std::string& text, ReminderService::ReminderTask& out) {
    return reminder_service_.AddFromText(text, out);
}

bool Application::AddReminderAt(int64_t remind_at, const std::string& content, ReminderService::ReminderTask& out) {
    return reminder_service_.AddTask(ReminderService::ReminderType::kAbsolute, remind_at, content, "", out);
}

bool Application::DeleteReminder(uint32_t id) {
    return reminder_service_.DeleteTask(id);
}

cJSON* Application::GetRemindersJson() {
    return reminder_service_.ToJson();
}
