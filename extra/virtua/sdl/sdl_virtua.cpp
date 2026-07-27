#include "SDL.h"

#include "media.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef VIRTUA_DISPLAY_WIDTH
#define VIRTUA_DISPLAY_WIDTH 960
#endif
#ifndef VIRTUA_DISPLAY_HEIGHT
#define VIRTUA_DISPLAY_HEIGHT 720
#endif

struct SDL_Window {
    int x;
    int y;
    int width;
    int height;
    Uint32 flags;
    int visible;
};

struct SDL_Texture {
    int width;
    int height;
    int pitch;
    int bytes_per_pixel;
    Uint32 format;
    SDL_ScaleMode scale_mode;
    Uint8 *pixels;
};

struct SDL_Renderer {
    SDL_Window *window;
    Uint8 clear_r;
    Uint8 clear_g;
    Uint8 clear_b;
    Uint8 clear_a;
    int logical_width;
    int logical_height;
    int vsync;
    int framebuffer_fd;
    Uint8 *framebuffer;
    Uint16 *fallback_rgb565;
};

struct SDL_mutex { pthread_mutex_t value; };
struct SDL_cond { pthread_cond_t value; };
struct SDL_Thread {
    pthread_t handle;
    SDL_ThreadFunction function;
    void *data;
    int status;
};

