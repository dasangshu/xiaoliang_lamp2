#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "button.h"
#include "config.h"
#include "lcd_init_cmds.h"
#include "mcp_server.h"
#include "led/single_led.h"

#include "esp_video.h"
#include "esp_video_init.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <cstring>

#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#define TAG "Esp32P4c5Sangshu"

static SemaphoreHandle_t touch_wake_sem = nullptr;

class Esp32P4c5SangshuBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    i2c_master_bus_handle_t camera_i2c_bus_ = nullptr;
    Button boot_button_;
    LcdDisplay* display_ = nullptr;
    EspVideo* camera_ = nullptr;

    static esp_err_t EnableDsiPhyPower() {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        static esp_ldo_channel_handle_t phy_pwr_chan = nullptr;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY powered on");
#endif
        return ESP_OK;
    }

    void ResetLcdAndTouch() {
        gpio_config_t rst_cfg = {
            .pin_bit_mask = (1ULL << PIN_NUM_LCD_RST),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&rst_cfg));
        gpio_set_level(PIN_NUM_LCD_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_NUM_LCD_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeCameraI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CAMERA_I2C_SDA_PIN,
            .scl_io_num = CAMERA_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &camera_i2c_bus_));
    }

    void InitializeLCD() {
        ResetLcdAndTouch();
        EnableDsiPhyPower();

        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_panel_handle_t disp_panel = nullptr;
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;

        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
            .lane_bit_rate_mbps = LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus));

        esp_lcd_dbi_io_config_t dbi_config = {
            .virtual_channel = 0,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io));

        esp_lcd_dpi_panel_config_t dpi_config = {
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 20,
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs = 3,
            .video_timing = {
                .h_size = 480,
                .v_size = 800,
                .hsync_pulse_width = 4,
                .hsync_back_porch = 32,
                .hsync_front_porch = 32,
                .vsync_pulse_width = 4,
                .vsync_back_porch = 10,
                .vsync_front_porch = 10,
            },
            .flags = {
                .use_dma2d = true,
            },
        };

        st7701_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
            },
            .flags = {
                .use_mipi_interface = 1,
            },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = GPIO_NUM_NC,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(io, &lcd_dev_config, &disp_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(disp_panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(disp_panel));
        // MIPI-DPI uses DMA2D; LVGL rotation.mirror_x does not flip scan direction.
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(disp_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        if (DISPLAY_SWAP_XY) {
            ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(disp_panel, true));
        }

        display_ = new MipiLcdDisplay(io, disp_panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                      DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                      false, false, false);
    }

    static void TouchWakeIsr(void* arg) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (touch_wake_sem != nullptr) {
            xSemaphoreGiveFromISR(touch_wake_sem, &xHigherPriorityTaskWoken);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }

    static void TouchWakeTask(void* arg) {
        bool was_touched = false;
        while (true) {
            if (xSemaphoreTake(touch_wake_sem, portMAX_DELAY) != pdTRUE) {
                continue;
            }

            bool is_touched = (gpio_get_level(TOUCH_WAKE_GPIO) == 0);
            if (is_touched == was_touched) {
                continue;
            }
            was_touched = is_touched;

            if (!is_touched) {
                continue;
            }

            auto& app = Application::GetInstance();
            auto& board = static_cast<WifiBoard&>(Board::GetInstance());
            if (app.GetDeviceState() == kDeviceStateStarting) {
                board.EnterWifiConfigMode();
            } else {
                // ToggleChatState supports device AEC barge-in while speaking
                app.ToggleChatState();
            }
        }
    }

    void InitializeTouchWake() {
        touch_wake_sem = xSemaphoreCreateBinary();

        gpio_config_t int_gpio_config = {
            .pin_bit_mask = (1ULL << TOUCH_WAKE_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&int_gpio_config));
        ESP_ERROR_CHECK(gpio_install_isr_service(0));
        ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_WAKE_GPIO, TouchWakeIsr, nullptr));

        xTaskCreate(TouchWakeTask, "touch_wake", 4096, nullptr, 5, nullptr);
        ESP_LOGI(TAG, "CT8233 touch wake initialized on GPIO%d", TOUCH_WAKE_GPIO);
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = false,
            .i2c_handle = camera_i2c_bus_,
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);
    }

    void InitializeLampUart() {
        uart_config_t uart_config = {
            .baud_rate = LAMP_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(LAMP_UART_PORT_NUM, LAMP_UART_BUF_SIZE * 2, 0, 0, nullptr, 0));
        ESP_ERROR_CHECK(uart_param_config(LAMP_UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(LAMP_UART_PORT_NUM, LAMP_UART_TX_PIN, LAMP_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_LOGI(TAG, "Lamp UART initialized on TX=%d RX=%d", LAMP_UART_TX_PIN, LAMP_UART_RX_PIN);
    }

    void SendLampCommand(const char* command) {
        size_t len = strlen(command);
        uart_write_bytes(LAMP_UART_PORT_NUM, command, len);
        uart_write_bytes(LAMP_UART_PORT_NUM, "\n", 1);
        ESP_LOGI(TAG, "Lamp UART command: %s", command);
    }

    void InitializeLampTools() {
        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.lamp.turn_on", "Turn on the external desk lamp", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendLampCommand("ON");
            return true;
        });

        mcp_server.AddTool("self.lamp.turn_off", "Turn off the external desk lamp", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SendLampCommand("OFF");
            return true;
        });

        mcp_server.AddTool("self.lamp.send_command", "Send a raw UART command to the external desk lamp controller",
            PropertyList({Property("command", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                SendLampCommand(properties["command"].value<std::string>().c_str());
                return true;
            });
    }

    void InitializeSdCard() {
#if SDCARD_SDMMC_ENABLED
        sd_pwr_ctrl_handle_t sd_ldo = nullptr;
        sd_pwr_ctrl_ldo_config_t ldo_cfg = {.ldo_chan_id = 4};
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &sd_ldo) == ESP_OK) {
            ESP_LOGI(TAG, "SD LDO channel 4 enabled");
        }

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        slot_config.clk = SDCARD_SDMMC_CLK_PIN;
        slot_config.cmd = SDCARD_SDMMC_CMD_PIN;
        slot_config.d0 = SDCARD_SDMMC_D0_PIN;
        slot_config.width = SDCARD_SDMMC_BUS_WIDTH;
        if (SDCARD_SDMMC_BUS_WIDTH == 4) {
            slot_config.d1 = SDCARD_SDMMC_D1_PIN;
            slot_config.d2 = SDCARD_SDMMC_D2_PIN;
            slot_config.d3 = SDCARD_SDMMC_D3_PIN;
        }

        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 0,
            .disk_status_check_enable = true,
        };

        sdmmc_card_t* card = nullptr;
        host.pwr_ctrl_handle = sd_ldo;
        esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK) {
            sdmmc_card_print_info(stdout, card);
            ESP_LOGI(TAG, "SD card mounted at %s", SDCARD_MOUNT_POINT);
        } else {
            ESP_LOGW(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        }
#endif
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void ConfigureWifiRemote() override {
        esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Set WiFi band mode AUTO failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi band mode: 2.4G + 5G auto (ESP32-C5 slave)");
        }
    }

public:
    Esp32P4c5SangshuBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeCameraI2c();
        InitializeLCD();
        InitializeTouchWake();
        InitializeLampUart();
        InitializeLampTools();
        // InitializeSdCard();
        InitializeCamera();
        InitializeButtons();
        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }
};

DECLARE_BOARD(Esp32P4c5SangshuBoard);
