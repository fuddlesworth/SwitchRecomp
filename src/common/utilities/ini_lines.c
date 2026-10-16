/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ini_lines.h"
#include <string.h>

static char* trim(char* s) {
    size_t n;
    while (*s == ' ' || *s == '\t') ++s;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    return s;
}

void snx_ini_start(SnxIni* ini, char* text) {
    ini->at = strncmp(text, "\xEF\xBB\xBF", 3) ? text : text + 3;
    ini->section[0] = 0;
}

int snx_ini_next(SnxIni* ini, char** key, char** value) {
    while (ini->at) {
        char* s = ini->at;
        char* equals;
        ini->at = strchr(s, '\n');
        if (ini->at) *ini->at++ = 0;
        s = trim(s);
        if (!*s || *s == ';' || *s == '#') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            size_t n = end ? (size_t)(end - s - 1) : sizeof ini->section;
            if (n >= sizeof ini->section) n = 0;
            memcpy(ini->section, s + 1, n);
            ini->section[n] = 0;
            continue;
        }
        equals = strchr(s, '=');
        if (!equals) continue;
        *equals = 0;
        *key = trim(s);
        *value = trim(equals + 1);
        return 1;
    }
    return 0;
}
