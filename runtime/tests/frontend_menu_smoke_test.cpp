#include "config_loader.h"
#include "launcher.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef PSX_TEST_ASSETS_DIR
#define PSX_TEST_ASSETS_DIR "."
#endif

int main() {
    /* Optional: PSX_LAUNCHER_VIEW=controls exercises the keybind page layout. */
    const char* start_view = std::getenv("PSX_LAUNCHER_VIEW");
    const bool controls_view =
        start_view && std::strcmp(start_view, "controls") == 0;

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 2;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    constexpr int width = 1280;
    constexpr int height = 800;
    SDL_Window* window = SDL_CreateWindow(
        "PSXRecomp settings smoke", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        SDL_Quit();
        return 3;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 4;
    }
    SDL_GL_MakeCurrent(window, context);
    SDL_GL_SetSwapInterval(0);

    PSXRecompV4::UserSettings settings;
    settings.renderer = 1; settings.has_renderer = true;
    settings.antialiasing = true; settings.has_antialiasing = true;
    settings.fullscreen = false; settings.has_fullscreen = true;
    settings.p1_device = "auto"; settings.has_p1_device = true;
    settings.p2_device = "auto"; settings.has_p2_device = true;
    settings.p1_mode = PSXRecompV4::PAD_MODE_ANALOG;
    settings.p2_mode = PSXRecompV4::PAD_MODE_ANALOG;
    settings.has_p1_mode = settings.has_p2_mode = true;

    psx_launcher::GameInfo game;
    game.name = controls_view ? "Sonic Wings Special" : "Frontend Smoke";

    /* Pump a couple of no-op events so a controls start-view can rebuild its
     * keybind table after the first Update makes the panel visible, then quit.
     * Pause-menu SDL_QUIT means Resume. */
    for (int i = 0; i < 3; ++i) {
        SDL_Event pulse{};
        pulse.type = SDL_USEREVENT;
        SDL_PushEvent(&pulse);
    }
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    const auto result = psx_launcher::run(
        window, context, settings, game, PSX_TEST_ASSETS_DIR,
        psx_launcher::Mode::PauseMenu);

    int draw_w = width, draw_h = height;
    SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
    if (draw_w <= 0 || draw_h <= 0) { draw_w = width; draw_h = height; }

    std::vector<uint32_t> pixels((size_t)draw_w * (size_t)draw_h);
    using FinishFn = void (*)();
    using ReadBufferFn = void (*)(GLenum);
    using ReadPixelsFn = void (*)(GLint, GLint, GLsizei, GLsizei,
                                  GLenum, GLenum, void*);
    const auto finish = reinterpret_cast<FinishFn>(SDL_GL_GetProcAddress("glFinish"));
    const auto read_buffer = reinterpret_cast<ReadBufferFn>(
        SDL_GL_GetProcAddress("glReadBuffer"));
    const auto read_pixels = reinterpret_cast<ReadPixelsFn>(
        SDL_GL_GetProcAddress("glReadPixels"));
    if (!finish || !read_buffer || !read_pixels) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 5;
    }
    finish();
    read_buffer(GL_FRONT);
    read_pixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t visible_pixels = 0;
    size_t bright_pixels = 0;
    int content_max_x = 0;
    const auto* rgba = reinterpret_cast<const uint8_t*>(pixels.data());
    for (int y = 0; y < draw_h; ++y) {
        for (int x = 0; x < draw_w; ++x) {
            const size_t i = ((size_t)y * (size_t)draw_w + (size_t)x) * 4;
            const uint8_t r = rgba[i + 0];
            const uint8_t g = rgba[i + 1];
            const uint8_t b = rgba[i + 2];
            if (r || g || b) ++visible_pixels;
            if (std::max(r, std::max(g, b)) >= 96) {
                ++bright_pixels;
                if (x > content_max_x) content_max_x = x;
            }
        }
    }
    const bool rendered = visible_pixels > pixels.size() / 20 &&
                          bright_pixels > 1000;
    const bool committed = result == psx_launcher::Result::Launch &&
                           settings.renderer == 1 && settings.antialiasing &&
                           settings.p1_mode == PSXRecompV4::PAD_MODE_ANALOG;
    /* Controls page must spread across the window (old bug collapsed it to the
     * left ~third, so content_max_x stayed under half width). */
    const bool layout_ok = !controls_view ||
                           (content_max_x > draw_w * 55 / 100);

    std::printf(
        "{\"schema\":1,\"artifact\":\"frontend_menu_smoke\","
        "\"view\":\"%s\",\"rendered\":%s,\"visible_pixels\":%zu,"
        "\"bright_pixels\":%zu,\"total_pixels\":%zu,\"content_max_x\":%d,"
        "\"drawable\":[%d,%d],\"layout_ok\":%s,\"settings_committed\":%s}\n",
        controls_view ? "controls" : "dashboard",
        rendered ? "true" : "false", visible_pixels, bright_pixels, pixels.size(),
        content_max_x, draw_w, draw_h,
        layout_ok ? "true" : "false",
        committed ? "true" : "false");

    if (const char* dump = std::getenv("PSX_LAUNCHER_DUMP_PPM")) {
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fprintf(f, "P6\n%d %d\n255\n", draw_w, draw_h);
            /* GL origin is bottom-left; flip for a top-left image. */
            for (int y = draw_h - 1; y >= 0; --y) {
                for (int x = 0; x < draw_w; ++x) {
                    const uint8_t* c =
                        reinterpret_cast<const uint8_t*>(
                            &pixels[(size_t)y * (size_t)draw_w + (size_t)x]);
                    std::fputc(c[0], f);
                    std::fputc(c[1], f);
                    std::fputc(c[2], f);
                }
            }
            std::fclose(f);
            std::printf("wrote %s\n", dump);
        }
    }

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rendered && committed && layout_ok ? 0 : 1;
}
