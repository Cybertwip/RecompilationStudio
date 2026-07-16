#include "config_loader.h"
#include "launcher.h"

#include <SDL.h>
#include <SDL_opengl.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifndef PSX_TEST_ASSETS_DIR
#define PSX_TEST_ASSETS_DIR "."
#endif

int main() {
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
    game.name = "Frontend Smoke";

    /* Pause-menu SDL_QUIT means Resume. It is consumed during the first event
     * pass, after which the loop still updates and renders one complete frame
     * before returning. */
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
    const auto result = psx_launcher::run(
        window, context, settings, game, PSX_TEST_ASSETS_DIR,
        psx_launcher::Mode::PauseMenu);

    std::vector<uint32_t> pixels((size_t)width * height);
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
    read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    size_t visible_pixels = 0;
    size_t bright_pixels = 0;
    const auto* rgba = reinterpret_cast<const uint8_t*>(pixels.data());
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint8_t r = rgba[i * 4 + 0];
        const uint8_t g = rgba[i * 4 + 1];
        const uint8_t b = rgba[i * 4 + 2];
        if (r || g || b) ++visible_pixels;
        if (std::max(r, std::max(g, b)) >= 96) ++bright_pixels;
    }
    const bool rendered = visible_pixels > pixels.size() / 20 &&
                          bright_pixels > 1000;
    const bool committed = result == psx_launcher::Result::Launch &&
                           settings.renderer == 1 && settings.antialiasing &&
                           settings.p1_mode == PSXRecompV4::PAD_MODE_ANALOG;

    std::printf(
        "{\"schema\":1,\"artifact\":\"frontend_menu_smoke\","
        "\"rendered\":%s,\"visible_pixels\":%zu,\"bright_pixels\":%zu,"
        "\"total_pixels\":%zu,\"settings_committed\":%s}\n",
        rendered ? "true" : "false", visible_pixels, bright_pixels, pixels.size(),
        committed ? "true" : "false");

    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return rendered && committed ? 0 : 1;
}
