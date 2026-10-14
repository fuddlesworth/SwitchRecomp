// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import org.libsdl.app.SDLActivity;

/** The menu: scarletnx-gui, as libmain.so */
public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (!Environment.isExternalStorageManager())
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
    }

    /** src/gui/android.c: the runtime with these arguments, in the game activity's process */
    public void play(String[] args) {
        startActivity(new Intent(this, GameActivity.class).putExtra("args", args));
    }

    /** src/gui/android.c: the logs, as text, to the app the user picks */
    public void share(String text) {
        Intent send = new Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, text);
        startActivity(Intent.createChooser(send, "ScarletNX log"));
    }
}