namespace {

static Uint32 initialized_flags;
static char last_error[192];
static SDL_Window *current_gl_window;
static SDL_GLContext current_gl_context;
static int input_fd = -1;
static Uint8 keyboard_state[SDL_NUM_SCANCODES];
static Uint16 keyboard_modifiers;
static SDL_Event event_queue[64];
static unsigned event_head;
static unsigned event_tail;
static unsigned event_count;

struct AudioState {
    int fd;
    SDL_AudioSpec format;
    SDL_mutex *mutex;
    SDL_Thread *thread;
    volatile int running;
    volatile int paused;
};
static AudioState audio_state = {-1, {}, nullptr, nullptr, 0, 1};

static void set_error(const char *message)
{
    size_t i = 0;
    if (!message) message = "Virtua SDL error";
    while (message[i] && i + 1 < sizeof(last_error)) {
        last_error[i] = message[i];
        ++i;
    }
    last_error[i] = '\0';
}

static int ascii_lower(int value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int text_equal(const char *left, const char *right)
{
    if (!left || !right) return 0;
    while (*left && *right) {
        if (ascii_lower((unsigned char)*left) != ascii_lower((unsigned char)*right))
            return 0;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static Uint64 monotonic_microseconds()
{
    struct timeval value{};
    if (gettimeofday(&value, nullptr) != 0) return 0;
    return (Uint64)value.tv_sec * 1000000ull + (Uint64)value.tv_usec;
}

static void cooperative_yield()
{
    (void)sched_yield();
}

static void queue_event(const SDL_Event &event)
{
    if (event_count == sizeof(event_queue) / sizeof(event_queue[0])) {
        event_head = (event_head + 1u) % (sizeof(event_queue) / sizeof(event_queue[0]));
        --event_count;
    }
    event_queue[event_tail] = event;
    event_tail = (event_tail + 1u) % (sizeof(event_queue) / sizeof(event_queue[0]));
    ++event_count;
}

static SDL_Scancode linux_key_to_scancode(Uint16 code)
{
    switch (code) {
    case 1: return SDL_SCANCODE_ESCAPE;
    case 14: return SDL_SCANCODE_BACKSPACE;
    case 15: return SDL_SCANCODE_TAB;
    case 16: return SDL_SCANCODE_Q;
    case 17: return SDL_SCANCODE_W;
    case 18: return SDL_SCANCODE_E;
    case 19: return SDL_SCANCODE_R;
    case 20: return SDL_SCANCODE_T;
    case 21: return SDL_SCANCODE_Y;
    case 22: return SDL_SCANCODE_U;
    case 23: return SDL_SCANCODE_I;
    case 24: return SDL_SCANCODE_O;
    case 25: return SDL_SCANCODE_P;
    case 28: return SDL_SCANCODE_RETURN;
    case 29: return SDL_SCANCODE_LCTRL;
    case 30: return SDL_SCANCODE_A;
    case 31: return SDL_SCANCODE_S;
    case 32: return SDL_SCANCODE_D;
    case 33: return SDL_SCANCODE_F;
    case 34: return SDL_SCANCODE_G;
    case 35: return SDL_SCANCODE_H;
    case 36: return SDL_SCANCODE_J;
    case 37: return SDL_SCANCODE_K;
    case 38: return SDL_SCANCODE_L;
    case 42: return SDL_SCANCODE_LSHIFT;
    case 43: return SDL_SCANCODE_BACKSLASH;
    case 44: return SDL_SCANCODE_Z;
    case 45: return SDL_SCANCODE_X;
    case 46: return SDL_SCANCODE_C;
    case 47: return SDL_SCANCODE_V;
    case 48: return SDL_SCANCODE_B;
    case 49: return SDL_SCANCODE_N;
    case 50: return SDL_SCANCODE_M;
    case 54: return SDL_SCANCODE_RSHIFT;
    case 56: return SDL_SCANCODE_LALT;
    case 57: return SDL_SCANCODE_SPACE;
    case 59: return SDL_SCANCODE_F1;
    case 60: return SDL_SCANCODE_F2;
    case 61: return SDL_SCANCODE_F3;
    case 62: return SDL_SCANCODE_F4;
    case 63: return SDL_SCANCODE_F5;
    case 64: return SDL_SCANCODE_F6;
    case 65: return SDL_SCANCODE_F7;
    case 66: return SDL_SCANCODE_F8;
    case 67: return SDL_SCANCODE_F9;
    case 68: return SDL_SCANCODE_F10;
    case 87: return SDL_SCANCODE_F11;
    case 88: return SDL_SCANCODE_F12;
    case 97: return SDL_SCANCODE_RCTRL;
    case 100: return SDL_SCANCODE_RALT;
    case 103: return SDL_SCANCODE_UP;
    case 105: return SDL_SCANCODE_LEFT;
    case 106: return SDL_SCANCODE_RIGHT;
    case 108: return SDL_SCANCODE_DOWN;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

static SDL_Keycode scancode_to_keycode(SDL_Scancode scancode)
{
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return 'a' + (scancode - SDL_SCANCODE_A);
    if (scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER)
        return SDLK_RETURN;
    if (scancode == SDL_SCANCODE_ESCAPE) return SDLK_ESCAPE;
    if (scancode == SDL_SCANCODE_BACKSPACE) return SDLK_BACKSPACE;
    if (scancode == SDL_SCANCODE_SPACE) return SDLK_SPACE;
    if (scancode == SDL_SCANCODE_RIGHT) return SDLK_RIGHT;
    if (scancode == SDL_SCANCODE_LEFT) return SDLK_LEFT;
    if (scancode == SDL_SCANCODE_DOWN) return SDLK_DOWN;
    if (scancode == SDL_SCANCODE_UP) return SDLK_UP;
    if (scancode >= SDL_SCANCODE_F1 && scancode <= SDL_SCANCODE_F12)
        return SDLK_F1 + (scancode - SDL_SCANCODE_F1);
    return SDLK_UNKNOWN;
}

static Uint16 modifier_for_scancode(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: return KMOD_LSHIFT;
    case SDL_SCANCODE_RSHIFT: return KMOD_RSHIFT;
    case SDL_SCANCODE_LCTRL: return KMOD_LCTRL;
    case SDL_SCANCODE_RCTRL: return KMOD_RCTRL;
    case SDL_SCANCODE_LALT: return KMOD_LALT;
    case SDL_SCANCODE_RALT: return KMOD_RALT;
    default: return KMOD_NONE;
    }
}

static void pump_input()
{
    if (input_fd < 0) input_fd = open("/dev/input0", O_RDONLY | O_NONBLOCK);
    if (input_fd < 0) return;

    TouchEvent input{};
    for (int count = 0; count < 64; ++count) {
        const ssize_t bytes = read(input_fd, &input, sizeof(input));
        if (bytes != (ssize_t)sizeof(input)) break;
        if (input.type != KEY_DOWN && input.type != KEY_UP) continue;
        const SDL_Scancode scancode = linux_key_to_scancode(input.x);
        if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_NUM_SCANCODES) continue;
        const int pressed = input.type == KEY_DOWN;
        keyboard_state[scancode] = pressed ? 1 : 0;
        const Uint16 modifier = modifier_for_scancode(scancode);
        if (pressed)
            keyboard_modifiers |= modifier;
        else
            keyboard_modifiers &= (Uint16)~modifier;

        SDL_Event event{};
        event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
        event.key.type = event.type;
        event.key.timestamp = SDL_GetTicks();
        event.key.state = pressed ? 1 : 0;
        event.key.repeat = pressed && input.y > 1 ? 1 : 0;
        event.key.keysym.scancode = scancode;
        event.key.keysym.sym = scancode_to_keycode(scancode);
        event.key.keysym.mod = keyboard_modifiers;
        queue_event(event);
    }
}

static void fill_renderer(SDL_Renderer *renderer, Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha)
{
    if (!renderer || !renderer->framebuffer || !renderer->window) return;
    const size_t pixels = (size_t)renderer->window->width * (size_t)renderer->window->height;
    for (size_t i = 0; i < pixels; ++i) {
        renderer->framebuffer[i * 4u + 0u] = red;
        renderer->framebuffer[i * 4u + 1u] = green;
        renderer->framebuffer[i * 4u + 2u] = blue;
        renderer->framebuffer[i * 4u + 3u] = alpha;
    }
}

static void argb_to_rgba(Uint32 pixel, Uint8 *out)
{
    out[0] = (Uint8)((pixel >> 16u) & 0xffu);
    out[1] = (Uint8)((pixel >> 8u) & 0xffu);
    out[2] = (Uint8)(pixel & 0xffu);
    out[3] = (Uint8)((pixel >> 24u) & 0xffu);
}

static void *thread_trampoline(void *opaque)
{
    SDL_Thread *thread = (SDL_Thread *)opaque;
    thread->status = thread->function ? thread->function(thread->data) : -1;
    return nullptr;
}

static void texture_pixel_rgba(const SDL_Texture *texture, int x, int y, Uint8 out[4])
{
    const Uint8 *pixel = texture->pixels + (size_t)y * texture->pitch +
                         (size_t)x * texture->bytes_per_pixel;
    if (texture->format == SDL_PIXELFORMAT_RGB24) {
        out[0] = pixel[0]; out[1] = pixel[1]; out[2] = pixel[2]; out[3] = 255;
    } else if (texture->format == SDL_PIXELFORMAT_RGB565) {
        const Uint16 value = (Uint16)(pixel[0] | ((Uint16)pixel[1] << 8u));
        out[0] = (Uint8)(((value >> 11u) & 31u) * 255u / 31u);
        out[1] = (Uint8)(((value >> 5u) & 63u) * 255u / 63u);
        out[2] = (Uint8)((value & 31u) * 255u / 31u);
        out[3] = 255;
    } else if (texture->format == SDL_PIXELFORMAT_ARGB1555) {
        const Uint16 value = (Uint16)(pixel[0] | ((Uint16)pixel[1] << 8u));
        out[0] = (Uint8)(((value >> 10u) & 31u) * 255u / 31u);
        out[1] = (Uint8)(((value >> 5u) & 31u) * 255u / 31u);
        out[2] = (Uint8)((value & 31u) * 255u / 31u);
        out[3] = (value & 0x8000u) ? 255 : 0;
    } else {
        Uint32 value = 0;
        memcpy(&value, pixel, sizeof(value));
        argb_to_rgba(value, out);
    }
}

static int audio_callback_thread(void *)
{
    const Uint32 bytes = audio_state.format.size ? audio_state.format.size : 4096u;
    Uint8 *buffer = (Uint8 *)malloc(bytes);
    if (!buffer) return -1;
    while (__atomic_load_n(&audio_state.running, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&audio_state.paused, __ATOMIC_ACQUIRE)) {
            SDL_Delay(1);
            continue;
        }
        memset(buffer, 0, bytes);
        audio_state.format.callback(audio_state.format.userdata, buffer, (int)bytes);
        Uint32 written = 0;
        while (written < bytes && __atomic_load_n(&audio_state.running, __ATOMIC_ACQUIRE)) {
            const ssize_t result = write(audio_state.fd, buffer + written, bytes - written);
            if (result > 0) written += (Uint32)result;
            else if (result < 0 && (errno == EINTR || errno == EAGAIN)) cooperative_yield();
            else break;
        }
    }
    free(buffer);
    return 0;
}

static const char *scancode_name(SDL_Scancode scancode)
{
    static const char *const letters[] = {
        "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
        "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"
    };
    if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return letters[scancode - SDL_SCANCODE_A];
    switch (scancode) {
    case SDL_SCANCODE_RETURN: return "Return";
    case SDL_SCANCODE_ESCAPE: return "Escape";
    case SDL_SCANCODE_BACKSPACE: return "Backspace";
    case SDL_SCANCODE_TAB: return "Tab";
    case SDL_SCANCODE_SPACE: return "Space";
    case SDL_SCANCODE_BACKSLASH: return "Backslash";
    case SDL_SCANCODE_F1: return "F1";
    case SDL_SCANCODE_F2: return "F2";
    case SDL_SCANCODE_F3: return "F3";
    case SDL_SCANCODE_F4: return "F4";
    case SDL_SCANCODE_F5: return "F5";
    case SDL_SCANCODE_F6: return "F6";
    case SDL_SCANCODE_F7: return "F7";
    case SDL_SCANCODE_F8: return "F8";
    case SDL_SCANCODE_F9: return "F9";
    case SDL_SCANCODE_F10: return "F10";
    case SDL_SCANCODE_F11: return "F11";
    case SDL_SCANCODE_F12: return "F12";
    case SDL_SCANCODE_RIGHT: return "Right";
    case SDL_SCANCODE_LEFT: return "Left";
    case SDL_SCANCODE_DOWN: return "Down";
    case SDL_SCANCODE_UP: return "Up";
    case SDL_SCANCODE_KP_ENTER: return "Keypad Enter";
    case SDL_SCANCODE_LCTRL: return "Left Ctrl";
    case SDL_SCANCODE_LSHIFT: return "Left Shift";
    case SDL_SCANCODE_LALT: return "Left Alt";
    case SDL_SCANCODE_RCTRL: return "Right Ctrl";
    case SDL_SCANCODE_RSHIFT: return "Right Shift";
    case SDL_SCANCODE_RALT: return "Right Alt";
    default: return "";
    }
}

} // namespace

extern "C" {

int SDL_Init(Uint32 flags)
{
    initialized_flags |= flags;
    if ((flags & (SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)) != 0)
        pump_input();
    return 0;
}

int SDL_InitSubSystem(Uint32 flags) { return SDL_Init(flags); }
Uint32 SDL_WasInit(Uint32 flags) { return flags ? initialized_flags & flags : initialized_flags; }

void SDL_Quit(void)
{
    if (input_fd >= 0) { close(input_fd); input_fd = -1; }
    if (audio_state.thread) {
        __atomic_store_n(&audio_state.running, 0, __ATOMIC_RELEASE);
        SDL_WaitThread(audio_state.thread, nullptr);
        audio_state.thread = nullptr;
    }
    if (audio_state.fd >= 0) { close(audio_state.fd); audio_state.fd = -1; }
    if (audio_state.mutex) { SDL_DestroyMutex(audio_state.mutex); audio_state.mutex = nullptr; }
    initialized_flags = 0;
}

const char *SDL_GetError(void) { return last_error[0] ? last_error : ""; }
SDL_bool SDL_SetHint(const char *, const char *) { return SDL_TRUE; }
SDL_bool SDL_SetHintWithPriority(const char *name, const char *value, SDL_HintPriority) { return SDL_SetHint(name, value); }
void SDL_SetMainReady(void) {}
Uint64 SDL_GetPerformanceCounter(void) { return monotonic_microseconds(); }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000000ull; }
Uint32 SDL_GetTicks(void) { return (Uint32)(monotonic_microseconds() / 1000ull); }

void SDL_Delay(Uint32 milliseconds)
{
    struct timespec delay{};
    delay.tv_sec = milliseconds / 1000u;
    delay.tv_nsec = (long)(milliseconds % 1000u) * 1000000l;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

SDL_Window *SDL_CreateWindow(const char *, int x, int y, int w, int h, Uint32 flags)
{
    if (w <= 0 || h <= 0) { set_error("invalid Virtua window dimensions"); return nullptr; }
    SDL_Window *window = (SDL_Window *)calloc(1, sizeof(*window));
    if (!window) { set_error("out of memory creating Virtua window"); return nullptr; }
    window->x = x;
    window->y = y;
    window->width = w;
    window->height = h;
    window->flags = flags | SDL_WINDOW_SHOWN;
    window->visible = 1;
    return window;
}

void SDL_DestroyWindow(SDL_Window *window) { free(window); }
void SDL_ShowWindow(SDL_Window *window) { if (window) { window->visible = 1; window->flags |= SDL_WINDOW_SHOWN; } }
void SDL_HideWindow(SDL_Window *window) { if (window) { window->visible = 0; window->flags &= ~SDL_WINDOW_SHOWN; } }
void SDL_RaiseWindow(SDL_Window *) {}
Uint32 SDL_GetWindowFlags(SDL_Window *window) { return window ? window->flags : 0; }
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h) { if (w) *w = window ? window->width : 0; if (h) *h = window ? window->height : 0; }
void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y) { if (x) *x = window ? window->x : 0; if (y) *y = window ? window->y : 0; }
void SDL_SetWindowPosition(SDL_Window *window, int x, int y) { if (window) { window->x = x; window->y = y; } }
void SDL_SetWindowSize(SDL_Window *window, int w, int h) { if (window && w > 0 && h > 0) { window->width = w; window->height = h; } }
void SDL_SetWindowTitle(SDL_Window *, const char *) {}
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags) { if (!window) return -1; window->flags &= ~SDL_WINDOW_FULLSCREEN_DESKTOP; window->flags |= flags & SDL_WINDOW_FULLSCREEN_DESKTOP; return 0; }
int SDL_GetWindowDisplayIndex(SDL_Window *window) { return window ? 0 : -1; }
int SDL_GetCurrentDisplayMode(int display_index, SDL_DisplayMode *mode) { if (display_index != 0 || !mode) return -1; mode->format = SDL_PIXELFORMAT_ARGB8888; mode->w = VIRTUA_DISPLAY_WIDTH; mode->h = VIRTUA_DISPLAY_HEIGHT; mode->refresh_rate = 60; mode->driverdata = nullptr; return 0; }
int SDL_GetDisplayUsableBounds(int display_index, SDL_Rect *rect) { if (display_index != 0 || !rect) return -1; rect->x = 0; rect->y = 0; rect->w = VIRTUA_DISPLAY_WIDTH; rect->h = VIRTUA_DISPLAY_HEIGHT; return 0; }
const char *SDL_GetCurrentVideoDriver(void) { return "virtua"; }

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int, Uint32 flags)
{
    if (!window) { set_error("Virtua renderer requires a window"); return nullptr; }
    SDL_Renderer *renderer = (SDL_Renderer *)calloc(1, sizeof(*renderer));
    if (!renderer) { set_error("out of memory creating Virtua renderer"); return nullptr; }
    const size_t pixels = (size_t)window->width * (size_t)window->height;
    renderer->framebuffer = (Uint8 *)malloc(pixels * 4u);
    renderer->fallback_rgb565 = (Uint16 *)malloc(pixels * sizeof(Uint16));
    if (!renderer->framebuffer || !renderer->fallback_rgb565) {
        free(renderer->framebuffer); free(renderer->fallback_rgb565); free(renderer);
        set_error("out of memory allocating Virtua framebuffer");
        return nullptr;
    }
    renderer->window = window;
    renderer->clear_a = 255;
    renderer->vsync = (flags & SDL_RENDERER_PRESENTVSYNC) != 0;
    renderer->framebuffer_fd = -1;
    fill_renderer(renderer, 0, 0, 0, 255);
    return renderer;
}

