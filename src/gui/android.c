/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "android.h"
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void gui_android_start(void) {
    const char* files = SDL_GetAndroidInternalStoragePath();
    if (files) setenv("HOME", files, 1);
    if (!gui_android_folder() && files) chdir(files);
}

const char* gui_android_folder(void) {
    static const char folder[] = "/sdcard/ScarletNX";
    mkdir(folder, 0770);
    return !access(folder, W_OK) && !chdir(folder) ? folder : NULL;
}

char* gui_android_document_file(const char* uri) {
    SDL_IOStream* io = SDL_IOFromFile(uri, "rb");
    long fd =
        io ? (long)SDL_GetNumberProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1) : -1;
    char link[64], path[4096], *file;
    ssize_t n = -1;
    if (fd >= 0) {
        snprintf(link, sizeof link, "/proc/self/fd/%ld", fd);
        n = readlink(link, path, sizeof path - 1);
    }
    if (io) SDL_CloseIO(io);
    if (n <= 0 || path[0] != '/') return NULL;
    path[n] = 0;
    /* the provider may open it on the storage's own file system, which is the user's shared storage */
    if (!strncmp(path, "/data/media/", 12)) {
        file = (char*)malloc((size_t)n + 32);
        if (file) snprintf(file, (size_t)n + 32, "/storage/emulated/%s", path + 12);
    } else
        file = strdup(path);
    if (file && access(file, R_OK)) {
        free(file);
        file = NULL;
    }
    return file;
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
