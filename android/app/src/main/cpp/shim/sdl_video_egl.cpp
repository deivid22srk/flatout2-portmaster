// shim/sdl_video_egl.cpp — window + GLES2 context over ANativeWindow/EGL.
//
// The guest asks SDL for an OpenGL ES context and swaps per frame. Here the
// "window" is the app's SurfaceView Surface (passed over JNI as an
// ANativeWindow). EGL calls are bionic code: every one runs inside a
// BionicGuard. On activity pause/resume the Surface is replaced; the frame gate
// in SDL_GL_SwapWindow recreates the EGL surface transparently for the guest.
#include "sdl_state.h"
#include "shim_common.h"
#include "../core/guest_thread.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>

using namespace fo2;
using fo2::BionicGuard;
using fo2::logi;

// SDL_GLattr indices we care about (values from the port's SDL2 headers):
//   0 RED_SIZE, 1 GREEN_SIZE, 2 BLUE_SIZE, 3 ALPHA_SIZE,
//   5 DOUBLEBUFFER, 6 DEPTH_SIZE, 7 STENCIL_SIZE
namespace {

struct WindowObj { int w, h; };
WindowObj g_window_obj{640, 480};

// One heap object per SDL_GLContext; the guest only passes the opaque pointer
// back to MakeCurrent/DeleteContext, and SwapWindow uses the thread-local copy.
struct CtxBundle {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLSurface surf = EGL_NO_SURFACE;
    EGLContext ctx = EGL_NO_CONTEXT;
};
static thread_local CtxBundle* t_current = nullptr;

EGLDisplay ensure_display() {
    if (g_video.egl_dpy == nullptr) {
        EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
        if (!eglInitialize(dpy, nullptr, nullptr)) return EGL_NO_DISPLAY;
        g_video.egl_dpy = (void*)dpy;
        logi("EGL initialized");
    }
    return (EGLDisplay)g_video.egl_dpy;
}

EGLConfig choose_config(EGLDisplay dpy) {
    if (g_video.egl_config != nullptr) return (EGLConfig)g_video.egl_config;
    const EGLint attribs[] = {
        EGL_RED_SIZE,   g_video.gl_attribs[0],
        EGL_GREEN_SIZE, g_video.gl_attribs[1],
        EGL_BLUE_SIZE,  g_video.gl_attribs[2],
        EGL_ALPHA_SIZE, g_video.gl_attribs[3],
        EGL_DEPTH_SIZE, g_video.gl_attribs[6],
        EGL_STENCIL_SIZE, g_video.gl_attribs[7],
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE
    };
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attribs, &cfg, 1, &n) || n < 1) {
        // Fallback: RGB565/D16, matches virtually every device.
        const EGLint fallback[] = {
            EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5,
            EGL_DEPTH_SIZE, 16, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
        eglChooseConfig(dpy, fallback, &cfg, 1, &n);
    }
    g_video.egl_config = (void*)cfg;
    return cfg;
}

// Blocks while the activity is paused or has no surface; recreates the EGL
// window surface if the ANativeWindow changed (activity resume).
// Returns the EGLSurface to swap on, or EGL_NO_SURFACE on hard failure.
EGLSurface frame_gate(EGLDisplay dpy, EGLContext ctx, EGLSurface* surf_slot) {
    while (true) {
        pthread_mutex_lock(&g_video.mu);
        while (g_video.paused.load() || g_video.anw == nullptr) {
            struct timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_video.resume_cv, &g_video.mu, &ts);
        }
        bool stale = g_video.surface_dirty.load() || g_video.surf_owner_anw != g_video.anw;
        if (stale && *surf_slot != EGL_NO_SURFACE) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglDestroySurface(dpy, *surf_slot);
            *surf_slot = EGL_NO_SURFACE;
        }
        if (*surf_slot == EGL_NO_SURFACE) {
            EGLSurface surf = eglCreateWindowSurface(dpy, choose_config(dpy),
                                                     (ANativeWindow*)g_video.anw, nullptr);
            if (surf != EGL_NO_SURFACE) {
                *surf_slot = surf;
                g_video.surf_owner_anw = g_video.anw;
                g_video.surface_dirty.store(false);
            }
        }
        EGLSurface surf = *surf_slot;
        pthread_mutex_unlock(&g_video.mu);
        if (surf != EGL_NO_SURFACE && eglMakeCurrent(dpy, surf, surf, ctx)) {
            return surf;
        }
        // Surface still failing: ask the app for a new one and keep waiting.
        g_video.surface_dirty.store(true);
        g_video.paused.store(true);
    }
}

} // namespace