void SDL_DestroyRenderer(SDL_Renderer *renderer)
{
    if (!renderer) return;
    if (renderer->framebuffer_fd >= 0) close(renderer->framebuffer_fd);
    free(renderer->framebuffer);
    free(renderer->fallback_rgb565);
    free(renderer);
}

int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h) { if (!renderer || w <= 0 || h <= 0) return -1; renderer->logical_width = w; renderer->logical_height = h; return 0; }
int SDL_RenderSetVSync(SDL_Renderer *renderer, int vsync) { if (!renderer) return -1; renderer->vsync = vsync != 0; return 0; }
int SDL_GetRendererInfo(SDL_Renderer *renderer, SDL_RendererInfo *info) { if (!renderer || !info) return -1; memset(info, 0, sizeof(*info)); info->name = "virtua"; info->flags = SDL_RENDERER_SOFTWARE | (renderer->vsync ? SDL_RENDERER_PRESENTVSYNC : 0u); info->num_texture_formats = 4; info->texture_formats[0] = SDL_PIXELFORMAT_RGB24; info->texture_formats[1] = SDL_PIXELFORMAT_ARGB8888; info->texture_formats[2] = SDL_PIXELFORMAT_RGB565; info->texture_formats[3] = SDL_PIXELFORMAT_ARGB1555; info->max_texture_width = 4096; info->max_texture_height = 4096; return 0; }
int SDL_GetRendererOutputSize(SDL_Renderer *renderer, int *w, int *h) { if (!renderer || !renderer->window) return -1; if (w) *w = renderer->window->width; if (h) *h = renderer->window->height; return 0; }
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a) { if (!renderer) return -1; renderer->clear_r = r; renderer->clear_g = g; renderer->clear_b = b; renderer->clear_a = a; return 0; }
int SDL_RenderClear(SDL_Renderer *renderer) { if (!renderer) return -1; fill_renderer(renderer, renderer->clear_r, renderer->clear_g, renderer->clear_b, renderer->clear_a); return 0; }

