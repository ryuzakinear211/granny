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

// ============================================================================
// EspView - Overlay transparan untuk menggambar ESP (line + nametag).
// Ditambah via WindowManager (MATCH_PARENT), tidak menangkap sentuhan.
// Diupdate dari native (GrannyESP.cpp) lewat:
//   showEsp(Context) -> tampilkan overlay
//   hideEsp()        -> sembunyikan overlay
//   updateEsp(x, y, name) -> update posisi + invalidate (redraw).
// Koordinat x,y berasal dari Camera.WorldToScreenPoint (origin kiri-BAWAH),
// jadi di onDraw dikonversi ke koordinat Android (origin kiri-ATAS).
// ============================================================================
public class EspView extends View {

    private static EspView instance;
    private static WindowManager espWM;
    private static final Handler espUiHandler = new Handler(Looper.getMainLooper());

    // Data ESP (satu entity: Granny). volatile karena ditulis dari UI thread
    // via Handler dan dibaca di onDraw (UI thread juga) — aman.
    private volatile boolean hasData = false;
    private volatile float espX = -1;
    private volatile float espY = -1;
    private volatile String espName = null;

    private final Paint linePaint = new Paint();
    private final Paint textPaint = new Paint();
    private final Paint dotPaint = new Paint();

    public EspView(Context context) {
        super(context);
        setWillNotDraw(false);

        linePaint.setColor(Color.parseColor("#FF5252")); // merah
        linePaint.setStrokeWidth(4.0f);
        linePaint.setAntiAlias(true);

        textPaint.setColor(Color.parseColor("#FFFFFF")); // putih
        textPaint.setTextSize(42.0f);
        textPaint.setAntiAlias(true);
        textPaint.setTextAlign(Paint.Align.CENTER);
        // outline agar terbaca di atas game
        textPaint.setShadowLayer(6.0f, 0, 0, Color.parseColor("#CC000000"));

        dotPaint.setColor(Color.parseColor("#FF5252"));
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

    // Dipanggil dari native saat toggle ESP OFF
    public static void hideEsp() {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance != null) {
                    instance.hasData = false;
                    instance.setVisibility(View.GONE);
                }
            }
        });
    }

    // Dipanggil dari native tiap ~100 ms. x<0 atau name==null = sembunyikan.
    public static void updateEsp(final float x, final float y, final String name) {
        espUiHandler.post(new Runnable() {
            @Override
            public void run() {
                if (instance == null) return;
                if (x < 0 || name == null) {
                    instance.hasData = false;
                } else {
                    instance.espX = x;
                    instance.espY = y;
                    instance.espName = name;
                    instance.hasData = true;
                }
                instance.invalidate(); // picu onDraw
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
        if (!hasData || espName == null) return;

        int w = getWidth();
        int h = getHeight();
        float sx = espX;
        float sy = h - espY; // Unity origin kiri-bawah -> Android kiri-atas

        // Jangan gambar di luar layar
        if (sx < -100 || sx > w + 100 || sy < -100 || sy > h + 100) return;

        // 1. Garis dari tengah-atas layar ke NPC
        canvas.drawLine(w / 2.0f, 0, sx, sy, linePaint);
        // 2. Titik di posisi NPC
        canvas.drawCircle(sx, sy, 10.0f, dotPaint);
        // 3. Nametag di atas titik
        canvas.drawText(espName, sx, sy - 36.0f, textPaint);
    }
}
