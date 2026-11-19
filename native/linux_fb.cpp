#include "linux_fb.h"

#include <SDL.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace lofi_fb {

bool autoWanted(const char* wayland, const char* x11, const char* sdlDriver) {
    auto set = [](const char* v) { return v && *v; };
    return !set(wayland) && !set(x11) && !set(sdlDriver);
}

#ifdef __linux__
std::int32_t sdlKeyFor(unsigned code) {
    static const struct { unsigned code; std::int32_t sym; } table[] = {
        {KEY_ESC, SDLK_ESCAPE}, {KEY_ENTER, SDLK_RETURN}, {KEY_KPENTER, SDLK_RETURN},
        {KEY_BACKSPACE, SDLK_BACKSPACE}, {KEY_TAB, SDLK_TAB}, {KEY_SPACE, SDLK_SPACE},
        {KEY_UP, SDLK_UP}, {KEY_DOWN, SDLK_DOWN}, {KEY_LEFT, SDLK_LEFT}, {KEY_RIGHT, SDLK_RIGHT},
        {KEY_MINUS, '-'}, {KEY_EQUAL, '='}, {KEY_SEMICOLON, ';'}, {KEY_APOSTROPHE, '\''},
        {KEY_GRAVE, '`'}, {KEY_BACKSLASH, '\\'}, {KEY_COMMA, ','}, {KEY_DOT, '.'},
        {KEY_SLASH, '/'}, {KEY_LEFTBRACE, '['}, {KEY_RIGHTBRACE, ']'},
        {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'}, {KEY_Y, 'y'},
        {KEY_U, 'u'}, {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'}, {KEY_A, 'a'}, {KEY_S, 's'},
        {KEY_D, 'd'}, {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'},
        {KEY_L, 'l'}, {KEY_Z, 'z'}, {KEY_X, 'x'}, {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'},
        {KEY_N, 'n'}, {KEY_M, 'm'},
        {KEY_VOLUMEUP, SDLK_SCANCODE_MASK | SDL_SCANCODE_VOLUMEUP},
        {KEY_VOLUMEDOWN, SDLK_SCANCODE_MASK | SDL_SCANCODE_VOLUMEDOWN},
        {KEY_KP7, SDLK_SCANCODE_MASK | SDL_SCANCODE_KP_7},   // SYMB + minus
        {KEY_KP9, SDLK_SCANCODE_MASK | SDL_SCANCODE_KP_9},   // SYMB + equals
    };
    if (code >= KEY_1 && code <= KEY_9) return std::int32_t('1' + (code - KEY_1));
    if (code == KEY_0) return '0';
    for (const auto& e : table) if (e.code == code) return e.sym;
    return 0;
}
#else
std::int32_t sdlKeyFor(unsigned) { return 0; }
#endif

void compose(const std::uint16_t* src, int sw, int sh, std::uint16_t* dst, int dw, int dh, bool fit) {
    std::fill(dst, dst + std::size_t(dw) * dh, std::uint16_t(0));
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    int ow = sw, oh = sh;
    if (fit) {
        // Largest size with the source's aspect ratio that fits the panel.
        oh = dh; ow = sw * dh / sh;
        if (ow > dw) { ow = dw; oh = sh * dw / sw; }
    }
    const int ox = (dw - ow) / 2, oy = (dh - oh) / 2;
    for (int y = 0; y < oh; ++y) {
        const int ty = oy + y;
        if (ty < 0 || ty >= dh) continue;
        const std::uint16_t* row = src + std::size_t(y * sh / oh) * sw;
        std::uint16_t* out = dst + std::size_t(ty) * dw;
        for (int x = 0; x < ow; ++x) {
            const int tx = ox + x;
            if (tx >= 0 && tx < dw) out[tx] = row[x * sw / ow];
        }
    }
}

int dimLevelFor(int level, int max) {
    int d = max * 2 / 100;
    if (d < 1) d = 1;
    if (level > 0 && d > level) d = level;
    return d;
}

#ifdef __linux__
// ---- Panel ----

Panel::~Panel() {
    if (map_) munmap(map_, size_);
    if (fd_ >= 0) ::close(fd_);
}

bool Panel::open(const char* path, std::string& error) {
    fd_ = ::open(path, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) { error = std::string(path) + ": " + std::strerror(errno); return false; }
    fb_var_screeninfo var{};
    fb_fix_screeninfo fix{};
    if (ioctl(fd_, FBIOGET_VSCREENINFO, &var) || ioctl(fd_, FBIOGET_FSCREENINFO, &fix)) {
        error = std::string(path) + ": not a framebuffer"; return false;
    }
    if (var.bits_per_pixel != 16 || var.red.offset != 11) {
        error = std::string(path) + ": needs RGB565, found " + std::to_string(var.bits_per_pixel) + " bpp";
        return false;
    }
    width_ = int(var.xres); height_ = int(var.yres); stride_ = int(fix.line_length);
    size_ = fix.smem_len;
    void* m = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (m == MAP_FAILED) { error = std::string(path) + ": mmap: " + std::strerror(errno); return false; }
    map_ = static_cast<std::uint8_t*>(m);
    back_.assign(std::size_t(width_) * height_, 0);
    last_.clear();
    std::fprintf(stderr, "[fb] %s: %dx%d, stride %d\n", path, width_, height_, stride_);
    return true;
}

void Panel::present(const std::uint16_t* src, int sw, int sh, bool fit) {
    if (!map_) return;
    compose(src, sw, sh, back_.data(), width_, height_, fit);
    if (back_ == last_) return;
    for (int y = 0; y < height_; ++y)
        std::memcpy(map_ + std::size_t(y) * stride_, back_.data() + std::size_t(y) * width_, std::size_t(width_) * 2);
    last_ = back_;
}

// ---- Keyboard ----

Keyboard::~Keyboard() {
    if (fd_ >= 0) { ioctl(fd_, EVIOCGRAB, 0); ::close(fd_); }
}

bool Keyboard::open(const char* name, std::string& error) {
    DIR* d = opendir("/dev/input");
    if (!d) { error = std::string("/dev/input: ") + std::strerror(errno); return false; }
    std::string found;
    int lastErrno = 0;
    while (dirent* e = readdir(d)) {
        if (std::strncmp(e->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + e->d_name;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { lastErrno = errno; continue; }
        char n[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof n - 1), n) >= 0 && std::strcmp(n, name) == 0) {
            fd_ = fd; found = path; break;
        }
        ::close(fd);
    }
    closedir(d);
    if (fd_ < 0) {
        error = std::string("no input device named ") + name +
                (lastErrno ? std::string(" (") + std::strerror(lastErrno) + ")" : "");
        return false;
    }
    if (ioctl(fd_, EVIOCGRAB, 1) != 0)
        std::fprintf(stderr, "[fb] %s: grab failed (%s); the launcher also sees keys\n", found.c_str(), std::strerror(errno));
    std::fprintf(stderr, "[fb] keyboard %s (%s)\n", found.c_str(), name);
    return true;
}

void Keyboard::poll(std::vector<KeyEvent>& out) {
    if (fd_ < 0) return;
    input_event ev[32];
    for (;;) {
        const ssize_t n = read(fd_, ev, sizeof ev);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n / ssize_t(sizeof(input_event)); ++i) {
            if (ev[i].type != EV_KEY || ev[i].value > 2) continue;
            const std::int32_t sym = sdlKeyFor(ev[i].code);
            if (sym) out.push_back({sym, ev[i].value != 0, ev[i].value == 2});
        }
    }
}

// ---- Backlight ----

namespace {
int readInt(int fd) {
    char b[16] = {0};
    const ssize_t n = pread(fd, b, sizeof b - 1, 0);
    return n > 0 ? std::atoi(b) : -1;
}
}  // namespace

bool Backlight::open(std::string& error) {
    std::string dir;
    if (DIR* d = opendir("/sys/class/backlight")) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') { dir = std::string("/sys/class/backlight/") + e->d_name; break; }
        closedir(d);
    }
    if (dir.empty()) { error = "no /sys/class/backlight device"; return false; }
    const int mfd = ::open((dir + "/max_brightness").c_str(), O_RDONLY | O_CLOEXEC);
    max_ = mfd >= 0 ? readInt(mfd) : -1;
    if (mfd >= 0) ::close(mfd);
    fd_ = ::open((dir + "/brightness").c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0 || max_ <= 0) {
        error = dir + "/brightness: " + (fd_ < 0 ? std::strerror(errno) : "no max_brightness");
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        return false;
    }
    user_ = readInt(fd_);
    std::fprintf(stderr, "[fb] backlight %s: level %d of %d\n", dir.c_str(), user_, max_);
    return true;
}

void Backlight::setDimmed(bool dimmed) {
    if (fd_ < 0 || dimmed == dimmed_) return;
    auto write = [&](int level) {
        char b[16];
        const int n = std::snprintf(b, sizeof b, "%d\n", level);
        return pwrite(fd_, b, std::size_t(n), 0) == n;
    };
    if (dimmed) {
        // Whatever is set now is the user's level (they may have changed it
        // in the launcher since we started); a panel already off stays off.
        const int cur = readInt(fd_);
        if (cur > 0) user_ = cur;
        if (cur == 0 || user_ <= 0) return;
        if (write(dimLevelFor(user_, max_))) dimmed_ = true;
    } else {
        if (user_ > 0) write(user_);
        dimmed_ = false;
    }
}

void Backlight::close() {
    if (fd_ < 0) return;
    setDimmed(false);
    ::close(fd_);
    fd_ = -1;
}

#else
Panel::~Panel() {}
bool Panel::open(const char*, std::string& e) { e = "framebuffer mode is Linux-only"; return false; }
void Panel::present(const std::uint16_t*, int, int, bool) {}
Keyboard::~Keyboard() {}
bool Keyboard::open(const char*, std::string& e) { e = "Linux-only"; return false; }
void Keyboard::poll(std::vector<KeyEvent>&) {}
bool Backlight::open(std::string& e) { e = "Linux-only"; return false; }
void Backlight::setDimmed(bool) {}
void Backlight::close() {}
#endif

}  // namespace lofi_fb