namespace fo2 {

void video_set_surface(void* anw) {
    pthread_mutex_lock(&g_video.mu);
    if (anw != g_video.anw) g_video.surface_dirty.store(true);
    g_video.anw = anw;
    g_video.paused.store(false);
    pthread_cond_broadcast(&g_video.resume_cv);
    pthread_mutex_unlock(&g_video.mu);
}

void video_clear_surface() {
    pthread_mutex_lock(&g_video.mu);
    g_video.anw = nullptr;
    g_video.paused.store(true);
    pthread_mutex_unlock(&g_video.mu);
}

void video_set_paused(bool paused) {
    pthread_mutex_lock(&g_video.mu);
    g_video.paused.store(paused);
    pthread_cond_broadcast(&g_video.resume_cv);
    pthread_mutex_unlock(&g_video.mu);
}

} // namespace fo2

// ---- SDL video surface ----------------------------------------------------------

extern "C" SDL_Window* sdl_SDL_CreateWindow(const char*, int, int, int w, int h, Uint32 flags) {
    (void)flags;
    g_window_obj.w = w;
    g_window_obj.h = h;
    g_video.win_w = w;
    g_video.win_h = h;
    logi("SDL_CreateWindow %dx%d", w, h);
    return (SDL_Window*)&g_window_obj;
}

extern "C" void sdl_SDL_DestroyWindow(SDL_Window*) {
    // The Android surface belongs to the activity; nothing to free here.
}

extern "C" int sdl_SDL_GetDesktopDisplayMode(int, SDL_DisplayMode* mode) {
    if (mode == nullptr) return -1;
    memset(mode, 0, sizeof(*mode));
    mode->w = g_video.win_w;
    mode->h = g_video.win_h;
    mode->refresh_rate = 60;
    return 0;
}

extern "C" int sdl_SDL_GetCurrentDisplayMode(int, SDL_DisplayMode* mode) {
    return sdl_SDL_GetDesktopDisplayMode(0, mode);
}

// ---- GL context -----------------------------------------------------------------

extern "C" int sdl_SDL_GL_SetAttribute(int attr, int value) {
    switch (attr) {
        case 0: g_video.gl_attribs[0] = value; break;  // RED_SIZE
        case 1: g_video.gl_attribs[1] = value; break;  // GREEN_SIZE
        case 2: g_video.gl_attribs[2] = value; break;  // BLUE_SIZE
        case 3: g_video.gl_attribs[3] = value; break;  // ALPHA_SIZE
        case 5: g_video.gl_attribs[5] = value; break;  // DOUBLEBUFFER
        case 6: g_video.gl_attribs[6] = value; break;  // DEPTH_SIZE
        case 7: g_video.gl_attribs[7] = value; break;  // STENCIL_SIZE
        default: break;                                // rest: accepted, ignored
    }
    return 0;
}

