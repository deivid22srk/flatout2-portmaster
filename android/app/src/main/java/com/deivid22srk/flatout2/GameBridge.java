package com.deivid22srk.flatout2;

import android.view.Surface;

/**
 * JNI surface of the guest runtime (libfo2rt.so).
 *
 * The native side loads fo2.aarch64 (the recompiled FlatOut 2 ARM64 binary)
 * into this process, satisfies its glibc/SDL2 imports through shims, and
 * renders into the Surface handed over by GameActivity.
 */
public final class GameBridge {

    static {
        System.loadLibrary("fo2rt");
    }

    /** Registers the native crash handlers and the guest-exit callback. */
    public static native void nativeSetExitCallback();

    /** Loads and starts the guest binary. Call after the Surface exists. */
    public static native boolean nativeStart(String binPath, String gameDir,
                                             int renderW, int renderH, int capFps);

    public static native void nativeSetSurface(Surface surface);
    public static native void nativeClearSurface();
    public static native void nativeSetPaused(boolean paused);

    // axis: 0..5 = LEFTX, LEFTY, RIGHTX, RIGHTY, TRIGGERLEFT, TRIGGERRIGHT
    public static native void nativeInjectAxis(int axis, float value);
    // button: SDL_GameControllerButton codes (0=A .. 14=DPAD_RIGHT)
    public static native void nativeInjectButton(int button, boolean down);

    /** Called from native when the guest main() returns. */
    @SuppressWarnings("unused")
    static void onGuestExited(int code) {
        ExitListener l = sExitListener;
        if (l != null) l.onGuestExited(code);
    }

    public interface ExitListener {
        void onGuestExited(int code);
    }

    private static volatile ExitListener sExitListener;

    public static void setExitListener(ExitListener l) {
        sExitListener = l;
    }

    private GameBridge() {}
}
