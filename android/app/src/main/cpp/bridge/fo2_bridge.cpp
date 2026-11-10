// bridge/fo2_bridge.cpp — JNI surface between the app and the guest runtime.
#include "../core/guest_thread.h"
#include "../core/shim_common.h"
#include "../shim/sdl_state.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <ucontext.h>

using namespace fo2;

static JavaVM* g_vm = nullptr;

jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

// ---- crash reporting: the first on-device runs need actionable logs -------------

static void crash_handler(int sig, siginfo_t* info, void* ctx) {
    // May run on a guest thread (TPIDR = guest TCB): swap before bionic calls.
    BionicGuard g;
    uint64_t pc = 0;
    if (ctx != nullptr) {
        ucontext_t* uc = (ucontext_t*)ctx;
        pc = uc->uc_mcontext.pc;
    }
    __android_log_print(ANDROID_LOG_ERROR, "FO2",
                        "CRASH signal=%d addr=%p pc=0x%llx%s", sig, info ? info->si_addr : nullptr,
                        (unsigned long long)pc, " (see full logcat)");
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers() {
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
}

// ---- lifecycle -------------------------------------------------------------------

extern "C" JNIEXPORT jboolean JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeStart(JNIEnv* env, jclass, jstring binPath,
                                                     jstring gameDir, jint renderW, jint renderH,
                                                     jint capFps) {
    install_crash_handlers();

    const char* bin = env->GetStringUTFChars(binPath, nullptr);
    const char* dir = env->GetStringUTFChars(gameDir, nullptr);

    // Environment the launch script would normally provide (see port/FlatOut 2.sh):
    // FO2_RENDER sizes the frame, FO2_CAP limits the frame rate.
    char render[64];
    snprintf(render, sizeof(render), "%dx%d", renderW > 0 ? renderW : 640, renderH > 0 ? renderH : 480);
    setenv("FO2_RENDER", render, 1);
    char cap[32];
    snprintf(cap, sizeof(cap), "%d", capFps > 0 ? capFps : 60);
    setenv("FO2_CAP", cap, 1);
    setenv("NFSU2_SHOW_FPS", "0", 1);
    setenv("HOME", dir, 1);
    setenv("LC_ALL", "C", 1);
    chdir(dir);  // gamedata/ lives here, exactly like the PortMaster launch script

    bool ok = guest_start(bin, dir, renderW, renderH, capFps);
    if (!ok) loge("guest_start failed (see earlier log lines)");

    env->ReleaseStringUTFChars(binPath, bin);
    env->ReleaseStringUTFChars(gameDir, dir);
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeSetSurface(JNIEnv* env, jclass, jobject surface) {
    if (surface == nullptr) {
        video_set_surface(nullptr);
        return;
    }
    ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
    if (win != nullptr) {
        logi("native surface: %dx%d", ANativeWindow_getWidth(win), ANativeWindow_getHeight(win));
    }
    video_set_surface(win);
}

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeClearSurface(JNIEnv*, jclass) {
    video_clear_surface();
}

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeSetPaused(JNIEnv*, jclass, jboolean paused) {
    video_set_paused(paused == JNI_TRUE);
}

// ---- input injection ---------------------------------------------------------------

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeInjectAxis(JNIEnv*, jclass, jint axis, jfloat value) {
    input_set_axis(axis, value);
}

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeInjectButton(JNIEnv*, jclass, jint button, jboolean down) {
    input_set_button(button, down == JNI_TRUE);
}

// Guest exit callback: called on the guest main thread (guest TPIDR active!).
// Uses the cached class/method so no FindClass is needed from a native thread.
static jclass g_exit_cls = nullptr;         // global ref
static jmethodID g_exit_mid = nullptr;

static void on_guest_exit_trampoline(int code) {
    BionicGuard g;  // guest TPIDR active here; swap before touching the VM
    JavaVM* vm = g_vm;
    JNIEnv* env = nullptr;
    if (vm == nullptr || g_exit_cls == nullptr || g_exit_mid == nullptr) return;
    bool attached = false;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    if (env != nullptr) {
        env->CallStaticVoidMethod(g_exit_cls, g_exit_mid, (jint)code);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (attached) vm->DetachCurrentThread();
}

extern "C" JNIEXPORT void JNICALL
Java_com_deivid22srk_flatout2_GameBridge_nativeSetExitCallback(JNIEnv* env, jclass cls) {
    install_crash_handlers();
    if (g_exit_cls == nullptr) {
        jclass local = (jclass)env->NewGlobalRef(cls);
        g_exit_cls = local;
        g_exit_mid = env->GetStaticMethodID(cls, "onGuestExited", "(I)V");
    }
    g_on_guest_exit = on_guest_exit_trampoline;
}