extern "C" SDL_GLContext sdl_SDL_GL_CreateContext(SDL_Window*) {
    BionicGuard g;
    EGLDisplay dpy = ensure_display();
    if (dpy == EGL_NO_DISPLAY) return nullptr;

    while (g_video.anw == nullptr) {
        // Wait for the app to hand us a Surface (avoid a boot race).
        pthread_mutex_lock(&g_video.mu);
        if (g_video.anw == nullptr) {
            struct timespec ts{};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_video.resume_cv, &g_video.mu, &ts);
        }
        pthread_mutex_unlock(&g_video.mu);
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, choose_config(dpy), EGL_NO_CONTEXT, ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        logi("eglCreateContext failed: 0x%x", eglGetError());
        return nullptr;
    }

    EGLSurface surf = eglCreateWindowSurface(dpy, choose_config(dpy),
                                             (ANativeWindow*)g_video.anw, nullptr);
    if (surf == EGL_NO_SURFACE) {
        logi("eglCreateWindowSurface failed: 0x%x", eglGetError());
        eglDestroyContext(dpy, ctx);
        return nullptr;
    }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        logi("eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroySurface(dpy, surf);
        eglDestroyContext(dpy, ctx);
        return nullptr;
    }
    g_video.surf_owner_anw = g_video.anw;

    auto* bundle = new CtxBundle{dpy, surf, ctx};
    t_current = bundle;
    logi("SDL_GL_CreateContext ok (surf %p ctx %p)", (void*)surf, (void*)ctx);
    return (SDL_GLContext)bundle;
}

extern "C" int sdl_SDL_GL_MakeCurrent(SDL_Window*, SDL_GLContext glctx) {
    BionicGuard g;
    if (glctx == nullptr) {
        EGLDisplay dpy = (EGLDisplay)g_video.egl_dpy;
        if (dpy != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        }
        t_current = nullptr;
        return 0;
    }
    auto* b = (CtxBundle*)glctx;
    if (!eglMakeCurrent(b->dpy, b->surf, b->surf, b->ctx)) return -1;
    t_current = b;
    return 0;
}

extern "C" int sdl_SDL_GL_SwapWindow(SDL_Window*) {
    BionicGuard g;
    CtxBundle* b = t_current;
    if (b == nullptr) {
        // Swap on a context the shim did not create (not expected for this port).
        EGLContext cur = eglGetCurrentContext();
        if (cur == EGL_NO_CONTEXT) return -1;
        b = new CtxBundle{eglGetCurrentDisplay(), eglGetCurrentSurface(EGL_DRAW), cur};
        t_current = b;
    }
    EGLSurface surf = frame_gate(b->dpy, b->ctx, &b->surf);
    if (surf == EGL_NO_SURFACE) return -1;
    return eglSwapBuffers(b->dpy, surf) ? 0 : -1;
}

extern "C" void sdl_SDL_GL_DeleteContext(SDL_GLContext glctx) {
    if (glctx == nullptr) return;
    BionicGuard g;
    auto* b = (CtxBundle*)glctx;
    eglMakeCurrent(b->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (t_current == b) t_current = nullptr;
    if (b->surf != EGL_NO_SURFACE) eglDestroySurface(b->dpy, b->surf);
    eglDestroyContext(b->dpy, b->ctx);
    delete b;
}

extern "C" void* sdl_SDL_GL_GetProcAddress(const char* name) {
    if (name == nullptr) return nullptr;
    BionicGuard g;
    void* p = (void*)eglGetProcAddress(name);
    if (p == nullptr) {
        static void* gles = nullptr;
        static void* egl = nullptr;
        if (gles == nullptr) gles = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
        if (gles != nullptr) p = dlsym(gles, name);
        if (p == nullptr) {
            if (egl == nullptr) egl = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
            if (egl != nullptr) p = dlsym(egl, name);
        }
    }
    return p;
}

extern "C" int sdl_SDL_GL_GetDrawableSize(SDL_Window*, int* w, int* h) {
    if (w) *w = g_video.win_w;
    if (h) *h = g_video.win_h;
    return 0;
}

extern "C" int sdl_SDL_GL_SetSwapInterval(int interval) {
    BionicGuard g;
    EGLDisplay dpy = (EGLDisplay)g_video.egl_dpy;
    if (dpy != EGL_NO_DISPLAY) eglSwapInterval(dpy, interval ? 1 : 0);
    return 0;
}
