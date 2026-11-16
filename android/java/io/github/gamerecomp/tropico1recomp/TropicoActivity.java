// SPDX-License-Identifier: MIT
package io.github.gamerecomp.tropico1recomp;

import org.libsdl.app.SDLActivity;

/** SDL's activity with the libraries of the game shell. */
public class TropicoActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "SDL3_ttf", "SDL3_mixer", "main"};
    }
}
