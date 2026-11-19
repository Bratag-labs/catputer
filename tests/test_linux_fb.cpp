// Framebuffer-mode helpers (native/linux_fb.h): when the mode is chosen,
// the evdev -> SDL key mapping the preview's controls depend on, panel
// scaling and the dim level.
#include "linux_fb.h"

#include <SDL.h>

#include <cassert>
#include <cstdio>
#include <vector>

#ifdef __linux__
#include <linux/input.h>
#endif

int main() {
    using namespace lofi_fb;
    // Launcher environment: nothing set -> framebuffer.
    assert(autoWanted(nullptr, nullptr, nullptr));
    assert(autoWanted("", "", ""));
    assert(!autoWanted("wayland-0", nullptr, nullptr));
    assert(!autoWanted(nullptr, ":0", nullptr));
    assert(!autoWanted(nullptr, nullptr, "dummy"));   // smoke tests / explicit SDL choice

#ifdef __linux__
    assert(sdlKeyFor(KEY_ESC) == SDLK_ESCAPE);
    assert(sdlKeyFor(KEY_ENTER) == SDLK_RETURN);
    assert(sdlKeyFor(KEY_SPACE) == SDLK_SPACE);
    assert(sdlKeyFor(KEY_BACKSPACE) == SDLK_BACKSPACE);
    assert(sdlKeyFor(KEY_Q) == SDLK_q);
    assert(sdlKeyFor(KEY_M) == SDLK_m);
    assert(sdlKeyFor(KEY_1) == SDLK_1 && sdlKeyFor(KEY_9) == SDLK_9 && sdlKeyFor(KEY_0) == SDLK_0);
    assert(sdlKeyFor(KEY_SEMICOLON) == ';' && sdlKeyFor(KEY_DOT) == '.');
    assert(sdlKeyFor(KEY_COMMA) == ',' && sdlKeyFor(KEY_SLASH) == '/');
    assert(sdlKeyFor(KEY_GRAVE) == '`');
    assert(sdlKeyFor(KEY_MINUS) == SDLK_MINUS && sdlKeyFor(KEY_EQUAL) == SDLK_EQUALS);
    assert(sdlKeyFor(KEY_UP) == SDLK_UP && sdlKeyFor(KEY_RIGHT) == SDLK_RIGHT);
    // The device's volume keys, as SDL reported them there (2026-11-18 capture).
    assert(sdlKeyFor(KEY_VOLUMEUP) == 0x40000080);
    assert(sdlKeyFor(KEY_VOLUMEDOWN) == 0x40000081);
    assert(sdlKeyFor(KEY_KP7) == 0x4000005f);
    assert(sdlKeyFor(KEY_KP9) == 0x40000061);
    assert(sdlKeyFor(KEY_LEFTSHIFT) == 0);   // modifiers alone do nothing
#endif

    // 240x135 onto the 320x170 panel.
    const int sw = 240, sh = 135, dw = 320, dh = 170;
    std::vector<std::uint16_t> src(sw * sh), dst(dw * dh, 0xFFFF);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x) src[y * sw + x] = std::uint16_t(1 + (x == 0 || x == sw - 1 || y == 0 || y == sh - 1));
    compose(src.data(), sw, sh, dst.data(), dw, dh, true);
    // fit: 302x170, 9 px bars left and right.
    assert(dst[0 * dw + 8] == 0 && dst[0 * dw + 9] == 2 && dst[0 * dw + 310] == 2 && dst[0 * dw + 311] == 0);
    assert(dst[169 * dw + 9] == 2 && dst[85 * dw + 160] == 1);
    compose(src.data(), sw, sh, dst.data(), dw, dh, false);
    // 1:1: centred at (40,17).
    assert(dst[17 * dw + 40] == 2 && dst[16 * dw + 40] == 0 && dst[17 * dw + 39] == 0);
    assert(dst[(17 + 134) * dw + 40 + 239] == 2 && dst[(17 + 135) * dw + 40] == 0);
    assert(dst[85 * dw + 160] == 1);
    // A source larger than the panel never writes outside it.
    std::vector<std::uint16_t> big(400 * 300, 1), small(100 * 50, 0);
    compose(big.data(), 400, 300, small.data(), 100, 50, false);
    compose(big.data(), 400, 300, small.data(), 100, 50, true);
    assert(small[0] == 0 && small[50] == 1);

    assert(dimLevelFor(30, 100) == 2);
    assert(dimLevelFor(1, 100) == 1);
    assert(dimLevelFor(30, 10) == 1);
    assert(dimLevelFor(200, 255) == 5);

    std::puts("linux_fb ok");
    return 0;
}