int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *source, const SDL_Rect *destination)
{
    if (!renderer || !renderer->window || !renderer->framebuffer || !texture || !texture->pixels)
        return -1;
    SDL_Rect src{0, 0, texture->width, texture->height};
    SDL_Rect dst{0, 0, renderer->window->width, renderer->window->height};
    if (source) src = *source;
    if (destination) dst = *destination;
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0) return -1;
    for (int y = 0; y < dst.h; ++y) {
        const int dy = dst.y + y;
        if (dy < 0 || dy >= renderer->window->height) continue;
        const int sy = src.y + (int)((int64_t)y * src.h / dst.h);
        if (sy < 0 || sy >= texture->height) continue;
        for (int x = 0; x < dst.w; ++x) {
            const int dx = dst.x + x;
            if (dx < 0 || dx >= renderer->window->width) continue;
            const int sx = src.x + (int)((int64_t)x * src.w / dst.w);
            if (sx < 0 || sx >= texture->width) continue;
            Uint8 rgba[4];
            texture_pixel_rgba(texture, sx, sy, rgba);
            memcpy(renderer->framebuffer +
                   ((size_t)dy * renderer->window->width + dx) * 4u, rgba, 4u);
        }
    }
    return 0;
}

void SDL_RenderPresent(SDL_Renderer *renderer)
{
    if (!renderer || !renderer->window || !renderer->framebuffer) return;
    if (renderer->framebuffer_fd < 0)
        renderer->framebuffer_fd = open("/dev/fb0", O_RDWR);
    if (renderer->framebuffer_fd < 0) { set_error("/dev/fb0 is unavailable"); return; }
    fb_draw_rgba8 draw{};
    draw.x = 0; draw.y = 0;
    draw.w = (Uint16)renderer->window->width;
    draw.h = (Uint16)renderer->window->height;
    draw.data = renderer->framebuffer;
    if (ioctl(renderer->framebuffer_fd, FB_IOCTL_SWAP_RGBA8, &draw) == 0) return;

    const size_t pixels = (size_t)renderer->window->width * renderer->window->height;
    for (size_t i = 0; i < pixels; ++i) {
        const Uint8 r = renderer->framebuffer[i * 4u + 0u];
        const Uint8 g = renderer->framebuffer[i * 4u + 1u];
        const Uint8 b = renderer->framebuffer[i * 4u + 2u];
        renderer->fallback_rgb565[i] = (Uint16)(((r >> 3u) << 11u) |
                                                ((g >> 2u) << 5u) |
                                                (b >> 3u));
    }
    fb_draw fallback{};
    fallback.x = 0; fallback.y = 0;
    fallback.w = (Uint16)renderer->window->width;
    fallback.h = (Uint16)renderer->window->height;
    fallback.data = renderer->fallback_rgb565;
    if (ioctl(renderer->framebuffer_fd, FB_IOCTL_SWAP_BUFFER, &fallback) != 0)
        set_error("Virtua framebuffer present failed");
}

