/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void gui_android_start(void) {
    const char* files = SDL_GetAndroidInternalStoragePath();
    if (files && !chdir(files)) setenv("HOME", files, 1);
}

const char* gui_android_libraries(void) {
    static char folder[1024];
    Dl_info info;
    char* slash;
    if (!folder[0] && dladdr((void*)gui_android_libraries, &info) && info.dli_fname &&
        strlen(info.dli_fname) < sizeof folder) {
        strcpy(folder, info.dli_fname);
        slash = strrchr(folder, '/');
        if (slash) *slash = 0;
    }
    return folder[0] ? folder : NULL;
}

/* MainActivity.play(String[]) */
int gui_android_play(const char* const* argv, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass string, activity_class;
    jmethodID play;
    jobjectArray args;
    int n = 0, i, ok;
    if (!env || !activity) {
        snprintf(message, size, "No Android activity to start the game from.");
        return 0;
    }
    while (argv[n]) ++n;
    string = (*env)->FindClass(env, "java/lang/String");
    args = (*env)->NewObjectArray(env, n, string, NULL);
    for (i = 0; i < n; ++i) {
        jstring s = (*env)->NewStringUTF(env, argv[i]);
        (*env)->SetObjectArrayElement(env, args, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    activity_class = (*env)->GetObjectClass(env, activity);
    play = (*env)->GetMethodID(env, activity_class, "play", "([Ljava/lang/String;)V");
    if (play) (*env)->CallVoidMethod(env, activity, play, args);
    ok = play && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, args);
    (*env)->DeleteLocalRef(env, string);
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    if (!ok) snprintf(message, size, "The game activity did not start.");
    return ok;
}
