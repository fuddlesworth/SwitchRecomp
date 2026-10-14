// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import org.libsdl.app.SDLActivity;

/** A game: the runtime's main in libscarletnx.so, with the menu's arguments */
public class GameActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"scarletnx"};
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected String[] getArguments() {
        String[] args = getIntent().getStringArrayExtra("args");
        return args != null ? args : new String[0];
    }

    /** the runtime starts once per process: the next game gets a new one */
    @Override
    protected void onDestroy() {
        super.onDestroy();
        System.exit(0);
    }
}
