#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t Uint8;
typedef int8_t Sint8;
typedef uint16_t Uint16;
typedef int16_t Sint16;
typedef uint32_t Uint32;
typedef int32_t Sint32;
typedef uint64_t Uint64;
typedef int64_t Sint64;
typedef int32_t SDL_JoystickID;
typedef struct SDL_JoystickGUID { Uint8 data[16]; } SDL_JoystickGUID;
typedef uint32_t SDL_AudioDeviceID;
typedef int32_t SDL_Keycode;
typedef int SDL_bool;

enum { SDL_FALSE = 0, SDL_TRUE = 1 };

enum {
    SDL_INIT_TIMER = 0x00000001u,
    SDL_INIT_AUDIO = 0x00000010u,
    SDL_INIT_VIDEO = 0x00000020u,
    SDL_INIT_JOYSTICK = 0x00000200u,
    SDL_INIT_GAMECONTROLLER = 0x00002000u,
};

enum {
    SDL_QUIT = 0x100,
    SDL_WINDOWEVENT = 0x200,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP = 0x301,
    SDL_CONTROLLERDEVICEADDED = 0x650,
    SDL_CONTROLLERDEVICEREMOVED = 0x651,
};

enum { SDL_WINDOWEVENT_FOCUS_LOST = 13 };

enum {
    SDL_WINDOW_FULLSCREEN = 0x00000001u,
    SDL_WINDOW_OPENGL = 0x00000002u,
    SDL_WINDOW_SHOWN = 0x00000004u,
    SDL_WINDOW_HIDDEN = 0x00000008u,
    SDL_WINDOW_RESIZABLE = 0x00000020u,
    SDL_WINDOW_VULKAN = 0x10000000u,
    SDL_WINDOW_ALWAYS_ON_TOP = 0x00008000u,
    SDL_WINDOW_FULLSCREEN_DESKTOP = SDL_WINDOW_FULLSCREEN | 0x00001000u,
};

#define SDL_WINDOWPOS_CENTERED 0x2FFF0000u
#define SDL_VERSION_ATLEAST(major, minor, patch) \
    (((major) < 2) || ((major) == 2 && ((minor) < 26 || ((minor) == 26 && (patch) <= 0))))

enum {
    SDL_RENDERER_SOFTWARE = 0x00000001u,
    SDL_RENDERER_ACCELERATED = 0x00000002u,
    SDL_RENDERER_PRESENTVSYNC = 0x00000004u,
};

enum { SDL_TEXTUREACCESS_STREAMING = 1 };
#define SDL_PIXELFORMAT_ARGB8888 0x16362004u

typedef enum SDL_ScaleMode {
    SDL_ScaleModeNearest = 0,
    SDL_ScaleModeLinear = 1,
} SDL_ScaleMode;

typedef enum SDL_Scancode {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4,
    SDL_SCANCODE_B = 5,
    SDL_SCANCODE_C = 6,
    SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8,
    SDL_SCANCODE_F = 9,
    SDL_SCANCODE_G = 10,
    SDL_SCANCODE_H = 11,
    SDL_SCANCODE_I = 12,
    SDL_SCANCODE_J = 13,
    SDL_SCANCODE_K = 14,
    SDL_SCANCODE_L = 15,
    SDL_SCANCODE_M = 16,
    SDL_SCANCODE_N = 17,
    SDL_SCANCODE_O = 18,
    SDL_SCANCODE_P = 19,
    SDL_SCANCODE_Q = 20,
    SDL_SCANCODE_R = 21,
    SDL_SCANCODE_S = 22,
    SDL_SCANCODE_T = 23,
    SDL_SCANCODE_U = 24,
    SDL_SCANCODE_V = 25,
    SDL_SCANCODE_W = 26,
    SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Y = 28,
    SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_RETURN = 40,
    SDL_SCANCODE_ESCAPE = 41,
    SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43,
    SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_BACKSLASH = 49,
    SDL_SCANCODE_F1 = 58,
    SDL_SCANCODE_F2 = 59,
    SDL_SCANCODE_F3 = 60,
    SDL_SCANCODE_F4 = 61,
    SDL_SCANCODE_F5 = 62,
    SDL_SCANCODE_F6 = 63,
    SDL_SCANCODE_F7 = 64,
    SDL_SCANCODE_F8 = 65,
    SDL_SCANCODE_F9 = 66,
    SDL_SCANCODE_F10 = 67,
    SDL_SCANCODE_F11 = 68,
    SDL_SCANCODE_F12 = 69,
    SDL_SCANCODE_RIGHT = 79,
    SDL_SCANCODE_LEFT = 80,
    SDL_SCANCODE_DOWN = 81,
    SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_KP_ENTER = 88,
    SDL_SCANCODE_LCTRL = 224,
    SDL_SCANCODE_LSHIFT = 225,
    SDL_SCANCODE_LALT = 226,
    SDL_SCANCODE_RCTRL = 228,
    SDL_SCANCODE_RSHIFT = 229,
    SDL_SCANCODE_RALT = 230,
    SDL_NUM_SCANCODES = 512,
} SDL_Scancode;

