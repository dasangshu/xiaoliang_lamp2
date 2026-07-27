#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/uart.h>

#define AUDIO_INPUT_SAMPLE_RATE         24000
#define AUDIO_OUTPUT_SAMPLE_RATE        24000
#define AUDIO_INPUT_REFERENCE           true

/* ES8311 + ES7210 box audio (I2S0) */
#define AUDIO_I2S_GPIO_MCLK             GPIO_NUM_7
#define AUDIO_I2S_GPIO_WS               GPIO_NUM_5
#define AUDIO_I2S_GPIO_BCLK             GPIO_NUM_6
#define AUDIO_I2S_GPIO_DIN              GPIO_NUM_8
#define AUDIO_I2S_GPIO_DOUT             GPIO_NUM_4
#define AUDIO_CODEC_PA_PIN              GPIO_NUM_1

#define AUDIO_CODEC_I2C_SDA_PIN         GPIO_NUM_9
#define AUDIO_CODEC_I2C_SCL_PIN         GPIO_NUM_10
#define AUDIO_CODEC_ES8311_ADDR         ES8311_CODEC_DEFAULT_ADDR
#define AUDIO_CODEC_ES7210_ADDR         ES7210_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO                GPIO_NUM_35

/* MIPI DSI LCD 480x800 + optional capacitive touch (I2C on codec bus) */
#define DISPLAY_WIDTH                   480
#define DISPLAY_HEIGHT                  800
#define PIN_NUM_LCD_RST                 GPIO_NUM_11
#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_23
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define TOUCH_I2C_SDA_PIN               AUDIO_CODEC_I2C_SDA_PIN
#define TOUCH_I2C_SCL_PIN               AUDIO_CODEC_I2C_SCL_PIN
#define TOUCH_PANEL_INT_GPIO            GPIO_NUM_20
#define TOUCH_PANEL_RST_GPIO            GPIO_NUM_NC  /* shared with LCD_RST */

#define LCD_MIPI_DSI_LANE_NUM           2
#define LCD_MIPI_DSI_LANE_BITRATE_MBPS  500

#define MIPI_DSI_PHY_PWR_LDO_CHAN       3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500

/* MIPI ST7701: mirror via esp_lcd_panel_mirror() in board init, not LVGL rotation */
#define DISPLAY_SWAP_XY                 false
#define DISPLAY_MIRROR_X                true
#define DISPLAY_MIRROR_Y                true
#define DISPLAY_OFFSET_X                0
#define DISPLAY_OFFSET_Y                0

/* CT8233 touch wake (digital output TOUCH_H, active low when touched) */
#define TOUCH_WAKE_GPIO                 GPIO_NUM_45

/* DVP camera (DCMI) + dedicated I2C1 for SCCB */
#define CAMERA_I2C_SDA_PIN              GPIO_NUM_46
#define CAMERA_I2C_SCL_PIN              GPIO_NUM_47

#define CAMERA_PIN_PWDN                 GPIO_NUM_NC
#define CAMERA_PIN_RESET                GPIO_NUM_NC
#define CAMERA_PIN_XCLK                 GPIO_NUM_32
#define CAMERA_PIN_VSYNC                GPIO_NUM_48
#define CAMERA_PIN_HREF                 GPIO_NUM_49
#define CAMERA_PIN_PCLK                 GPIO_NUM_29

#define CAMERA_PIN_D0                   GPIO_NUM_27
#define CAMERA_PIN_D1                   GPIO_NUM_12
#define CAMERA_PIN_D2                   GPIO_NUM_13
#define CAMERA_PIN_D3                   GPIO_NUM_26
#define CAMERA_PIN_D4                   GPIO_NUM_28
#define CAMERA_PIN_D5                   GPIO_NUM_30
#define CAMERA_PIN_D6                   GPIO_NUM_31
#define CAMERA_PIN_D7                   GPIO_NUM_33
#define XCLK_FREQ_HZ                    20000000

/* External desk lamp UART */
#define LAMP_UART_PORT_NUM              UART_NUM_2
#define LAMP_UART_TX_PIN                GPIO_NUM_51
#define LAMP_UART_RX_PIN                GPIO_NUM_50
#define LAMP_UART_BAUD_RATE             115200
#define LAMP_UART_BUF_SIZE              256

/* WS2812 status LED */
#define BUILTIN_LED_GPIO                GPIO_NUM_34

/* SD card (SDMMC 4-bit) */
#define SDCARD_SDMMC_ENABLED            1
#define SDCARD_SDMMC_BUS_WIDTH          4
#define SDCARD_SDMMC_CLK_PIN            GPIO_NUM_43
#define SDCARD_SDMMC_CMD_PIN            GPIO_NUM_44
#define SDCARD_SDMMC_D0_PIN             GPIO_NUM_39
#define SDCARD_SDMMC_D1_PIN             GPIO_NUM_40
#define SDCARD_SDMMC_D2_PIN             GPIO_NUM_41
#define SDCARD_SDMMC_D3_PIN             GPIO_NUM_42
#define SDCARD_MOUNT_POINT              "/sdcard"

#endif // _BOARD_CONFIG_H_
