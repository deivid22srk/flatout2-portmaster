package com.deivid22srk.flatout2;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.File;
import java.io.RandomAccessFile;

/**
 * Hosts the game: a SurfaceView the guest renders into, the touch overlay and
 * physical gamepad forwarding. Landscape + immersive fullscreen.
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback {

    private SurfaceView surface;
    private TouchControlsView controls;
    private boolean started = false;
    private boolean surfaceReady = false;
    private int renderW = 640, renderH = 480;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout root = new FrameLayout(this);
        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        controls = new TouchControlsView(this);
        root.addView(controls, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        Button toggle = new Button(this);
        toggle.setText("\u2630");
        toggle.setBackgroundColor(0x66000000);
        toggle.setGravity(Gravity.CENTER);
        toggle.setOnClickListener(v -> {
            SharedPreferences sp = getPreferences(Context.MODE_PRIVATE);
            boolean show = !sp.getBoolean("controls", true);
            sp.edit().putBoolean("controls", show).apply();
            controls.setVisibility(show ? View.VISIBLE : View.GONE);
            Toast.makeText(this, R.string.toggle_controls, Toast.LENGTH_SHORT).show();
        });
        FrameLayout.LayoutParams tp = new FrameLayout.LayoutParams(
                dp(56), dp(40), Gravity.TOP | Gravity.END);
        tp.rightMargin = dp(6);
        tp.topMargin = dp(6);
        root.addView(toggle, tp);

        setContentView(root);
        hideSystemUi();
        controls.setVisibility(getPreferences(Context.MODE_PRIVATE)
                .getBoolean("controls", true) ? View.VISIBLE : View.GONE);

        computeRenderSize();
        applyConsoleLaunchSettings();
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private void hideSystemUi() {
        View d = getWindow().getDecorView();
        d.setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    /** Like the PortMaster script: render follows the screen with at most 480 lines. */
    private void computeRenderSize() {
        int sw = getResources().getDisplayMetrics().widthPixels;
        int sh = getResources().getDisplayMetrics().heightPixels;
        if (sw < sh) { int t = sw; sw = sh; sh = t; }
        if (sh > 480) {
            sw = sw * 480 / sh;
            sh = 480;
        }
        renderW = Math.max(320, sw / 2 * 2);
        renderH = Math.max(240, sh / 2 * 2);
    }

    /**
     * Same device.cfg/options.cfg adjustments the launch script does: the render
     * mode in device.cfg (bytes 120/124/128) and lowest visual settings.
     */
    private void applyConsoleLaunchSettings() {
        try {
            File device = new File(getExternalFilesDir(null),
                    "gamedata/Savegame/device.cfg");
            if (device.exists() && device.length() >= 132) {
                try (RandomAccessFile raf = new RandomAccessFile(device, "rw")) {
                    putdw(raf, 120, renderW);
                    putdw(raf, 124, renderH);
                    putdw(raf, 128, 32);
                }
            }
            File options = new File(getExternalFilesDir(null),
                    "gamedata/Savegame/options.cfg");
            if (options.exists()) {
                java.nio.file.Path p = options.toPath();
                String s = new String(java.nio.file.Files.readAllBytes(p));
                s = s.replaceAll("(?m)^(Settings\\.Visual\\.TextureQuality = )\\d+", "$10");
                s = s.replaceAll("(?m)^(Settings\\.Visual\\.TrackDetail = )\\d+", "$10");
                s = s.replaceAll("(?m)^(Settings\\.Visual\\.Visibility = )\\d+", "$10");
                s = s.replaceAll("(?m)^(Settings\\.Visual\\.PS2MotionBlurIngame = )\\d+", "$10");
                s = s.replaceAll("(?m)^(Settings\\.Visual\\.SunFlare = )\\w+", "$1FALSE");
                java.nio.file.Files.write(p, s.getBytes());
            }
        } catch (Exception e) {
            // Non-fatal: first run may not have Savegame files yet.
        }
    }

    private static void putdw(RandomAccessFile raf, long at, int v) throws java.io.IOException {
        raf.seek(at);
        raf.write(v & 0xFF);
        raf.write((v >> 8) & 0xFF);
        raf.write((v >> 16) & 0xFF);
        raf.write((v >> 24) & 0xFF);
    }

    // ---- Surface lifecycle ------------------------------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        GameBridge.nativeSetSurface(holder.getSurface());
        surfaceReady = true;
        if (!started) {
            started = true;
            GameBridge.setExitListener(code -> {
                if (!isFinishing()) finish();
            });
            String bin = BootActivity.binaryFile(this).getAbsolutePath();
            String gameDir = new File(getExternalFilesDir(null), "gamedata").getParentFile()
                    .getAbsolutePath();
            SharedPreferences sp = getPreferences(Context.MODE_PRIVATE);
            int cap = sp.getBoolean("cap30", false) ? 30 : 60;
            boolean ok = GameBridge.nativeStart(bin, gameDir, renderW, renderH, cap);
            if (!ok) {
                Toast.makeText(this, "Falha ao iniciar o hóspede (veja adb logcat -s FO2)",
                        Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        GameBridge.nativeSetSurface(holder.getSurface());
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        GameBridge.nativeClearSurface();
    }

    @Override
    protected void onPause() {
        super.onPause();
        GameBridge.nativeSetPaused(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUi();
        GameBridge.nativeSetPaused(false);
    }

    // ---- physical gamepad --------------------------------------------------------------

    private static int mapKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: return 0;   // south
            case KeyEvent.KEYCODE_BUTTON_B: return 1;   // east
            case KeyEvent.KEYCODE_BUTTON_X: return 2;   // west
            case KeyEvent.KEYCODE_BUTTON_Y: return 3;   // north
            case KeyEvent.KEYCODE_BACK: return 4;
            case KeyEvent.KEYCODE_BUTTON_START: return 6;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 7;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 8;
            case KeyEvent.KEYCODE_BUTTON_L1: return 9;
            case KeyEvent.KEYCODE_BUTTON_R1: return 10;
            case KeyEvent.KEYCODE_DPAD_UP: return 11;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 12;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 13;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 14;
            default: return -1;
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        int b = mapKey(keyCode);
        if (b >= 0) {
            GameBridge.nativeInjectButton(b, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        int b = mapKey(keyCode);
        if (b >= 0) {
            GameBridge.nativeInjectButton(b, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & android.view.InputDevice.SOURCE_CLASS_JOYSTICK) != 0
                && event.getAction() == MotionEvent.ACTION_MOVE) {
            GameBridge.nativeInjectAxis(0, event.getAxisValue(MotionEvent.AXIS_X));
            GameBridge.nativeInjectAxis(1, event.getAxisValue(MotionEvent.AXIS_Y));
            GameBridge.nativeInjectAxis(2, event.getAxisValue(MotionEvent.AXIS_Z));
            GameBridge.nativeInjectAxis(3, event.getAxisValue(MotionEvent.AXIS_RZ));
            float lt = event.getAxisValue(MotionEvent.AXIS_LTRIGGER);
            if (lt == 0f) lt = event.getAxisValue(MotionEvent.AXIS_BRAKE);
            float rt = event.getAxisValue(MotionEvent.AXIS_RTRIGGER);
            if (rt == 0f) rt = event.getAxisValue(MotionEvent.AXIS_GAS);
            GameBridge.nativeInjectAxis(4, lt);
            GameBridge.nativeInjectAxis(5, rt);
            return true;
        }
        return super.onGenericMotionEvent(event);
    }
}