enum {
    SDLK_UNKNOWN = 0,
    SDLK_RETURN = '\r',
    SDLK_ESCAPE = 27,
    SDLK_f = 'f',
    SDLK_F1 = 0x4000003a,
    SDLK_F2,
    SDLK_F3,
    SDLK_F4,
    SDLK_F5,
    SDLK_F6,
    SDLK_F7,
    SDLK_F8,
    SDLK_F9,
    SDLK_F10,
    SDLK_F11,
    SDLK_F12,
};

enum {
    KMOD_NONE = 0x0000,
    KMOD_LSHIFT = 0x0001,
    KMOD_RSHIFT = 0x0002,
    KMOD_LCTRL = 0x0040,
    KMOD_RCTRL = 0x0080,
    KMOD_LALT = 0x0100,
    KMOD_RALT = 0x0200,
    KMOD_LGUI = 0x0400,
    KMOD_RGUI = 0x0800,
    KMOD_CTRL = KMOD_LCTRL | KMOD_RCTRL,
    KMOD_SHIFT = KMOD_LSHIFT | KMOD_RSHIFT,
    KMOD_ALT = KMOD_LALT | KMOD_RALT,
    KMOD_GUI = KMOD_LGUI | KMOD_RGUI,
};

typedef struct SDL_Keysym {
    SDL_Scancode scancode;
    SDL_Keycode sym;
    Uint16 mod;
    Uint32 unused;
} SDL_Keysym;

typedef struct SDL_KeyboardEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 state;
    Uint8 repeat;
    Uint8 padding2;
    Uint8 padding3;
    SDL_Keysym keysym;
} SDL_KeyboardEvent;

typedef struct SDL_ControllerDeviceEvent {
    Uint32 type;
    Uint32 timestamp;
    Sint32 which;
} SDL_ControllerDeviceEvent;

typedef struct SDL_WindowEvent {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowID;
    Uint8 event;
    Uint8 padding1;
    Uint8 padding2;
    Uint8 padding3;
    Sint32 data1;
    Sint32 data2;
} SDL_WindowEvent;

typedef union SDL_Event {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_ControllerDeviceEvent cdevice;
    SDL_WindowEvent window;
    Uint8 padding[64];
} SDL_Event;

typedef struct SDL_Rect { int x, y, w, h; } SDL_Rect;
typedef struct SDL_DisplayMode {
    Uint32 format;
    int w;
    int h;
    int refresh_rate;
    void *driverdata;
} SDL_DisplayMode;

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef void *SDL_GLContext;

typedef void (*SDL_AudioCallback)(void *userdata, Uint8 *stream, int len);
typedef Uint16 SDL_AudioFormat;
typedef struct SDL_AudioSpec {
    int freq;
    SDL_AudioFormat format;
    Uint8 channels;
    Uint8 silence;
    Uint16 samples;
    Uint16 padding;
    Uint32 size;
    SDL_AudioCallback callback;
    void *userdata;
} SDL_AudioSpec;

#define AUDIO_S16SYS 0x8010u
#define SDL_AUDIO_ALLOW_FREQUENCY_CHANGE 0x00000001u

