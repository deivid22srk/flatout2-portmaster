package com.deivid22srk.flatout2;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;
import java.util.Map;

/**
 * Translucent touch controls over the game surface:
 *   - left region: steering (horizontal drag)
 *   - right: accelerate, brake, handbrake, nitro
 *   - top right cluster: reset, camera, pause, shift up/down
 * The whole overlay can be hidden from GameActivity (gamepad play).
 *
 * Buttons map to SDL_GameControllerButton codes fed through GameBridge.
 */
public class TouchControlsView extends View {

    // SDL_GameControllerButton values (match the native shim)
    private static final int BTN_A = 0;        // south: accelerate / confirm
    private static final int BTN_B = 1;        // east: handbrake
    private static final int BTN_X = 2;        // west: brake / reverse
    private static final int BTN_Y = 3;        // north: reset car
    private static final int BTN_BACK = 4;     // select: camera
    private static final int BTN_START = 6;    // pause
    private static final int BTN_R1 = 10;      // right shoulder: nitro
    private static final int AXIS_LT = 4;      // shift down (L2)
    private static final int AXIS_RT = 5;      // shift up (R2)

    private static final class Btn {
        final String label;
        final float cx, cy, r;    // center (fraction of view) + radius (dp)
        final int sdlButton;      // -1 when it drives an axis
        final int sdlAxis;        // -1 when it drives a button

        Btn(String label, float cx, float cy, float r, int sdlButton, int sdlAxis) {
            this.label = label;
            this.cx = cx;
            this.cy = cy;
            this.r = r;
            this.sdlButton = sdlButton;
            this.sdlAxis = sdlAxis;
        }
    }

    // Right-side pedal cluster + small top cluster (fractions of the view size).
    private static final Btn[] BTNS = {
            new Btn("ACEL", 0.845f, 0.72f, 52f, BTN_A, -1),
            new Btn("FREIO", 0.685f, 0.80f, 44f, BTN_X, -1),
            new Btn("FREIO M\u00C3O", 0.685f, 0.58f, 36f, BTN_B, -1),
            new Btn("NITRO", 0.845f, 0.42f, 36f, BTN_R1, -1),
            new Btn("RESET", 0.955f, 0.16f, 28f, BTN_Y, -1),
            new Btn("C\u00C2M", 0.80f, 0.14f, 26f, BTN_BACK, -1),
            new Btn("PAUSA", 0.655f, 0.13f, 26f, BTN_START, -1),
            new Btn("M+", 0.545f, 0.20f, 24f, -1, AXIS_RT),   // shift up (R2)
            new Btn("M-", 0.545f, 0.34f, 24f, -1, AXIS_LT),   // shift down (L2)
    };

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    // pointerId -> pressed button index (or -1 for steering pointer)
    private final Map<Integer, Integer> active = new HashMap<>();
    private float steering = 0f;   // -1..1

    public TouchControlsView(Context ctx) {
        super(ctx);
        fill.setColor(Color.argb(70, 255, 255, 255));
        fill.setStyle(Paint.Style.FILL);
        stroke.setColor(Color.argb(140, 210, 74, 30));
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(3f);
        text.setColor(Color.argb(200, 255, 255, 255));
        text.setTextAlign(Paint.Align.CENTER);
        setHapticFeedbackEnabled(false);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        float w = getWidth(), h = getHeight();
        float density = getResources().getDisplayMetrics().density;

        // steering zone
        RectF zone = new RectF(0, h * 0.45f, w * 0.42f, h);
        fill.setAlpha(46);
        c.drawRoundRect(zone, 24, 24, fill);
        stroke.setAlpha(120);
        c.drawRoundRect(zone, 24, 24, stroke);
        float cx = zone.centerX() + steering * zone.width() * 0.35f;
        fill.setAlpha(120);
        c.drawCircle(cx, zone.centerY(), 34 * density, fill);
        text.setTextSize(13 * density);
        c.drawText("\u2194", cx, zone.centerY() + 5 * density, text);

        for (int i = 0; i < BTNS.length; i++) {
            Btn b = BTNS[i];
            float bx = b.cx * w, by = b.cy * h, br = b.r * density;
            boolean down = active.containsValue(i);
            fill.setAlpha(down ? 150 : 60);
            c.drawCircle(bx, by, br, fill);
            stroke.setAlpha(down ? 220 : 140);
            c.drawCircle(bx, by, br, stroke);
            text.setTextSize(Math.max(11f, br * 0.34f));
            text.setAlpha(down ? 255 : 190);
            c.drawText(b.label, bx, by + text.getTextSize() * 0.35f, text);
        }
    }

    private int hitButton(float x, float y) {
        float w = getWidth(), h = getHeight();
        float density = getResources().getDisplayMetrics().density;
        for (int i = BTNS.length - 1; i >= 0; i--) {
            Btn b = BTNS[i];
            float dx = x - b.cx * w, dy = y - b.cy * h;
            float r = b.r * density + 12 * density;
            if (dx * dx + dy * dy <= r * r) return i;
        }
        return -1;
    }

    private boolean inSteerZone(float x, float y) {
        return x < getWidth() * 0.42f && y > getHeight() * 0.45f;
    }

    private void press(int idx, boolean down) {
        Btn b = BTNS[idx];
        if (b.sdlButton >= 0) {
            GameBridge.nativeInjectButton(b.sdlButton, down);
        } else {
            GameBridge.nativeInjectAxis(b.sdlAxis, down ? 1f : 0f);
        }
        if (down && b.sdlButton == BTN_START) {
            // nothing extra: pause is handled by the game itself
        }
        invalidate();
    }

    private void setSteering(float fraction) {
        steering = Math.max(-1f, Math.min(1f, fraction));
        GameBridge.nativeInjectAxis(0, steering); // LEFTX
        invalidate();
    }

    @Override
    public boolean onTouchEvent(MotionEvent ev) {
        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int pi = ev.getActionIndex();
                int id = ev.getPointerId(pi);
                float x = ev.getX(pi), y = ev.getY(pi);
                int idx = hitButton(x, y);
                if (idx >= 0) {
                    active.put(id, idx);
                    press(idx, true);
                } else if (inSteerZone(x, y)) {
                    active.put(id, -1);
                    setSteering((x - getWidth() * 0.21f) / (getWidth() * 0.19f));
                } else {
                    active.put(id, -2); // ignored pointer
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < ev.getPointerCount(); i++) {
                    int id = ev.getPointerId(i);
                    Integer role = active.get(id);
                    if (role == null) continue;
                    if (role == -1) {
                        setSteering((ev.getX(i) - getWidth() * 0.21f) / (getWidth() * 0.19f));
                    }
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = ev.getPointerId(ev.getActionIndex());
                Integer role = active.remove(id);
                if (role != null) {
                    if (role >= 0) press(role, false);
                    else if (role == -1) setSteering(0f);
                }
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                for (Integer role : active.values()) {
                    if (role >= 0) press(role, false);
                    else if (role == -1) setSteering(0f);
                }
                active.clear();
                return true;
            }
            default:
                return super.onTouchEvent(ev);
        }
    }
}
