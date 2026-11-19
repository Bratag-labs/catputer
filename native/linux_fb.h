// Linux framebuffer mode for the native preview (Cardputer Zero launcher).
//
// The M5 APPLaunch launcher starts apps with only HOME and XDG_RUNTIME_DIR.
// Without WAYLAND_DISPLAY, SDL's Wayland backend falls back to `wayland-0`,
// which on the Zero is labwc driving the HDMI output: the window is created
// but never reaches the built-in panel. In this mode the preview instead
//   - draws each frame straight to /dev/fb0 (RGB565, scaled to the panel),
//   - reads the keyboard from evdev with EVIOCGRAB, so the launcher's lock
//     screen and its own Esc-hold timeout never see the keys, and feeds them
//     to the normal loop as SDL key events,
//   - turns the controller's auto-dim into the real backlight (never 0: that
//     is "screen off" to the launcher and cz-battery-saver), restoring the
//     user's level on wake and on exit.
// SDL is still used for audio, timing and the event queue.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lofi_fb {

// ---- pure helpers (unit tested) ----

// Use the framebuffer automatically when nothing tells SDL where to draw.
bool autoWanted(const char* waylandDisplay, const char* x11Display, const char* sdlVideoDriver);
// SDL keycode for a Linux KEY_* code, or 0 to ignore the key. Volume and
// SYMB-layer keys come back as the masked scancode keysyms SDL reports for
// them on the device, so the preview's volumeKey() mapping applies unchanged.
std::int32_t sdlKeyFor(unsigned evdevCode);
// Nearest-neighbour copy of an sw x sh RGB565 image into a dw x dh buffer,
// black outside the image. fit=true scales to the panel height keeping the
// aspect ratio; fit=false draws 1:1. Both are centred.
void compose(const std::uint16_t* src, int sw, int sh, std::uint16_t* dst, int dw, int dh, bool fit);
// Backlight level while dimmed: 2% of the range, never 0, never above `level`.
int dimLevelFor(int level, int max);

// ---- devices ----

class Panel {
public:
    ~Panel();
    bool open(const char* path, std::string& error);
    int width() const { return width_; }
    int height() const { return height_; }
    // Shows an sw x sh RGB565 image (skipped when unchanged).
    void present(const std::uint16_t* src, int sw, int sh, bool fit);
private:
    int fd_ = -1, width_ = 0, height_ = 0, stride_ = 0;
    std::size_t size_ = 0;
    std::uint8_t* map_ = nullptr;
    std::vector<std::uint16_t> back_, last_;
};

struct KeyEvent { std::int32_t sym; bool down; bool repeat; };

class Keyboard {
public:
    ~Keyboard();
    // Opens the evdev device named `name` and grabs it.
    bool open(const char* name, std::string& error);
    void poll(std::vector<KeyEvent>& out);
private:
    int fd_ = -1;
};

class Backlight {
public:
    ~Backlight() { close(); }
    bool open(std::string& error);   // the first /sys/class/backlight device
    void setDimmed(bool dimmed);
    void close();                    // restores the user's level
private:
    int fd_ = -1, max_ = 0, user_ = 0;
    bool dimmed_ = false;
};

}  // namespace lofi_fb
