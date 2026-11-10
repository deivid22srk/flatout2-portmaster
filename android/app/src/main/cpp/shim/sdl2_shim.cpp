// shim/sdl2_shim.cpp — the slice of SDL2 that fo2.aarch64 actually imports:
// init/error, event queue, the virtual game controller and RWops.
// Backed by events injected from Java over JNI (touch overlay + Android gamepads).
#include "sdl_state.h"
#include "shim_common.h"
#include "../core/guest_thread.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace fo2;
using fo2::BionicGuard;
using fo2::logi;

// SDL_GameControllerButton / SDL_GameControllerAxis come from the real SDL2 headers.

namespace fo2 {
VideoState g_video;
AudioState g_audio;
InputState g_input;
} // namespace fo2

void video_init_once() {}

// ---- init / error -------------------------------------------------------------

extern "C" int sdl_SDL_Init(Uint32 flags) {
    logi("SDL_Init(0x%x)", flags);
    return 0;
}

extern "C" int sdl_SDL_InitSubSystem(Uint32 flags) {
    return 0;
}

extern "C" void sdl_SDL_Quit(void) {}

extern "C" Uint32 sdl_SDL_WasInit(Uint32 flags) {
    return flags; // pretend everything requested is up
}

extern "C" const char* sdl_SDL_GetError(void) {
    return "";
}

extern "C" void sdl_SDL_free(void* p) {
    if (p == nullptr) return;
    BionicGuard g;
    free(p);
}

// ---- events --------------------------------------------------------------------

extern "C" int sdl_SDL_PollEvent(SDL_Event* event) {
    if (event == nullptr) return 0;
    BionicGuard g;
    pthread_mutex_lock(&g_input.mu);
    if (!g_input.device_announced) {
        // Announce the virtual controller once, like a hot-plugged pad.
        g_input.device_announced = true;
        SDL_Event ev{};
        ev.cdevice.type = SDL_CONTROLLERDEVICEADDED;
        ev.cdevice.timestamp = 0;
        ev.cdevice.which = 0;
        g_input.queue.push_back(ev);
    }
    if (g_input.queue.empty()) {
        pthread_mutex_unlock(&g_input.mu);
        return 0;
    }
    *event = g_input.queue.front();
    g_input.queue.pop_front();
    pthread_mutex_unlock(&g_input.mu);
    return 1;
}

extern "C" int sdl_SDL_PushEvent(SDL_Event* event) {
    if (event == nullptr) return -1;
    BionicGuard g;
    pthread_mutex_lock(&g_input.mu);
    g_input.queue.push_back(*event);
    pthread_mutex_unlock(&g_input.mu);
    return 1;
}

// ---- virtual controller (fed by JNI) --------------------------------------------

namespace fo2 {

void input_set_axis(int axis, float value) {
    if (axis < 0 || axis > 5) return;
    if (value > 1.f) value = 1.f;
    if (value < -1.f) value = -1.f;
    int16_t raw = (int16_t)(value * 32767.f);

    SDL_Event ev{};
    ev.caxis.type = SDL_CONTROLLERAXISMOTION;
    ev.caxis.timestamp = 0;
    ev.caxis.which = 0;
    ev.caxis.axis = (uint8_t)axis;
    ev.caxis.value = raw;

    BionicGuard g;
    pthread_mutex_lock(&g_input.mu);
    g_input.axes[axis] = raw;
    if (g_input.queue.size() < 256) g_input.queue.push_back(ev);
    pthread_mutex_unlock(&g_input.mu);
}

void input_set_button(int button, bool down) {
    if (button < 0 || button > 14) return;

    SDL_Event ev{};
    ev.cbutton.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    ev.cbutton.timestamp = 0;
    ev.cbutton.which = 0;
    ev.cbutton.button = (uint8_t)button;
    ev.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED;

    BionicGuard g;
    pthread_mutex_lock(&g_input.mu);
    g_input.buttons[button] = down ? 1 : 0;
    if (g_input.queue.size() < 256) g_input.queue.push_back(ev);
    pthread_mutex_unlock(&g_input.mu);
}

} // namespace fo2

extern "C" int sdl_SDL_NumJoysticks(void) { return 1; }

extern "C" SDL_bool sdl_SDL_IsGameController(int index) {
    return index == 0 ? SDL_TRUE : SDL_FALSE;
}

extern "C" SDL_GameController* sdl_SDL_GameControllerOpen(int index) {
    return index == 0 ? (SDL_GameController*)(uintptr_t)1 : nullptr;
}

extern "C" void sdl_SDL_GameControllerClose(SDL_GameController*) {}

extern "C" const char* sdl_SDL_GameControllerName(SDL_GameController*) {
    return "FlatOut2 Virtual Controller";
}

extern "C" SDL_Joystick* sdl_SDL_GameControllerGetJoystick(SDL_GameController* ctrl) {
    return (SDL_Joystick*)ctrl;
}

extern "C" Sint16 sdl_SDL_GameControllerGetAxis(SDL_GameController*, int axis) {
    if (axis < 0 || axis > 5) return 0;
    // Racing state polling happens every frame on a guest thread: lockless read
    // of int16 is atomic enough, and skips a TPIDR swap per call.
    return g_input.axes[axis];
}