SDL_Texture *SDL_CreateTexture(SDL_Renderer *, Uint32 format, int access, int w, int h)
{
    if (access != SDL_TEXTUREACCESS_STREAMING || w <= 0 || h <= 0) {
        set_error("unsupported Virtua texture access");
        return nullptr;
    }
    int bytes_per_pixel = 0;
    if (format == SDL_PIXELFORMAT_RGB24) bytes_per_pixel = 3;
    else if (format == SDL_PIXELFORMAT_ARGB8888) bytes_per_pixel = 4;
    else if (format == SDL_PIXELFORMAT_RGB565 || format == SDL_PIXELFORMAT_ARGB1555) bytes_per_pixel = 2;
    else { set_error("unsupported Virtua texture format"); return nullptr; }
    SDL_Texture *texture = (SDL_Texture *)calloc(1, sizeof(*texture));
    if (!texture) return nullptr;
    texture->width = w; texture->height = h;
    texture->bytes_per_pixel = bytes_per_pixel;
    texture->pitch = w * bytes_per_pixel;
    texture->format = format;
    texture->scale_mode = SDL_ScaleModeNearest;
    texture->pixels = (Uint8 *)calloc((size_t)texture->pitch * h, 1u);
    if (!texture->pixels) { free(texture); return nullptr; }
    return texture;
}

