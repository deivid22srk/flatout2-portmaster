// shim/sdl_state.h — internal state shared by the SDL2 shim implementation files.
#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <deque>

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_audio.h>

namespace fo2 {

// --- video (sdl_video_egl.cpp) ---
struct VideoState {
    // set from JNI (bionic side)
    void* anw = nullptr;                 // ANativeWindow*
    std::atomic<bool> paused{false};
    std::atomic<bool> surface_dirty{false};

    // window/attrs (written on guest threads, read in shim)
    int win_w = 640, win_h = 480;
    int gl_attribs[8] = {5, 5, 5, 0, 16, 1, 24, 8}; // index by SDL_GLattr subset below

    // EGL handles
    void* egl_dpy = nullptr;             // EGLDisplay
    void* egl_config = nullptr;          // EGLConfig
    void* egl_surf = nullptr;            // EGLSurface (current window surface)
    void* surf_owner_anw = nullptr;      // ANativeWindow the surface was created on

    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t resume_cv = PTHREAD_COND_INITIALIZER;
};
extern VideoState g_video;

// called by sdl2_shim / bridge
void video_init_once();

// --- audio (sdl_audio_opensl.cpp) ---
struct AudioState {
    SDL_AudioSpec desired{}, obtained{};
    SDL_AudioCallback callback = nullptr;
    void* userdata = nullptr;
    std::atomic<bool> open{false};
    std::atomic<bool> running{false};
};
extern AudioState g_audio;

// --- input (sdl2_shim.cpp) ---
struct InputState {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    std::deque<SDL_Event> queue;
    int16_t axes[6] = {0, 0, 0, 0, 0, 0};   // SDL_GameControllerAxis
    uint8_t buttons[15] = {0};              // SDL_GameControllerButton
    bool device_announced = false;
};
extern InputState g_input;

} // namespace fo2
