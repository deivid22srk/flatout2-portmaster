// bridge/fo2_bridge.cpp — JNI surface between the app and the guest runtime.
#include "../core/guest_thread.h"
#include "../core/shim_common.h"
#include "../shim/sdl_state.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

namespace {

struct CrashModule {
    uint64_t start, end, offset;   // exec mapping: [start,end) file offset `offset`
    char name[96];
};

constexpr int kMaxCrashModules = 160;
CrashModule g_crash_modules[kMaxCrashModules];
int g_crash_module_count = 0;

// Snapshots the executable mappings of this process. Called from a normal
// thread when handlers are installed, and re-called from the crash handler
// when a pc is not yet covered (e.g. the guest image mapped after install).
void snapshot_crash_modules() {
    FILE* f = fopen("/proc/self/maps", "re");
    if (f == nullptr) return;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) != nullptr && n < kMaxCrashModules) {
        uint64_t s = 0, e = 0;
        char perms[8] = {0};
        unsigned long long off = 0;
        unsigned int devmaj = 0, devmin = 0;
        unsigned long long ino = 0;
        int consumed = 0;
        if (sscanf(line, "%lx-%lx %7s %llx %x:%x %llx %n",
                   &s, &e, perms, &off, &devmaj, &devmin, &ino, &consumed) < 7) continue;
        if (perms[2] != 'x') continue;                       // keep executable ranges
        const char* p = line + consumed;
        while (*p == ' ' || *p == '\t') ++p;
        char* nl = strchr(line, '\n');
        if (nl != nullptr) *nl = '\0';
        if (*p == '\0') continue;                            // skip anonymous code (JIT)
        CrashModule& m = g_crash_modules[n];
        m.start = s; m.end = e; m.offset = off;
        strncpy(m.name, p, sizeof(m.name) - 1);
        m.name[sizeof(m.name) - 1] = '\0';
        ++n;
    }
    fclose(f);
    g_crash_module_count = n;
}

// "name+rel (file_off abs)" for a code address, or nullptr when not resolved.
const char* describe_crash_pc(uint64_t pc, uint64_t* rel_out, bool refresh) {
    for (int i = 0; i < g_crash_module_count; ++i) {
        const CrashModule& m = g_crash_modules[i];
        if (pc >= m.start && pc < m.end) {
            uint64_t rel = pc - m.start + m.offset;
            static char buf[160];
            snprintf(buf, sizeof(buf), "%s+0x%llx", m.name, (unsigned long long)rel);
            if (rel_out) *rel_out = rel;
            return buf;
        }
    }
    if (refresh) {
        snapshot_crash_modules();
        return describe_crash_pc(pc, rel_out, false);
    }
    if (rel_out) *rel_out = 0;
    return nullptr;
}

bool is_code_addr(uint64_t v) {
    for (int i = 0; i < g_crash_module_count; ++i)
        if (v >= g_crash_modules[i].start && v < g_crash_modules[i].end) return true;
    return false;
}

} // namespace

static void crash_handler(int sig, siginfo_t* info, void* ctx) {
    // May run on a guest thread (TPIDR = guest TCB): swap before bionic calls.
    BionicGuard g;
    uint64_t pc = 0, lr = 0, sp = 0;
    if (ctx != nullptr) {
        ucontext_t* uc = (ucontext_t*)ctx;
        pc = uc->uc_mcontext.pc;
        lr = uc->uc_mcontext.regs[30];
        sp = uc->uc_mcontext.sp;
    }

    // Resolve against the snapshot; a guest-image pc needs a fresh snapshot.
    const char* where = describe_crash_pc(pc, nullptr, false);
    if (where == nullptr) where = describe_crash_pc(pc, nullptr, true);

    __android_log_print(ANDROID_LOG_ERROR, "FO2",
                        "CRASH signal=%d addr=%p pc=0x%llx (%s)", sig,
                        info ? info->si_addr : nullptr, (unsigned long long)pc,
                        where ? where : "unknown module");

    // Cheap unwinder: LR plus code pointers found on the stack below sp.
    char line[256];
    const char* d = describe_crash_pc(lr, nullptr, false);
    if (d != nullptr) {
        snprintf(line, sizeof(line), "CRASH bt: lr=0x%llx %s",
                 (unsigned long long)lr, d);
        __android_log_print(ANDROID_LOG_ERROR, "FO2", "%s", line);
    }
    if (sp != 0) {
        int shown = 0;
        for (uint64_t a = sp; a + 8 <= sp + 0x8000 && shown < 12; a += 8) {
            uint64_t v;
            memcpy(&v, (const void*)(uintptr_t)a, sizeof(v));  // crash path: best effort
            if ((v & 3) != 0 || !is_code_addr(v)) continue;
            d = describe_crash_pc(v, nullptr, false);
            if (d == nullptr) continue;
            snprintf(line, sizeof(line), "CRASH bt: [%llx] %llx %s",
                     (unsigned long long)a, (unsigned long long)v, d);
            __android_log_print(ANDROID_LOG_ERROR, "FO2", "%s", line);
            ++shown;
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers() {
    snapshot_crash_modules();
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