void SDL_DestroyTexture(SDL_Texture *texture) { if (texture) { free(texture->pixels); free(texture); } }

int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    if (!texture || !pixels || pitch <= 0) return -1;
    SDL_Rect area{0, 0, texture->width, texture->height};
    if (rect) area = *rect;
    if (area.x < 0 || area.y < 0 || area.w < 0 || area.h < 0 ||
        area.x + area.w > texture->width || area.y + area.h > texture->height) return -1;
    const Uint8 *source = (const Uint8 *)pixels;
    for (int row = 0; row < area.h; ++row) {
        memcpy(texture->pixels + (size_t)(area.y + row) * texture->pitch +
                         (size_t)area.x * texture->bytes_per_pixel,
               source + (size_t)row * pitch,
               (size_t)area.w * texture->bytes_per_pixel);
    }
    return 0;
}

int SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode scale_mode) { if (!texture) return -1; texture->scale_mode = scale_mode; return 0; }

int SDL_GL_SetAttribute(int, int) { return 0; }
void SDL_GL_ResetAttributes(void) {}
SDL_GLContext SDL_GL_CreateContext(SDL_Window *) { set_error("OpenGL is unavailable on Virtua; software rendering is required"); return nullptr; }
void SDL_GL_DeleteContext(SDL_GLContext) {}
int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context) { current_gl_window = window; current_gl_context = context; return context ? 0 : -1; }
SDL_GLContext SDL_GL_GetCurrentContext(void) { return current_gl_context; }
SDL_Window *SDL_GL_GetCurrentWindow(void) { return current_gl_window; }
int SDL_GL_SetSwapInterval(int) { return -1; }
void SDL_GL_SwapWindow(SDL_Window *) {}
void *SDL_GL_GetProcAddress(const char *) { return nullptr; }
int SDL_Vulkan_LoadLibrary(const char *) { set_error("Vulkan is unavailable on Virtua"); return -1; }

int SDL_PollEvent(SDL_Event *event)
{
    SDL_PumpEvents();
    if (!event || event_count == 0) return 0;
    *event = event_queue[event_head];
    event_head = (event_head + 1u) % (sizeof(event_queue) / sizeof(event_queue[0]));
    --event_count;
    return 1;
}

void SDL_PumpEvents(void) { pump_input(); }

const Uint8 *SDL_GetKeyboardState(int *count) { if (count) *count = SDL_NUM_SCANCODES; SDL_PumpEvents(); return keyboard_state; }
Uint16 SDL_GetModState(void) { return keyboard_modifiers; }
SDL_Keycode SDL_GetKeyFromName(const char *name) { return scancode_to_keycode(SDL_GetScancodeFromName(name)); }
int SDL_strcasecmp(const char *left, const char *right) { if (!left || !right) return left ? 1 : right ? -1 : 0; while (*left && *right) { int a = ascii_lower((unsigned char)*left++), b = ascii_lower((unsigned char)*right++); if (a != b) return a - b; } return (unsigned char)*left - (unsigned char)*right; }

SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key)
{
    if (key >= 'a' && key <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + key - 'a');
    if (key >= 'A' && key <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + key - 'A');
    if (key == SDLK_RETURN) return SDL_SCANCODE_RETURN;
    if (key == SDLK_ESCAPE) return SDL_SCANCODE_ESCAPE;
    if (key >= SDLK_F1 && key <= SDLK_F12)
        return (SDL_Scancode)(SDL_SCANCODE_F1 + key - SDLK_F1);
    return SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode SDL_GetScancodeFromName(const char *name)
{
    if (!name || !name[0] || text_equal(name, "None")) return SDL_SCANCODE_UNKNOWN;
    for (int index = 1; index < SDL_NUM_SCANCODES; ++index)
        if (scancode_name((SDL_Scancode)index)[0] &&
            text_equal(name, scancode_name((SDL_Scancode)index)))
            return (SDL_Scancode)index;
    if (text_equal(name, "Enter")) return SDL_SCANCODE_RETURN;
    if (text_equal(name, "LShift")) return SDL_SCANCODE_LSHIFT;
    if (text_equal(name, "RShift")) return SDL_SCANCODE_RSHIFT;
    if (text_equal(name, "LCtrl")) return SDL_SCANCODE_LCTRL;
    if (text_equal(name, "RCtrl")) return SDL_SCANCODE_RCTRL;
    if (text_equal(name, "LAlt")) return SDL_SCANCODE_LALT;
    if (text_equal(name, "RAlt")) return SDL_SCANCODE_RALT;
    return SDL_SCANCODE_UNKNOWN;
}

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_NUM_SCANCODES)
        return scancode_name(scancode);
    return "";
}

