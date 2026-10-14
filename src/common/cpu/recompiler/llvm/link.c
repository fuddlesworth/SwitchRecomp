/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int snx_link(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
             const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    memset(output, 0, sizeof(*output));
    if (!count) {
        snprintf(error, error_size, "nothing to link");
        return -1;
    }
    if (inputs[0].size >= 4 && !memcmp(inputs[0].data, "\177ELF", 4))
        return snx_link_elf(inputs, count, exports, export_count, soname, output, error, error_size);
    if (inputs[0].size >= 2 && inputs[0].data[0] == 0x64 && inputs[0].data[1] == 0x86)
        return snx_link_coff(inputs, count, exports, export_count, soname, output, error, error_size);
    snprintf(error, error_size, "%s: not an ELF or COFF object", inputs[0].name);
    return -1;
}

void snx_link_reserve(SnxLinkBuffer* buffer, size_t size) {
    if (size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        unsigned char* data;
        while (capacity < size) capacity *= 2;
        data = (unsigned char*)realloc(buffer->data, capacity);
        if (!data) {
            fputs("out of memory\n", stderr);
            abort();
        }
        memset(data + buffer->capacity, 0, capacity - buffer->capacity);
        buffer->data = data;
        buffer->capacity = capacity;
    }
    if (size > buffer->size) buffer->size = size;
}

size_t snx_link_append(SnxLinkBuffer* buffer, const void* data, size_t size) {
    size_t at = buffer->size;
    snx_link_reserve(buffer, at + size);
    if (size && data) memcpy(buffer->data + at, data, size);
    return at;
}

void snx_link_align(SnxLinkBuffer* buffer, size_t alignment) {
    if (alignment > 1) snx_link_reserve(buffer, (buffer->size + alignment - 1) / alignment * alignment);
}

static size_t name_hash(const char* key) {
    size_t h = 5381;
    while (*key) h = h * 33 + (unsigned char)*key++;
    return h;
}

int snx_link_find(const SnxLinkNames* names, const char* key, size_t* value) {
    size_t i;
    if (!names->capacity) return 0;
    for (i = name_hash(key) & (names->capacity - 1); names->keys[i]; i = (i + 1) & (names->capacity - 1))
        if (!strcmp(names->keys[i], key)) {
            *value = names->values[i];
            return 1;
        }
    return 0;
}

void snx_link_insert(SnxLinkNames* names, const char* key, size_t value) {
    size_t i;
    if ((names->count + 1) * 2 > names->capacity) {
        SnxLinkNames grown;
        grown.capacity = names->capacity ? names->capacity * 2 : 1024;
        grown.count = 0;
        grown.keys = (char**)calloc(grown.capacity, sizeof(char*) + sizeof(size_t)); /* the values follow */
        if (!grown.keys) {
            fputs("out of memory\n", stderr);
            abort();
        }
        grown.values = (size_t*)(grown.keys + grown.capacity);
        for (i = 0; i < names->capacity; ++i)
            if (names->keys[i]) {
                size_t j = name_hash(names->keys[i]) & (grown.capacity - 1);
                while (grown.keys[j]) j = (j + 1) & (grown.capacity - 1);
                grown.keys[j] = names->keys[i];
                grown.values[j] = names->values[i];
                ++grown.count;
            }
        free(names->keys);
        *names = grown;
    }
    for (i = name_hash(key) & (names->capacity - 1); names->keys[i]; i = (i + 1) & (names->capacity - 1))
        if (!strcmp(names->keys[i], key)) {
            names->values[i] = value;
            return;
        }
    names->keys[i] = (char*)malloc(strlen(key) + 1);
    if (!names->keys[i]) abort();
    strcpy(names->keys[i], key);
    names->values[i] = value;
    ++names->count;
}

void snx_link_names_free(SnxLinkNames* names) {
    size_t i;
    for (i = 0; i < names->capacity; ++i) free(names->keys[i]);
    free(names->keys);
    memset(names, 0, sizeof(*names));
}
