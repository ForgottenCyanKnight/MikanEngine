package com.mikanengine;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;
import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.content.Context;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.RelativeLayout;

public class MikanEngineActivity extends SDLActivity {
    private static final float REQUESTED_FRAME_RATE = 120.0f;
    private static final long FRAME_RATE_REQUEST_INTERVAL_MS = 500L;
    private static final int FRAME_RATE_REQUEST_ATTEMPTS = 12;
    private final Handler frameRateHandler = new Handler(Looper.getMainLooper());
    private boolean frameRateRequestActive = false;
    private int frameRateRequestAttempts = 0;
    private boolean displayModeRequestLogged = false;
    private boolean surfaceFrameRateRequestLogged = false;
    private ImageView nativeSplashView;
    private boolean nativeSplashHidden = false;
    private final Runnable frameRateRequest = new Runnable() {
        @Override
        public void run() {
            if (!frameRateRequestActive) {
                return;
            }
            requestPreferredDisplayMode();
            requestPreferredFrameRateFromSurface();
            frameRateRequestAttempts++;
            if (frameRateRequestAttempts < FRAME_RATE_REQUEST_ATTEMPTS) {
                frameRateHandler.postDelayed(this, FRAME_RATE_REQUEST_INTERVAL_MS);
            }
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "mikanengine"
        };
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = super.createSDLSurface(context);
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                requestPreferredFrameRate(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                requestPreferredFrameRate(holder.getSurface());
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                // SDL owns the surface lifecycle; there is nothing to release here.
            }
        });
        return surface;
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installNativeSplashOverlay();
    }

    private void installNativeSplashOverlay() {
        if (mLayout == null) {
            return;
        }

        nativeSplashView = new ImageView(this);
        nativeSplashView.setImageResource(R.drawable.mikan_engine_splash);
        // 与 Vulkan 开屏保持一致：横屏使用 cover，裁掉上下留白，避免左右出现
        // 与 Logo 图片米白渐变不一致的纯色边带。
        nativeSplashView.setScaleType(ImageView.ScaleType.CENTER_CROP);
        nativeSplashView.setBackgroundColor(Color.rgb(252, 251, 240));
        nativeSplashView.setContentDescription("Mikan Engine");

        RelativeLayout.LayoutParams params = new RelativeLayout.LayoutParams(
            RelativeLayout.LayoutParams.MATCH_PARENT,
            RelativeLayout.LayoutParams.MATCH_PARENT);
        mLayout.addView(nativeSplashView, params);
        nativeSplashView.bringToFront();
    }

    // Called from the native SDL thread immediately before the Vulkan Splash is rendered.
    public void hideNativeSplash() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (nativeSplashHidden || nativeSplashView == null) {
                    return;
                }
                nativeSplashHidden = true;
                nativeSplashView.setVisibility(View.GONE);
            }
        });
    }

    @Override
    protected void onPostCreate(android.os.Bundle savedInstanceState) {
        super.onPostCreate(savedInstanceState);
        hideSystemUI();
        requestPreferredDisplayMode();
        requestPreferredFrameRateFromSurface();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUI();
            requestPreferredDisplayMode();
            requestPreferredFrameRateFromSurface();
            frameRateRequestActive = true;
            frameRateRequestAttempts = 0;
            frameRateHandler.removeCallbacks(frameRateRequest);
            frameRateHandler.post(frameRateRequest);
        } else {
            frameRateRequestActive = false;
            frameRateHandler.removeCallbacks(frameRateRequest);
        }
    }

    private void requestPreferredDisplayMode() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return;
        }

        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode bestMode = null;
        for (Display.Mode mode : display.getSupportedModes()) {
            if (Math.abs(mode.getRefreshRate() - REQUESTED_FRAME_RATE) < 0.5f &&
                (bestMode == null || mode.getModeId() < bestMode.getModeId())) {
                bestMode = mode;
            }
        }

        if (bestMode != null) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            if (attributes.preferredDisplayModeId != bestMode.getModeId() ||
                display.getMode().getModeId() != bestMode.getModeId()) {
                attributes.preferredDisplayModeId = bestMode.getModeId();
                getWindow().setAttributes(attributes);
                if (!displayModeRequestLogged) {
                    Log.i("MikanEngine", "Requested Android display mode: " +
                        bestMode.getModeId() + " @ " + bestMode.getRefreshRate() + "Hz");
                    displayModeRequestLogged = true;
                }
            }
        }
    }

    private void requestPreferredFrameRateFromSurface() {
        if (mSurface != null) {
            requestPreferredFrameRate(mSurface.getHolder().getSurface());
        }
    }

    private void requestPreferredFrameRate(Surface surface) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            surface != null && surface.isValid()) {
            // Fixed-source keeps Android's display policy from falling back to 60Hz
            // when the device supports a 120Hz mode.
            surface.setFrameRate(
                REQUESTED_FRAME_RATE,
                Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
            if (!surfaceFrameRateRequestLogged) {
                Log.i("MikanEngine", "Requested Android surface frame rate: 120Hz");
                surfaceFrameRateRequestLogged = true;
            }
        }
    }

    private void hideSystemUI() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            );
        }
    }
}