int SDL_NumJoysticks(void) { return 0; }
SDL_bool SDL_IsGameController(int) { return SDL_FALSE; }
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int) { return -1; }
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int) { SDL_JoystickGUID guid{}; return guid; }
void SDL_JoystickGetGUIDString(SDL_JoystickGUID, char *text, int text_size) { if (text && text_size > 0) text[0] = '\0'; }
const char *SDL_GameControllerNameForIndex(int) { return nullptr; }
const char *SDL_GameControllerPathForIndex(int) { return nullptr; }
const char *SDL_GameControllerGetSerial(SDL_GameController *) { return nullptr; }
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick) { return joystick ? joystick->instance : -1; }
SDL_GameController *SDL_GameControllerOpen(int) { set_error("Virtua gamepad device is not exposed as an SDL controller"); return nullptr; }
void SDL_GameControllerClose(SDL_GameController *controller) { free(controller); }
SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID) { return nullptr; }
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *controller) { return controller ? &controller->joystick : nullptr; }
SDL_bool SDL_GameControllerGetAttached(SDL_GameController *controller) { return controller && controller->attached ? SDL_TRUE : SDL_FALSE; }
Sint16 SDL_GameControllerGetAxis(SDL_GameController *, SDL_GameControllerAxis) { return 0; }
Uint8 SDL_GameControllerGetButton(SDL_GameController *, SDL_GameControllerButton) { return 0; }

SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *text)
{
    if (text_equal(text, "leftx")) return SDL_CONTROLLER_AXIS_LEFTX;
    if (text_equal(text, "lefty")) return SDL_CONTROLLER_AXIS_LEFTY;
    if (text_equal(text, "rightx")) return SDL_CONTROLLER_AXIS_RIGHTX;
    if (text_equal(text, "righty")) return SDL_CONTROLLER_AXIS_RIGHTY;
    if (text_equal(text, "lefttrigger")) return SDL_CONTROLLER_AXIS_TRIGGERLEFT;
    if (text_equal(text, "righttrigger")) return SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
    return SDL_CONTROLLER_AXIS_INVALID;
}

SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *text)
{
    static const char *const names[] = {"a", "b", "x", "y", "back", "guide", "start",
        "leftstick", "rightstick", "leftshoulder", "rightshoulder",
        "dpup", "dpdown", "dpleft", "dpright"};
    for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index)
        if (text_equal(text, names[index])) return (SDL_GameControllerButton)index;
    return SDL_CONTROLLER_BUTTON_INVALID;
}

void SDL_GameControllerUpdate(void) { SDL_PumpEvents(); }
int SDL_GameControllerEventState(int state) { return state; }
const char *SDL_GameControllerName(SDL_GameController *) { return nullptr; }
int SDL_GameControllerAddMappingsFromFile(const char *) { return 0; }

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int)
{
    if (iscapture || !desired || desired->format != AUDIO_S16SYS ||
        desired->channels == 0) {
        set_error("Virtua audio supports signed-16 PCM playback");
        return 0;
    }
    if (audio_state.fd >= 0) return 1;
    audio_state.fd = open("/dev/dac0", O_WRONLY);
    if (audio_state.fd < 0) { set_error("/dev/dac0 is unavailable"); return 0; }
    pcm_config config{};
    config.sample_rate = desired->freq > 0 ? (Uint32)desired->freq : 44100u;
    config.channels = desired->channels;
    config.bits_per_sample = 16;
    if (ioctl(audio_state.fd, AUDIO_IOCTL_SET_CONFIG, &config) != 0) {
        close(audio_state.fd); audio_state.fd = -1;
        set_error("Virtua audio format negotiation failed");
        return 0;
    }
    audio_state.format = *desired;
    audio_state.format.freq = (int)config.sample_rate;
    audio_state.format.size = (Uint32)audio_state.format.samples * audio_state.format.channels * 2u;
    if (obtained) *obtained = audio_state.format;
    if (!audio_state.mutex) audio_state.mutex = SDL_CreateMutex();
    audio_state.paused = 1;
    if (audio_state.format.callback) {
        audio_state.running = 1;
        audio_state.thread = SDL_CreateThread(audio_callback_thread, "virtua-audio", nullptr);
        if (!audio_state.thread) {
            audio_state.running = 0;
            close(audio_state.fd); audio_state.fd = -1;
            set_error("Virtua audio callback thread could not be created");
            return 0;
        }
    }
    initialized_flags |= SDL_INIT_AUDIO;
    return 1;
}

void SDL_CloseAudioDevice(SDL_AudioDeviceID device)
{
    if (device != 1 || audio_state.fd < 0) return;
    if (audio_state.thread) {
        __atomic_store_n(&audio_state.running, 0, __ATOMIC_RELEASE);
        SDL_WaitThread(audio_state.thread, nullptr);
        audio_state.thread = nullptr;
    }
    close(audio_state.fd); audio_state.fd = -1;
}
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause_on) { if (device == 1) __atomic_store_n(&audio_state.paused, pause_on != 0, __ATOMIC_RELEASE); }

int SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 len)
{
    if (device != 1 || audio_state.fd < 0 || (!data && len)) return -1;
    SDL_LockAudioDevice(device);
    const Uint8 *source = (const Uint8 *)data;
    Uint32 written = 0;
    while (written < len) {
        const ssize_t result = write(audio_state.fd, source + written, len - written);
        if (result > 0) { written += (Uint32)result; continue; }
        if (result < 0 && (errno == EINTR || errno == EAGAIN)) { cooperative_yield(); continue; }
        SDL_UnlockAudioDevice(device);
        set_error("Virtua audio write failed");
        return -1;
    }
    SDL_UnlockAudioDevice(device);
    return 0;
}

Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID device)
{
    if (device != 1 || audio_state.fd < 0) return 0;
    pcm_status status{};
    if (ioctl(audio_state.fd, AUDIO_IOCTL_GET_STATUS, &status) != 0) return 0;
    return status.buffer_size >= status.buffer_free ? status.buffer_size - status.buffer_free : 0;
}

void SDL_ClearQueuedAudio(SDL_AudioDeviceID device) { if (device == 1 && audio_state.fd >= 0) (void)ioctl(audio_state.fd, AUDIO_IOCTL_RESET, nullptr); }
void SDL_LockAudioDevice(SDL_AudioDeviceID device) { if (device == 1 && audio_state.mutex) (void)SDL_LockMutex(audio_state.mutex); }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID device) { if (device == 1 && audio_state.mutex) (void)SDL_UnlockMutex(audio_state.mutex); }

SDL_mutex *SDL_CreateMutex(void)
{
    SDL_mutex *mutex = (SDL_mutex *)calloc(1, sizeof(*mutex));
    if (!mutex) return nullptr;
    if (pthread_mutex_init(&mutex->value, nullptr) != 0) { free(mutex); return nullptr; }
    return mutex;
}
void SDL_DestroyMutex(SDL_mutex *mutex) { if (mutex) { (void)pthread_mutex_destroy(&mutex->value); free(mutex); } }
int SDL_LockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_lock(&mutex->value) : -1; }
int SDL_UnlockMutex(SDL_mutex *mutex) { return mutex ? pthread_mutex_unlock(&mutex->value) : -1; }

SDL_cond *SDL_CreateCond(void)
{
    SDL_cond *condition = (SDL_cond *)calloc(1, sizeof(*condition));
    if (!condition) return nullptr;
    if (pthread_cond_init(&condition->value, nullptr) != 0) { free(condition); return nullptr; }
    return condition;
}
void SDL_DestroyCond(SDL_cond *condition) { if (condition) { (void)pthread_cond_destroy(&condition->value); free(condition); } }
int SDL_CondSignal(SDL_cond *condition) { return condition ? pthread_cond_signal(&condition->value) : -1; }
int SDL_CondWait(SDL_cond *condition, SDL_mutex *mutex) { return condition && mutex ? pthread_cond_wait(&condition->value, &mutex->value) : -1; }

int SDL_CondWaitTimeout(SDL_cond *condition, SDL_mutex *mutex, Uint32 milliseconds)
{
    if (!condition || !mutex) return -1;
    struct timespec deadline{};
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += milliseconds / 1000u;
    deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000l;
    if (deadline.tv_nsec >= 1000000000l) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000l; }
    const int result = pthread_cond_timedwait(&condition->value, &mutex->value, &deadline);
    return result == ETIMEDOUT ? SDL_MUTEX_TIMEDOUT : result;
}

SDL_Thread *SDL_CreateThread(SDL_ThreadFunction function, const char *, void *data)
{
    if (!function) return nullptr;
    SDL_Thread *thread = (SDL_Thread *)calloc(1, sizeof(*thread));
    if (!thread) return nullptr;
    thread->function = function;
    thread->data = data;
    if (pthread_create(&thread->handle, nullptr, thread_trampoline, thread) != 0) {
        free(thread); return nullptr;
    }
    return thread;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
    if (!thread) return;
    (void)pthread_join(thread->handle, nullptr);
    if (status) *status = thread->status;
    free(thread);
}

int SDL_SetThreadPriority(SDL_ThreadPriority) { return 0; }
int SDL_AtomicSet(SDL_atomic_t *value, int desired) { return value ? __atomic_exchange_n(&value->value, desired, __ATOMIC_SEQ_CST) : 0; }
int SDL_AtomicGet(SDL_atomic_t *value) { return value ? __atomic_load_n(&value->value, __ATOMIC_SEQ_CST) : 0; }

char *SDL_GetBasePath(void)
{
    char buffer[1024];
    if (!getcwd(buffer, sizeof(buffer))) return nullptr;
    const size_t length = strlen(buffer);
    char *result = (char *)malloc(length + 2u);
    if (!result) return nullptr;
    memcpy(result, buffer, length);
    result[length] = '/'; result[length + 1u] = '\0';
    return result;
}
void SDL_free(void *memory) { free(memory); }

int SDL_ShowSimpleMessageBox(Uint32, const char *title, const char *message, SDL_Window *)
{
    if (title) { (void)write(2, title, strlen(title)); (void)write(2, ": ", 2); }
    if (message) (void)write(2, message, strlen(message));
    (void)write(2, "\n", 1);
    return 0;
}

} // extern "C"
