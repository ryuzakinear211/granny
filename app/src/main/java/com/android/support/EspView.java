package com.android.support;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;

import java.util.ArrayList;
import java.util.List;

// ============================================================================
// EspView - Overlay transparan untuk menggambar ESP.
// Mendukung BANYAK titik sekaligus (untuk diagnosis: tiap kandidat transform
// digambar dengan label & warna sendiri, mis. "T0", "78", "130"...).
//   showEsp(Context)
//   hideEsp()
//   updateEsp(x, y, name)                        -> 1 titik merah (ESP utama)
//   updateEspMulti(xs[], ys[], names[], colors[]) -> N titik (mode debug)
// Koordinat x,y berasal dari Camera.WorldToScreenPoint (origin kiri-BAWAH),
// jadi di onDraw dikonversi ke koordinat Android (origin kiri-ATAS).
// ============================================================================
public class EspView extends View {

    private static EspView instance;
    private static WindowManager espWM;
    private static final Handler espUiHandler = new Handler(Looper.getMainLooper());

    private static class EspItem {
        float x, y;
        String name;
        int color;
    }

    // Daftar titik digambar; hanya diakses dari UI thread via Handler.
    private final List<EspItem> items = new ArrayList<EspItem>();

    private final Paint linePaint = new Paint();
    private final Paint textPaint = new Paint();
    private final Paint dotPaint = new Paint();

    public EspView(Context context) {
        super(context);
        setWillNotDraw(false);

        linePaint.setStrokeWidth(4.0f);
        linePaint.setAntiAlias(true);

        textPaint.setTextSize(42.0f);
        textPaint.setAntiAlias(true);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setShadowLayer(6.0f, 0, 0, Color.parseColor("#CC000000"));

        dotPaint.setAntiAlias(true);
    }

    // Dipanggil dari native saat toggle ESP ON
    public static void showEsp(final Context ctx) {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance != null) {
                    instance.setVisibility(View.VISIBLE);
                    return;
                }
                instance = new EspView(ctx);
                int type = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O ? 2038 : 2002;
                WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                        WindowManager.LayoutParams.MATCH_PARENT,
                        WindowManager.LayoutParams.MATCH_PARENT,
                        type,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                                WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE |
                                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                        PixelFormat.TRANSLUCENT);
                espWM = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
                espWM.addView(instance, params);
            }
        });
    }

    // Dipanggil dari native saat toggle ESP OFF (dan debug OFF)
    public static void hideEsp() {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance != null) {
                    instance.items.clear();
                    instance.setVisibility(View.GONE);
                }
            }
        });
    }

    // ESP utama: 1 titik merah. x<0 atau name==null = kosongkan.
    public static void updateEsp(final float x, final float y, final String name) {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance == null) return;
                instance.items.clear();
                if (x >= 0 && name != null) {
                    EspItem it = new EspItem();
                    it.x = x; it.y = y; it.name = name;
                    it.color = Color.parseColor("#FF5252");
                    instance.items.add(it);
                }
                instance.invalidate();
            }
        });
    }

    // Mode debug: N titik berlabel + berwarna.
    public static void updateEspMulti(final float[] xs, final float[] ys,
                                      final String[] names, final int[] colors) {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance == null) return;
                instance.items.clear();
                if (xs != null && ys != null) {
                    int n = Math.min(xs.length, ys.length);
                    for (int i = 0; i < n; i++) {
                        EspItem it = new EspItem();
                        it.x = xs[i]; it.y = ys[i];
                        it.name = (names != null && i < names.length && names[i] != null)
                                ? names[i] : "";
                        it.color = (colors != null && i < colors.length)
                                ? colors[i] : Color.parseColor("#FF5252");
                        instance.items.add(it);
                    }
                }
                instance.invalidate();
            }
        });
    }

    // Bersihkan overlay sepenuhnya (dipanggil dari Menu.onDestroy)
    public static void destroyEsp() {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance != null && espWM != null) {
                    try { espWM.removeView(instance); } catch (Exception ignored) {}
                    instance = null;
                }
            }
        });
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        int w = getWidth();
        int h = getHeight();

        for (int i = 0; i < items.size(); i++) {
            EspItem it = items.get(i);
            float sx = it.x;
            float sy = h - it.y; // Unity origin kiri-bawah -> Android kiri-atas

            // Jangan gambar di luar layar
            if (sx < -100 || sx > w + 100 || sy < -100 || sy > h + 100) continue;

            linePaint.setColor(it.color);
            dotPaint.setColor(it.color);

            // 1. Garis dari tengah-atas layar ke titik
            canvas.drawLine(w / 2.0f, 0, sx, sy, linePaint);
            // 2. Titik
            canvas.drawCircle(sx, sy, 10.0f, dotPaint);
            // 3. Label di atas titik
            if (it.name != null && it.name.length() > 0) {
                canvas.drawText(it.name, sx, sy - 36.0f, textPaint);
            }
        }
    }
}