typedef enum SDL_GameControllerAxis {
    SDL_CONTROLLER_AXIS_INVALID = -1,
    SDL_CONTROLLER_AXIS_LEFTX,
    SDL_CONTROLLER_AXIS_LEFTY,
    SDL_CONTROLLER_AXIS_RIGHTX,
    SDL_CONTROLLER_AXIS_RIGHTY,
    SDL_CONTROLLER_AXIS_TRIGGERLEFT,
    SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
    SDL_CONTROLLER_AXIS_MAX,
} SDL_GameControllerAxis;

typedef enum SDL_GameControllerButton {
    SDL_CONTROLLER_BUTTON_INVALID = -1,
    SDL_CONTROLLER_BUTTON_A,
    SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X,
    SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK,
    SDL_CONTROLLER_BUTTON_GUIDE,
    SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_LEFTSTICK,
    SDL_CONTROLLER_BUTTON_RIGHTSTICK,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP,
    SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT,
    SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_MAX,
} SDL_GameControllerButton;

typedef struct SDL_Joystick { SDL_JoystickID instance; } SDL_Joystick;
typedef struct SDL_GameController {
    SDL_Joystick joystick;
    int attached;
} SDL_GameController;

typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;
typedef struct SDL_Thread SDL_Thread;
typedef int SDL_thread;
typedef int (*SDL_ThreadFunction)(void *data);
typedef struct SDL_atomic_t { int value; } SDL_atomic_t;

enum { SDL_MUTEX_TIMEDOUT = 1 };
typedef enum SDL_ThreadPriority { SDL_THREAD_PRIORITY_LOW = 0 } SDL_ThreadPriority;

enum {
    SDL_GL_CONTEXT_PROFILE_MASK = 21,
    SDL_GL_CONTEXT_MAJOR_VERSION = 17,
    SDL_GL_CONTEXT_MINOR_VERSION = 18,
    SDL_GL_CONTEXT_FLAGS = 20,
    SDL_GL_DOUBLEBUFFER = 5,
    SDL_GL_STENCIL_SIZE = 7,
    SDL_GL_CONTEXT_PROFILE_CORE = 1,
    SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG = 0x0002,
};

enum { SDL_MESSAGEBOX_ERROR = 0x10 };

#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_HINT_JOYSTICK_HIDAPI "SDL_JOYSTICK_HIDAPI"
#define SDL_HINT_JOYSTICK_HIDAPI_XBOX "SDL_JOYSTICK_HIDAPI_XBOX"
#define SDL_HINT_JOYSTICK_RAWINPUT "SDL_JOYSTICK_RAWINPUT"
#define SDL_HINT_RENDER_DRIVER "SDL_RENDER_DRIVER"
#define SDL_HINT_RENDER_SCALE_QUALITY "SDL_RENDER_SCALE_QUALITY"

#define SDL_zero(value) memset(&(value), 0, sizeof(value))

int SDL_Init(Uint32 flags);
int SDL_InitSubSystem(Uint32 flags);
Uint32 SDL_WasInit(Uint32 flags);
void SDL_Quit(void);
const char *SDL_GetError(void);
SDL_bool SDL_SetHint(const char *name, const char *value);

Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
Uint32 SDL_GetTicks(void);
void SDL_Delay(Uint32 milliseconds);

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags);
void SDL_DestroyWindow(SDL_Window *window);
void SDL_ShowWindow(SDL_Window *window);
void SDL_HideWindow(SDL_Window *window);
void SDL_RaiseWindow(SDL_Window *window);
Uint32 SDL_GetWindowFlags(SDL_Window *window);
void SDL_GetWindowSize(SDL_Window *window, int *w, int *h);
void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y);
int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags);
int SDL_GetWindowDisplayIndex(SDL_Window *window);
int SDL_GetCurrentDisplayMode(int display_index, SDL_DisplayMode *mode);
int SDL_GetDisplayUsableBounds(int display_index, SDL_Rect *rect);
const char *SDL_GetCurrentVideoDriver(void);

SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, int index, Uint32 flags);
void SDL_DestroyRenderer(SDL_Renderer *renderer);
int SDL_RenderSetLogicalSize(SDL_Renderer *renderer, int w, int h);
int SDL_RenderSetVSync(SDL_Renderer *renderer, int vsync);
int SDL_SetRenderDrawColor(SDL_Renderer *renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int SDL_RenderClear(SDL_Renderer *renderer);
int SDL_RenderCopy(SDL_Renderer *renderer, SDL_Texture *texture,
                   const SDL_Rect *source, const SDL_Rect *destination);
void SDL_RenderPresent(SDL_Renderer *renderer);
SDL_Texture *SDL_CreateTexture(SDL_Renderer *renderer, Uint32 format, int access,
                               int w, int h);
void SDL_DestroyTexture(SDL_Texture *texture);
int SDL_UpdateTexture(SDL_Texture *texture, const SDL_Rect *rect,
                      const void *pixels, int pitch);
int SDL_SetTextureScaleMode(SDL_Texture *texture, SDL_ScaleMode scale_mode);

int SDL_GL_SetAttribute(int attr, int value);
void SDL_GL_ResetAttributes(void);
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window);
void SDL_GL_DeleteContext(SDL_GLContext context);
int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context);
SDL_GLContext SDL_GL_GetCurrentContext(void);
SDL_Window *SDL_GL_GetCurrentWindow(void);
int SDL_GL_SetSwapInterval(int interval);
void SDL_GL_SwapWindow(SDL_Window *window);
void *SDL_GL_GetProcAddress(const char *name);
int SDL_Vulkan_LoadLibrary(const char *path);

int SDL_PollEvent(SDL_Event *event);
void SDL_PumpEvents(void);
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode key);
SDL_Scancode SDL_GetScancodeFromName(const char *name);
const char *SDL_GetScancodeName(SDL_Scancode scancode);

int SDL_NumJoysticks(void);
SDL_bool SDL_IsGameController(int joystick_index);
SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index);
SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index);
void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *text, int text_size);
const char *SDL_GameControllerNameForIndex(int joystick_index);
const char *SDL_GameControllerPathForIndex(int joystick_index);
const char *SDL_GameControllerGetSerial(SDL_GameController *controller);
SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick);
SDL_GameController *SDL_GameControllerOpen(int joystick_index);
void SDL_GameControllerClose(SDL_GameController *controller);
SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID instance_id);
SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *controller);
SDL_bool SDL_GameControllerGetAttached(SDL_GameController *controller);
Sint16 SDL_GameControllerGetAxis(SDL_GameController *controller, SDL_GameControllerAxis axis);
Uint8 SDL_GameControllerGetButton(SDL_GameController *controller, SDL_GameControllerButton button);
SDL_GameControllerAxis SDL_GameControllerGetAxisFromString(const char *text);
SDL_GameControllerButton SDL_GameControllerGetButtonFromString(const char *text);
void SDL_GameControllerUpdate(void);
int SDL_GameControllerAddMappingsFromFile(const char *path);

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture,
                                      const SDL_AudioSpec *desired,
                                      SDL_AudioSpec *obtained, int allowed_changes);
void SDL_CloseAudioDevice(SDL_AudioDeviceID device);
void SDL_PauseAudioDevice(SDL_AudioDeviceID device, int pause_on);
int SDL_QueueAudio(SDL_AudioDeviceID device, const void *data, Uint32 len);
Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID device);
void SDL_ClearQueuedAudio(SDL_AudioDeviceID device);
void SDL_LockAudioDevice(SDL_AudioDeviceID device);
void SDL_UnlockAudioDevice(SDL_AudioDeviceID device);

SDL_mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_mutex *mutex);
int SDL_LockMutex(SDL_mutex *mutex);
int SDL_UnlockMutex(SDL_mutex *mutex);
SDL_cond *SDL_CreateCond(void);
void SDL_DestroyCond(SDL_cond *condition);
int SDL_CondSignal(SDL_cond *condition);
int SDL_CondWait(SDL_cond *condition, SDL_mutex *mutex);
int SDL_CondWaitTimeout(SDL_cond *condition, SDL_mutex *mutex, Uint32 milliseconds);
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction function, const char *name, void *data);
void SDL_WaitThread(SDL_Thread *thread, int *status);
int SDL_SetThreadPriority(SDL_ThreadPriority priority);
int SDL_AtomicSet(SDL_atomic_t *value, int desired);
int SDL_AtomicGet(SDL_atomic_t *value);

char *SDL_GetBasePath(void);
void SDL_free(void *memory);
int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message,
                             SDL_Window *window);

#ifdef __cplusplus
}
#endif
