// SPDX-License-Identifier: MIT
package io.github.gamerecomp.tropico1recomp;

import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

/**
 * SDL's activity with the libraries of the game shell, always on the whole
 * screen. SDL's own starts with the system bars shown, and some devices show
 * them again after the app was left or a dialog took the focus, so the bars
 * are hidden at the start and whenever the app comes back. (SDL's
 * setWindowStyle would do the same but waits for the new size, which on this,
 * the UI thread, cannot come: the window is made full screen from the game
 * too, where waiting is right.)
 */
public class TropicoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "SDL3_ttf", "SDL3_mixer", "main"};
    }

    /** The system bars hidden, the picture under them and the camera's cutout. */
    private void wholeScreen() {
        Window window = getWindow();
        if (window == null) {
            return;
        }
        window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        window.addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.clearFlags(WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN);
        if (Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams a = window.getAttributes();
            a.layoutInDisplayCutoutMode = Build.VERSION.SDK_INT >= 30
                    ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                    : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            window.setAttributes(a);
        }
        // SDL hides the bars again when they come back while this is set.
        mFullscreenModeActive = true;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        wholeScreen();
    }

    @Override
    protected void onResume() {
        super.onResume();
        wholeScreen();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            wholeScreen();
        }
    }
}
