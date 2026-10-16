/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "internal.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#endif

bool ctr_fail(CtrReader* r, SnxCtrExtractResult result, const char* format, ...) {
    if (r->result == SNX_CTR_EXTRACT_OK) {
        va_list args;
        r->result = result;
        va_start(args, format);
        if (r->error_size) vsnprintf(r->error, r->error_size, format, args);
        va_end(args);
    }
    return false;
}
bool ctr_read(CtrReader* r, CtrSpan span, uint64_t at, void* output, size_t size) {
    if (!ctr_fits(r->size, span.offset, span.size) || !ctr_fits(span.size, at, size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "truncated or out-of-range archive section");
    uint64_t offset = span.offset + at;
    if (offset > INT64_MAX) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "file offset exceeds host limit");
#ifdef _WIN32
    int failed = _fseeki64(r->input, (int64_t)offset, SEEK_SET);
#else
    int failed = fseeko(r->input, (off_t)offset, SEEK_SET);
#endif
    if (failed || (size && fread(output, 1, size, r->input) != size))
        return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot read CIA input");
    return true;
}
bool ctr_sha256(CtrReader* r, const void* data, size_t size, unsigned char hash[32]) {
    return EVP_Digest(data, size, hash, NULL, EVP_sha256(), NULL) ||
           ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
}
bool ctr_hash(CtrReader* r, CtrSpan span, unsigned char hash[32]) {
    unsigned char bytes[32768];
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool ok = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL);
    for (uint64_t at = 0; ok && at < span.size;) {
        size_t n = span.size - at > sizeof bytes ? sizeof bytes : (size_t)(span.size - at);
        if (!ctr_read(r, span, at, bytes, n)) {
            EVP_MD_CTX_free(context);
            return false;
        }
        ok = EVP_DigestUpdate(context, bytes, n);
        at += n;
    }
    ok = ok && EVP_DigestFinal_ex(context, hash, NULL);
    EVP_MD_CTX_free(context);
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
}
bool ctr_hash_matches(CtrReader* r, CtrSpan span, const unsigned char expected[32]) {
    unsigned char hash[32];
    return ctr_hash(r, span, hash) &&
           (!memcmp(hash, expected, 32) || ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "archive SHA-256 mismatch"));
}
static bool budget(CtrReader* r, uint64_t bytes) {
    if (bytes > CTR_OUTPUT_LIMIT - r->output_bytes || r->entries >= 100000)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "extraction exceeds 1 GiB / 100000-entry limit");
    r->output_bytes += bytes;
    r->entries++;
    return true;
}
bool ctr_write(CtrReader* r, const char* path, const void* data, size_t size) {
    if (!budget(r, size)) return false;
    FILE* out = snx_fopen(path, "wbx");
    if (!out) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create new output file: %s", path);
    bool ok = !size || fwrite(data, 1, size, out) == size;
    if (fclose(out)) ok = false;
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot write output file: %s", path);
}
bool ctr_copy(CtrReader* r, CtrSpan span, const char* path, const unsigned char* expected) {
    if (!budget(r, span.size)) return false;
    FILE* out = snx_fopen(path, "wbx");
    if (!out) return ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot create new output file: %s", path);
    unsigned char bytes[32768], hash[32];
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool hashed = context && EVP_DigestInit_ex(context, EVP_sha256(), NULL);
    bool ok = true;
    for (uint64_t at = 0; at < span.size && ok;) {
        size_t n = span.size - at > sizeof bytes ? sizeof bytes : (size_t)(span.size - at);
        ok = ctr_read(r, span, at, bytes, n);
        if (ok) {
            hashed = hashed && EVP_DigestUpdate(context, bytes, n);
            ok = fwrite(bytes, 1, n, out) == n;
        }
        at += n;
    }
    if (fclose(out)) ok = false;
    hashed = hashed && EVP_DigestFinal_ex(context, hash, NULL);
    EVP_MD_CTX_free(context);
    if (ok && !hashed) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "SHA-256 failed (out of memory)");
    if (ok && expected && memcmp(hash, expected, 32))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "content SHA-256 mismatch");
    return ok || ctr_fail(r, SNX_CTR_EXTRACT_IO, "cannot copy archive section");
}
bool ctr_directory(CtrReader* r, const char* path) {
    if (!budget(r, 0)) return false;
    if (snx_path_exists(path) || snx_mkdirs(path))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate or invalid directory: %s", path);
    return true;
}
bool ctr_name(const char* name) {
    size_t size = strlen(name);
    if (!size || size > 255 || !strcmp(name, ".") || !strcmp(name, "..") || name[size - 1] == '.' ||
        name[size - 1] == ' ')
        return false;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p)
        if (*p < 32 || *p == 127 || strchr("/\\:<>\"|?*", *p)) return false;
    char base[8] = {0};
    size_t n = strcspn(name, ".");
    if (n <= 7) {
        for (size_t i = 0; i < n; ++i) base[i] = (char)toupper((unsigned char)name[i]);
        if (!strcmp(base, "CON") || !strcmp(base, "PRN") || !strcmp(base, "AUX") || !strcmp(base, "NUL") ||
            !strcmp(base, "CONIN$") || !strcmp(base, "CONOUT$") || !strcmp(base, "CLOCK$"))
            return false;
        if (!memcmp(base, "COM", 3) || !memcmp(base, "LPT", 3)) {
            if (n == 4 && base[3] >= '1' && base[3] <= '9') return false;
            /* Windows also recognizes superscript 1/2/3 as device numbers. */
            if (n == 5 && (unsigned char)base[3] == 0xc2 &&
                ((unsigned char)base[4] == 0xb9 || (unsigned char)base[4] == 0xb2 || (unsigned char)base[4] == 0xb3))
                return false;
        }
    }
    return true;
}
/* Check before snx_absolute: canonicalization must not hide existing links. */
static bool no_links(const char* path) {
    char* part = snx_strdup(path);
    bool ok = true;
    for (size_t i = 0; ok; ++i) {
        if (!part[i] || part[i] == '/' || part[i] == '\\') {
            char saved = part[i];
            part[i] = 0;
            if (*part && snx_is_link(part)) ok = false;
            part[i] = saved;
            if (!saved) break;
        }
    }
    free(part);
    return ok;
}
SnxCtrExtractResult snx_ctr_extract(const char* input, const char* destination, const char* work, char** published,
                                    char* error, size_t error_size) {
    CtrReader r = {.error = error, .error_size = error_size};
    CtrCia cia = {0};
    char *target = NULL, *parent = NULL, *stage = NULL, *generated = NULL;
    if (published) *published = NULL;
    if (error_size) error[0] = 0;
    if (!input || !snx_is_file(input) || !(r.input = snx_fopen(input, "rb"))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot open CIA input");
        goto done;
    }
#ifdef _WIN32
    if (_fseeki64(r.input, 0, SEEK_END) || _ftelli64(r.input) < 0) {
#else
    if (fseeko(r.input, 0, SEEK_END) || ftello(r.input) < 0) {
#endif
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot measure CIA input");
        goto done;
    }
#ifdef _WIN32
    r.size = (uint64_t)_ftelli64(r.input);
#else
    r.size = (uint64_t)ftello(r.input);
#endif
    if (!ctr_cia_load(&r, &cia)) goto done;
    if (!destination) {
        char hex[65];
        for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", cia.input_hash[i]);
        generated = snx_format("%s/%016" PRIx64 "-%s", work ? work : "work/3ds", cia.title_id, hex);
        destination = generated;
    }
    if (!*destination || !no_links(destination) || !(target = snx_absolute(destination))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "invalid output path or symlink ancestor");
        goto done;
    }
    for (const unsigned char* p = (const unsigned char*)target; *p; ++p) {
        if (*p < 32 || *p == 127) {
            ctr_fail(&r, SNX_CTR_EXTRACT_IO, "output path may not contain control characters");
            goto done;
        }
    }
    if (snx_path_exists(target)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_EXISTS, "output already exists; no cache reuse or overwrite: %s", target);
        goto done;
    }
    parent = snx_dirname(target);
    if (snx_mkdirs(parent) || !(stage = snx_temp_dir(parent))) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot create private staging directory");
        goto done;
    }
    if (!ctr_cia_contents(&r, &cia, stage) || !ctr_ncch_extract(&r, &cia, stage)) goto done;
    /* Detect normal concurrent input edits, including after content validation. */
    if (!ctr_hash_matches(&r, (CtrSpan){0, r.size}, cia.input_hash)) goto done;
    if (snx_path_exists(target) || snx_replace(stage, target)) {
        ctr_fail(&r, SNX_CTR_EXTRACT_IO, "cannot publish extraction without overwriting an existing output");
        goto done;
    }
    free(stage);
    stage = NULL;
    if (published) {
        *published = target;
        target = NULL;
    }
done:
    if (stage) snx_remove_tree(stage);
    if (r.input) fclose(r.input);
    ctr_cia_free(&cia);
    if (target) free(target);
    if (parent) free(parent);
    if (stage) free(stage);
    if (generated) free(generated);
    return r.result;
}
