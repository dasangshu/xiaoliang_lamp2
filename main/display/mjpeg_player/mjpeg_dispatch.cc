// This file bridges the C MJPEG player callback to the C++ Display layer
// It is included in the build and provides mjpeg_dispatch_frame()
// which safely forwards decoded frames to the display's SetFaceImage().

#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "mjpeg_dispatch";

// Frame data is passed directly; display owns the copy in SetFaceImage()

extern "C" void mjpeg_dispatch_frame(uint8_t *rgb565, uint32_t width, uint32_t height) {
    auto display = dynamic_cast<LvglDisplay *>(Board::GetInstance().GetDisplay());
    if (display) {
        display->SetFaceImage(rgb565, width, height);
    }
}