extern "C" Uint8 sdl_SDL_GameControllerGetButton(SDL_GameController*, int button) {
    if (button < 0 || button > 14) return 0;
    return g_input.buttons[button];
}

extern "C" int sdl_SDL_GameControllerAddMapping(const char*) { return 1; }

extern "C" int sdl_SDL_GameControllerAddMappingsFromRW(SDL_RWops* rw) {
    if (rw != nullptr) sdl_SDL_free(rw);   // SDL frees the RWops on success paths
    return 0;
}

extern "C" char* sdl_SDL_GameControllerMapping(SDL_GameController*) {
    BionicGuard g;
    return strdup("FlatOut2 Virtual Controller,a:b0,b:b1,x:b2,y:b3,"
                  "back:b4,start:b6,leftstick:b7,rightstick:b8,leftshoulder:b9,"
                  "rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,"
                  "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5");
}

extern "C" char* sdl_SDL_GameControllerMappingForDeviceIndex(int) {
    return sdl_SDL_GameControllerMapping(nullptr);
}

extern "C" const char* sdl_SDL_GameControllerGetStringForButton(int button) {
    static const char* names[] = {"a", "b", "x", "y", "back", "guide", "start",
                                  "leftstick", "rightstick", "leftshoulder",
                                  "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
    if (button < 0 || button > 14) return "";
    return names[button];
}

extern "C" int sdl_SDL_GameControllerGetButtonFromString(const char* s) {
    static const char* names[] = {"a", "b", "x", "y", "back", "guide", "start",
                                  "leftstick", "rightstick", "leftshoulder",
                                  "rightshoulder", "dpup", "dpdown", "dpleft", "dpright"};
    if (s == nullptr) return -1;
    for (int i = 0; i < 15; ++i)
        if (strcasecmp(s, names[i]) == 0) return i;
    return -1;
}

extern "C" SDL_JoystickGUID sdl_SDL_JoystickGetDeviceGUID(int) {
    SDL_JoystickGUID guid{};
    const char* hex = "03000000000000000000000000000000";
    for (int i = 0; i < 16; ++i) {
        unsigned v;
        sscanf(hex + i * 2, "%2x", &v);
        guid.data[i] = (uint8_t)v;
    }
    return guid;
}

extern "C" void sdl_SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char* out, int max) {
    if (out == nullptr || max <= 0) return;
    static const char* hex = "0123456789abcdef";
    int pos = 0;
    for (int i = 0; i < 16 && pos + 2 < max; ++i) {
        out[pos++] = hex[guid.data[i] >> 4];
        out[pos++] = hex[guid.data[i] & 0xF];
    }
    out[pos] = '\0';
}

extern "C" SDL_JoystickID sdl_SDL_JoystickInstanceID(SDL_Joystick*) { return 0; }

extern "C" const char* sdl_SDL_JoystickNameForIndex(int) {
    return "FlatOut2 Virtual Controller";
}

// ---- RWops (file-backed; struct layout comes from the port's own SDL2 headers) --

static Sint64 rw_size(SDL_RWops* rw) {
    BionicGuard g;
    FILE* fp = (FILE*)rw->hidden.stdio.fp;
    long cur = ftell(fp);
    fseek(fp, 0, SEEK_END);
    long end = ftell(fp);
    fseek(fp, cur, SEEK_SET);
    return end;
}

static Sint64 rw_seek(SDL_RWops* rw, Sint64 off, int whence) {
    BionicGuard g;
    FILE* fp = (FILE*)rw->hidden.stdio.fp;
    return fseek(fp, (long)off, whence) == 0 ? ftell(fp) : -1;
}

static size_t rw_read(SDL_RWops* rw, void* ptr, size_t size, size_t maxnum) {
    BionicGuard g;
    FILE* fp = (FILE*)rw->hidden.stdio.fp;
    return fread(ptr, size, maxnum, fp);
}

static size_t rw_write(SDL_RWops*, const void*, size_t, size_t) {
    return 0; // read-only
}

static int rw_close(SDL_RWops* rw) {
    if (rw == nullptr) return 0;
    {
        BionicGuard g;
        FILE* fp = (FILE*)rw->hidden.stdio.fp;
        if (fp) fclose(fp);
        free(rw);
    }
    return 0;
}

extern "C" SDL_RWops* sdl_SDL_RWFromFile(const char* file, const char* mode) {
    BionicGuard g;
    FILE* fp = fopen(file, mode ? mode : "rb");
    if (fp == nullptr) return nullptr;
    SDL_RWops* rw = (SDL_RWops*)calloc(1, sizeof(SDL_RWops));
    if (rw == nullptr) { fclose(fp); return nullptr; }
    rw->size = rw_size;
    rw->seek = rw_seek;
    rw->read = rw_read;
    rw->write = rw_write;
    rw->close = rw_close;
    rw->type = 0;                       // SDL_RWOPS_UNKNOWN is fine for fptr-only use
    rw->hidden.stdio.autoclose = SDL_FALSE;
    rw->hidden.stdio.fp = fp;
    return rw;
}
