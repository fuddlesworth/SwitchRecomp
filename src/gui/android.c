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

/* MainActivity's method, with one argument */
static int call(const char* method, const char* signature, jobject argument, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    jclass activity_class;
    jmethodID id;
    int ok;
    if (!env || !activity) {
        snprintf(message, size, "No Android activity to ask.");
        return 0;
    }
    activity_class = (*env)->GetObjectClass(env, activity);
    id = (*env)->GetMethodID(env, activity_class, method, signature);
    if (id) (*env)->CallVoidMethod(env, activity, id, argument);
    ok = id && !(*env)->ExceptionCheck(env);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, activity_class);
    (*env)->DeleteLocalRef(env, activity);
    if (!ok) snprintf(message, size, "MainActivity.%s did not run.", method);
    return ok;
}

int gui_android_play(const char* const* argv, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jclass string;
    jobjectArray args;
    int n = 0, i, ok;
    if (!env) return call("play", "", NULL, message, size);
    while (argv[n]) ++n;
    string = (*env)->FindClass(env, "java/lang/String");
    args = (*env)->NewObjectArray(env, n, string, NULL);
    for (i = 0; i < n; ++i) {
        jstring s = (*env)->NewStringUTF(env, argv[i]);
        (*env)->SetObjectArrayElement(env, args, i, s);
        (*env)->DeleteLocalRef(env, s);
    }
    ok = call("play", "([Ljava/lang/String;)V", args, message, size);
    (*env)->DeleteLocalRef(env, args);
    (*env)->DeleteLocalRef(env, string);
    return ok;
}

int gui_android_share(const char* text, char* message, size_t size) {
    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jstring s;
    int ok;
    if (!env) return call("share", "", NULL, message, size);
    s = (*env)->NewStringUTF(env, text);
    ok = call("share", "(Ljava/lang/String;)V", s, message, size);
    (*env)->DeleteLocalRef(env, s);
    return ok;
}
