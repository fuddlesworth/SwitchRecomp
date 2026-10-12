/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* fs: RomFS (built on the fly from an extracted romfs/ folder, or a romfs.bin), plus save data
   and SD card mapped to normal folders. */
#include "hle/fs.h"
#include "core/main.h"
#include "hle/services.h"
#include "kernel/ipc.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* nn::fs results */
#define FS_NOTFOUND            RC(2, 1)
#define FS_EXISTS              RC(2, 2)
#define FS_DIR_NOT_EMPTY       RC(2, 8)
#define FS_NO_SPACE            RC(2, 30) /* UsableSpaceNotEnough */
#define FS_OUT_OF_RANGE        RC(2, 3005)
#define FS_PATH_TOO_LONG       RC(2, 6003)
#define FS_DIR_UNOBTAINABLE    RC(2, 6006) /* a ".." above the root */
#define FS_NO_APPEND           RC(2, 6201) /* FileExtensionWithoutOpenModeAllowAppend */
#define FS_READ_NOT_PERMITTED  RC(2, 6202)
#define FS_WRITE_NOT_PERMITTED RC(2, 6203)
static void rm_rf(const char* p, int keep);

/* ============================================================ RomFS virtual */
typedef struct {
    char* path;
    u64 off, size;
    int logged;
} RFile;
typedef struct DNode {
    char* name;
    int parent;
    int first_child, next_sib, first_file;
    u32 off;
} DNode;
typedef struct FNode {
    char* name;
    int parent, next_sib;
    u32 off;
    u64 size;
    char* host;
} FNode;
#define NOPEN 48
static struct {
    size_t romdir_len;
    RFile* rfiles; /* in offset order: romfs_build lays the files out in this order */
    int nrfiles;
    u8* rmeta;
    u64 rmeta_size; /* header + tables sit at the start of the image */
    u64 rdata_off, rtotal;
    FILE* rraw; /* plain romfs.bin */
    PMutex romlock;
    DNode* dn;
    int ndn, capdn;
    FNode* fn;
    int nfn, capfn;
    /* cache of open host files */
    struct {
        const char* path;
        FILE* f;
        u64 last;
    } openf[NOPEN];
    u64 openclk;
    u8 emptyrom[0x200]; /* the empty RomFS of the system archives (emptyrom_init) */
    PMutex savelock;
} fsp;

static int add_dir(const char* name, int parent) {
    if (fsp.ndn == fsp.capdn) {
        fsp.capdn = fsp.capdn ? fsp.capdn * 2 : 256;
        fsp.dn = realloc(fsp.dn, sizeof(DNode) * fsp.capdn);
    }
    DNode* d = &fsp.dn[fsp.ndn];
    d->name = strdup(name);
    d->parent = parent;
    d->first_child = -1;
    d->next_sib = -1;
    d->first_file = -1;
    return fsp.ndn++;
}
static int add_file(const char* name, int parent, u64 size, const char* host) {
    if (fsp.nfn == fsp.capfn) {
        fsp.capfn = fsp.capfn ? fsp.capfn * 2 : 1024;
        fsp.fn = realloc(fsp.fn, sizeof(FNode) * fsp.capfn);
    }
    FNode* f = &fsp.fn[fsp.nfn];
    f->name = strdup(name);
    f->parent = parent;
    f->next_sib = -1;
    f->size = size;
    f->host = strdup(host);
    return fsp.nfn++;
}
/* a host directory that is a link (a symlink, or a junction on Windows): the RomFS builder doesn't follow them, so
   a link to a parent can't loop, and recursive deletes remove the link, not what it points to */
static int is_dir_link(const char* p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat s;
    return !lstat(p, &s) && S_ISLNK(s.st_mode);
#endif
}
typedef struct {
    char* name;
    int is_dir;
    u64 size;
} ScanEnt;
static int cmp_ent(const void* a, const void* b) {
    return strcmp(((const ScanEnt*)a)->name, ((const ScanEnt*)b)->name);
}
#define SCAN_DEPTH 64
static void scan(const char* host, int di, int depth) {
    if (depth > SCAN_DEPTH) {
        WARN("RomFS: %s is more than %d folders deep, not read\n", host, SCAN_DEPTH);
        return;
    }
    PDir d;
    if (plat_dir_open(&d, host)) return;
    ScanEnt* ents = NULL;
    int n = 0, cap = 0;
    while (plat_dir_next(&d)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            ents = realloc(ents, sizeof(ScanEnt) * cap);
        }
        ents[n].name = strdup(d.name);
        ents[n].is_dir = d.is_dir;
        ents[n].size = d.size;
        n++;
    }
    plat_dir_close(&d);
    /* dirs first, then alphabetical, so the layout is stable */
    if (n) qsort(ents, n, sizeof(ScanEnt), cmp_ent);
    for (int pass = 0; pass < 2; pass++) {
        int lastd = -1, lastf = -1;
        for (int i = 0; i < n; i++) {
            if (ents[i].is_dir != (pass == 0)) continue;
            char p[2048];
            snprintf(p, sizeof p, "%s/%s", host, ents[i].name);
            if (pass == 0) {
                if (is_dir_link(p)) {
                    WARN("RomFS: %s is a link to a folder, not followed\n", p);
                    continue;
                }
                int c = add_dir(ents[i].name, di);
                if (lastd < 0)
                    fsp.dn[di].first_child = c;
                else
                    fsp.dn[lastd].next_sib = c;
                lastd = c;
                scan(p, c, depth + 1);
            } else {
                int f = add_file(ents[i].name, di, ents[i].size, p);
                if (lastf < 0)
                    fsp.dn[di].first_file = f;
                else
                    fsp.fn[lastf].next_sib = f;
                lastf = f;
            }
        }
    }
    for (int i = 0; i < n; i++) free(ents[i].name);
    free(ents);
}
static u32 rhash(u32 parent, const char* s) {
    u32 h = parent ^ 123456789u;
    for (; *s; s++) {
        h = (h >> 5) | (h << 27);
        h ^= (u8)*s;
    }
    return h;
}
static u32 hcount(u32 n) {
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    u32 c = n;
    while (!(c % 2) || !(c % 3) || !(c % 5) || !(c % 7) || !(c % 11) || !(c % 13) || !(c % 17)) c++;
    return c;
}

static void romfs_build(const char* dir) {
    add_dir("", 0);
    fsp.dn[0].parent = 0;
    fsp.romdir_len = strlen(dir);
    scan(dir, 0, 0);
    /* table offsets */
    u32 doff = 0;
    for (int i = 0; i < fsp.ndn; i++) {
        fsp.dn[i].off = doff;
        doff += 0x18 + ((strlen(fsp.dn[i].name) + 3) & ~3u);
    }
    u32 foff = 0;
    for (int i = 0; i < fsp.nfn; i++) {
        fsp.fn[i].off = foff;
        foff += 0x20 + ((strlen(fsp.fn[i].name) + 3) & ~3u);
    }
    u32 dhc = hcount(fsp.ndn), fhc = hcount(fsp.nfn);
    u64 hdr = 0x50, dht = hdr, dmt = dht + dhc * 4, fht = dmt + doff, fmt = fht + fhc * 4, end = fmt + foff;
    fsp.rdata_off = (end + 0x1FF) & ~0x1FFull;
    fsp.rmeta_size = fsp.rdata_off;
    fsp.rmeta = calloc(1, fsp.rmeta_size);
    u64 H[10] = {0x50, dht, dhc * 4ull, dmt, doff, fht, fhc * 4ull, fmt, foff, fsp.rdata_off};
    memcpy(fsp.rmeta, H, 0x50);
    u32* dh = (u32*)(fsp.rmeta + dht);
    for (u32 i = 0; i < dhc; i++) dh[i] = 0xFFFFFFFFu;
    u32* fh = (u32*)(fsp.rmeta + fht);
    for (u32 i = 0; i < fhc; i++) fh[i] = 0xFFFFFFFFu;
    u64 data = 0;
    fsp.rfiles = calloc(fsp.nfn ? fsp.nfn : 1, sizeof(RFile));
    for (int i = 0; i < fsp.nfn; i++) {
        data = (data + 0xF) & ~0xFull;
        fsp.rfiles[fsp.nrfiles].path = fsp.fn[i].host;
        fsp.rfiles[fsp.nrfiles].off = fsp.rdata_off + data;
        fsp.rfiles[fsp.nrfiles].size = fsp.fn[i].size;
        fsp.nrfiles++;
        u8* e = fsp.rmeta + fmt + fsp.fn[i].off;
        u32 w[2] = {fsp.dn[fsp.fn[i].parent].off,
                    fsp.fn[i].next_sib >= 0 ? fsp.fn[fsp.fn[i].next_sib].off : 0xFFFFFFFFu};
        memcpy(e, w, 8);
        memcpy(e + 8, &data, 8);
        memcpy(e + 16, &fsp.fn[i].size, 8);
        u32 h = rhash(fsp.dn[fsp.fn[i].parent].off, fsp.fn[i].name) % fhc;
        u32 nl = (u32)strlen(fsp.fn[i].name);
        memcpy(e + 0x18, &fh[h], 4);
        fh[h] = fsp.fn[i].off;
        memcpy(e + 0x1C, &nl, 4);
        memcpy(e + 0x20, fsp.fn[i].name, nl);
        data += fsp.fn[i].size;
    }
    for (int i = 0; i < fsp.ndn; i++) {
        u8* e = fsp.rmeta + dmt + fsp.dn[i].off;
        u32 w[4] = {fsp.dn[fsp.dn[i].parent].off,
                    fsp.dn[i].next_sib >= 0 ? fsp.dn[fsp.dn[i].next_sib].off : 0xFFFFFFFFu,
                    fsp.dn[i].first_child >= 0 ? fsp.dn[fsp.dn[i].first_child].off : 0xFFFFFFFFu,
                    fsp.dn[i].first_file >= 0 ? fsp.fn[fsp.dn[i].first_file].off : 0xFFFFFFFFu};
        memcpy(e, w, 16);
        u32 h = rhash(i ? fsp.dn[fsp.dn[i].parent].off : 0, fsp.dn[i].name) % dhc;
        u32 nl = (u32)strlen(fsp.dn[i].name);
        memcpy(e + 16, &dh[h], 4);
        dh[h] = fsp.dn[i].off;
        memcpy(e + 20, &nl, 4);
        memcpy(e + 24, fsp.dn[i].name, nl);
    }
    fsp.rtotal = fsp.rdata_off + data;
    /* rfiles is already in offset order (offsets grow with i). No sort: one without a tie-break could put a zero
       size file after the file that starts at the same offset, and romfs_read would then find the empty one */
    LOG("RomFS: %d folders, %d files, %.1f MB\n", fsp.ndn, fsp.nfn, (double)fsp.rtotal / 1048576.0);
}

void romfs_init(void) {
    pm_init(&fsp.romlock);
    char p[1200];
    snprintf(p, sizeof p, "%sromfs.bin", g_datadir);
    fsp.rraw = fopen(p, "rb");
    if (fsp.rraw) {
        fsp.rtotal = plat_file_size(p);
        LOG("RomFS: image %s (%.1f MB)\n", p, (double)fsp.rtotal / 1048576.0);
        return;
    }
    snprintf(p, sizeof p, "%sromfs", g_datadir);
    if (plat_is_dir(p)) {
        romfs_build(p);
        return;
    }
    LOG("RomFS: none (folder %s)\n", p);
    char q[1200];
    snprintf(q, sizeof q, "%sexefs/main.nro", g_datadir);
    if (!plat_file_size(q)) {
        char m[1600];
        snprintf(m, sizeof m,
                 "The game's RomFS (its data files) is missing.\n\n"
                 "Extract the game with scripts/extract.py and recompile it with scripts/recomp.py, which puts the "
                 "romfs folder in:\n%s\n\nThe game will close when it tries to read its data.",
                 p);
#if defined(_WIN32)
        win_fatal_box(m);
#endif
        LOG("%s\n", m);
    }
}

static FILE* rf_open(const char* path) { /* fsp.romlock held */
    int lru = 0;
    for (int i = 0; i < NOPEN; i++) {
        if (fsp.openf[i].path == path) {
            fsp.openf[i].last = ++fsp.openclk;
            return fsp.openf[i].f;
        }
        if (fsp.openf[i].last < fsp.openf[lru].last) lru = i;
    }
    if (fsp.openf[lru].f) fclose(fsp.openf[lru].f);
    fsp.openf[lru].f = fopen(path, "rb");
    /* a file that didn't open isn't cached: the next read tries again */
    fsp.openf[lru].path = fsp.openf[lru].f ? path : NULL;
    fsp.openf[lru].last = fsp.openf[lru].f ? ++fsp.openclk : 0;
    return fsp.openf[lru].f;
}
/* -1 when a host file couldn't be read (that part is zeros) */
static int romfs_read(u64 off, void* dst, u64 size) {
    u8* d = dst;
    if (fsp.rraw) {
        pm_lock(&fsp.romlock);
        i64 r = plat_pread(fsp.rraw, d, size, off);
        pm_unlock(&fsp.romlock);
        return r < 0 || (u64)r < size ? -1 : 0;
    }
    if (off < fsp.rmeta_size) {
        u64 n = fsp.rmeta_size - off;
        if (n > size) n = size;
        memcpy(d, fsp.rmeta + off, n);
        d += n;
        off += n;
        size -= n;
    }
    int rc = 0;
    pm_lock(&fsp.romlock);
    while (size) {
        int lo = 0, hi = fsp.nrfiles - 1, k = -1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (fsp.rfiles[m].off <= off) {
                k = m;
                lo = m + 1;
            } else
                hi = m - 1;
        }
        if (k < 0) {
            memset(d, 0, size);
            break;
        }
        RFile* f = &fsp.rfiles[k];
        if (off >= f->off + f->size) { /* inter-file padding */
            u64 nx = k + 1 < fsp.nrfiles ? fsp.rfiles[k + 1].off : fsp.rtotal;
            u64 n = nx > off ? nx - off : size;
            if (n > size) n = size;
            if (!n) n = size;
            memset(d, 0, n);
            d += n;
            off += n;
            size -= n;
            continue;
        }
        u64 n = f->off + f->size - off;
        if (n > size) n = size;
        if (!f->logged) {
            f->logged = 1;
            LOG("romfs: %s (%.1f KB)\n", f->path + fsp.romdir_len, (double)f->size / 1024.0);
        }
        FILE* fp = rf_open(f->path);
        i64 got = fp ? plat_pread(fp, d, n, off - f->off) : -1;
        if (got < 0 || (u64)got < n) {
            if (f->logged < 2) {
                f->logged = 2;
                WARN("romfs: could not read %s (at %llu, %llu bytes), the game gets zeros\n", f->path,
                     (unsigned long long)(off - f->off), (unsigned long long)n);
            }
            memset(d, 0, n);
            rc = -1;
        }
        d += n;
        off += n;
        size -= n;
    }
    pm_unlock(&fsp.romlock);
    return rc;
}
/* QueryRange (OperateRange): an all-zero QueryRangeInfo */
static int query_range(IpcCtx* c) {
    u8 z[0x40];
    memset(z, 0, sizeof z);
    outbytes(c, z, 0x40);
    return 0;
}

/* ============================================================ IStorage (RomFS) */
static int romstorage(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: {
            u64 off = in64(c, 0), sz = in64(c, 8), a, bs;
            buf_out(c, 0, &a, &bs);
            if (sz > bs) sz = bs;
            if (off > fsp.rtotal) return RC(2, 6001);
            if (off + sz > fsp.rtotal) sz = fsp.rtotal - off;
            return romfs_read(off, GP(a), sz) ? FS_OUT_OF_RANGE : 0;
        }
        case 4: out64(c, fsp.rtotal); return 0;
        case 1:
        case 2:
        case 3: return RC(2, 6300);
        case 5: return query_range(c);
    }
    return stub_ok(o, c);
}

/* ============================================================ PC folders as IFileSystem */
typedef struct {
    char root[1024];
} FsData;
typedef struct {
    FILE* f;
    char path[1100];
    u32 mode;  /* OpenMode: 1 read, 2 write, 4 allow append */
    PMutex lk; /* a seek and the read or write after it are one step */
} FileData;
typedef struct {
    PDir d;
    int ok;
    u32 filter;
    char path[1100];
    int done;
} DirData;

static void read_path(IpcCtx* c, int idx, char* out, u32 max) {
    u64 a, s;
    if (!buf_xs(c, idx, &a, &s) || !a) { buf_in(c, idx, &a, &s); }
    u32 n = 0;
    if (a) {
        const char* p = (const char*)GP(a);
        while (n < max - 1 && n < 0x300 && p[n]) {
            out[n] = p[n];
            n++;
        }
    }
    out[n] = 0;
}
/* the host path of a guest path in this file system. The path is normalized like the console does it: "." goes,
   ".." takes the folder before it, and a ".." above the root is an error, so no path leaves the folder */
static u32 host_path(FsData* fs, const char* p, char* out, size_t max) {
    char norm[0x301];
    size_t n = 0;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char* e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t l = (size_t)(e - p);
        if (l == 2 && p[0] == '.' && p[1] == '.') {
            if (!n) return FS_DIR_UNOBTAINABLE;
            while (n && norm[n - 1] != '/') n--;
            n--;
        } else if (!(l == 1 && p[0] == '.')) {
            if (n + 1 + l >= sizeof norm) return FS_PATH_TOO_LONG;
            norm[n++] = '/';
            memcpy(norm + n, p, l);
            n += l;
        }
        p = e;
    }
    norm[n] = 0;
    snprintf(out, max, "%s%s", fs->root, norm);
    return 0;
}
/* the path argument idx of a request: the guest's (raw) and the host's */
static u32 req_path(IpcCtx* c, int idx, FsData* fs, char raw[0x301], char* hp, size_t max) {
    read_path(c, idx, raw, 0x301);
    return host_path(fs, raw, hp, max);
}
static int host_exists(const char* p) {
    if (plat_is_dir(p)) return 1;
    FILE* f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}
static void mkdirs(const char* path) {
    char t[1100];
    strncpy(t, path, 1099);
    t[1099] = 0;
    for (char* p = t + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            plat_mkdir(t);
            *p = '/';
        }
    }
    plat_mkdir(t);
}
/* 64-bit file offsets: long is 32 bits on Windows */
static int file_seek(FILE* f, u64 off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static u64 file_size(FILE* f) {
    fflush(f);
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    off_t n = ftello(f);
#endif
    return n < 0 ? 0 : (u64)n;
}
/* grows (with zeros) or shrinks the file in place */
static int file_truncate(FILE* f, u64 n) {
    fflush(f);
#if defined(_WIN32)
    return _chsize_s(_fileno(f), (long long)n) ? -1 : 0;
#else
    return ftruncate(fileno(f), (off_t)n);
#endif
}
static int host_rmdir(const char* p) {
#if defined(_WIN32)
    return RemoveDirectoryA(p) ? 0 : -1;
#else
    return rmdir(p);
#endif
}

static int file_disp(SvcObj* o, IpcCtx* c) {
    FileData* f = o->data;
    switch (c->cmd) {
        case 0: { /* Read(u32 option, i64 offset, i64 size) */
            u64 off = in64(c, 8), sz = in64(c, 16), a, bs;
            if (!(f->mode & 1)) return FS_READ_NOT_PERMITTED;
            if ((i64)off < 0 || (i64)sz < 0) return FS_OUT_OF_RANGE;
            buf_out(c, 0, &a, &bs);
            if (sz > bs) sz = bs;
            pm_lock(&f->lk);
            if (off > file_size(f->f) || file_seek(f->f, off)) {
                pm_unlock(&f->lk);
                return FS_OUT_OF_RANGE;
            }
            u64 n = fread(GP(a), 1, sz, f->f);
            pm_unlock(&f->lk);
            out64(c, n);
            return 0;
        }
        case 1: { /* Write(u32 option, i64 offset, i64 size) */
            u64 off = in64(c, 8), sz = in64(c, 16), a, bs;
            if (!(f->mode & 2)) return FS_WRITE_NOT_PERMITTED;
            if ((i64)off < 0 || (i64)sz < 0) return FS_OUT_OF_RANGE;
            buf_in(c, 0, &a, &bs);
            if (sz > bs) sz = bs;
            pm_lock(&f->lk);
            u32 rc = 0;
            if (off + sz > file_size(f->f) && !(f->mode & 4))
                rc = FS_NO_APPEND;
            else if (file_seek(f->f, off))
                rc = FS_OUT_OF_RANGE;
            else if (fwrite(GP(a), 1, sz, f->f) != sz)
                rc = FS_NO_SPACE;
            else if (in32(c, 0) & 1)
                fflush(f->f);
            pm_unlock(&f->lk);
            return (int)rc;
        }
        case 2:
            pm_lock(&f->lk);
            fflush(f->f);
            pm_unlock(&f->lk);
            return 0;
        case 3: { /* SetSize(i64) */
            u64 n = in64(c, 0);
            if (!(f->mode & 2)) return FS_WRITE_NOT_PERMITTED;
            if ((i64)n < 0) return FS_OUT_OF_RANGE;
            pm_lock(&f->lk);
            int r = file_truncate(f->f, n);
            pm_unlock(&f->lk);
            return r ? FS_NO_SPACE : 0;
        }
        case 4: {
            pm_lock(&f->lk);
            u64 n = file_size(f->f);
            pm_unlock(&f->lk);
            out64(c, n);
            return 0;
        }
        case 5: return query_range(c);
    }
    return stub_ok(o, c);
}
static void file_destroy(SvcObj* o) {
    FileData* f = o->data;
    if (f->f) fclose(f->f);
}

static int dir_disp(SvcObj* o, IpcCtx* c) {
    DirData* d = o->data;
    switch (c->cmd) {
        case 0: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            u64 n = 0, max = s / 0x310;
            while (n < max && d->ok && !d->done) {
                if (!plat_dir_next(&d->d)) {
                    d->done = 1;
                    break;
                }
                if (d->d.is_dir && !(d->filter & 1)) continue;
                if (!d->d.is_dir && !(d->filter & 2)) continue;
                u8* e = GP(a + n * 0x310);
                memset(e, 0, 0x310);
                strncpy((char*)e, d->d.name, 0x300);
                e[0x304] = d->d.is_dir ? 0 : 1;
                u64 sz = d->d.is_dir ? 0 : d->d.size;
                memcpy(e + 0x308, &sz, 8);
                n++;
            }
            out64(c, n);
            return 0;
        }
        case 1: {
            PDir t;
            u64 n = 0;
            if (!plat_dir_open(&t, d->path)) {
                while (plat_dir_next(&t)) {
                    if (t.is_dir ? (d->filter & 1) : (d->filter & 2)) n++;
                }
                plat_dir_close(&t);
            }
            out64(c, n);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static void dir_destroy(SvcObj* o) {
    DirData* d = o->data;
    if (d->ok) plat_dir_close(&d->d);
}

static int fs_disp(SvcObj* o, IpcCtx* c) {
    FsData* fs = o->data;
    char p[0x301], hp[1100], p2[0x301], hp2[1100];
    u32 rc;
    switch (c->cmd) {
        case 0: { /* CreateFile(u32 option, i64 size, path) */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            u64 sz = in64(c, 8);
            if (host_exists(hp)) return FS_EXISTS;
            FILE* t = fopen(hp, "wb");
            if (!t) return FS_NOTFOUND;
            int r = sz ? file_truncate(t, sz) : 0;
            fclose(t);
            if (r) {
                remove(hp);
                return FS_NO_SPACE;
            }
            return 0;
        }
        case 1: /* DeleteFile: not a directory */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (plat_is_dir(hp)) return FS_NOTFOUND;
            return remove(hp) ? FS_NOTFOUND : 0;
        case 2: /* CreateDirectory */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (host_exists(hp)) return FS_EXISTS;
            return plat_mkdir(hp) ? FS_NOTFOUND : 0;
        case 3: /* DeleteDirectory: only an empty one */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (!plat_is_dir(hp)) return FS_NOTFOUND;
            return host_rmdir(hp) ? FS_DIR_NOT_EMPTY : 0;
        case 4:    /* DeleteDirectoryRecursively */
        case 13: { /* CleanDirectoryRecursively: empties it, keeps it */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (!plat_is_dir(hp)) return FS_NOTFOUND;
            rm_rf(hp, c->cmd == 13);
            return 0;
        }
        case 5:
        case 6: /* RenameFile, RenameDirectory: the console doesn't replace an entry that exists */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if ((rc = req_path(c, 1, fs, p2, hp2, sizeof hp2))) return (int)rc;
            if (!host_exists(hp)) return FS_NOTFOUND;
            if (host_exists(hp2)) return FS_EXISTS;
            return rename(hp, hp2) ? FS_NOTFOUND : 0;
        case 7: {
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (plat_is_dir(hp) || !strcmp(p, "/") || !p[0]) {
                out32(c, 0);
                return 0;
            }
            FILE* t = fopen(hp, "rb");
            if (!t) return FS_NOTFOUND;
            fclose(t);
            out32(c, 1);
            return 0;
        }
        case 8: { /* OpenFile(u32 mode, path): a directory is not found */
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            u32 mode = in32(c, 0);
            if (plat_is_dir(hp)) return FS_NOTFOUND;
            FILE* t = fopen(hp, (mode & 2) ? "rb+" : "rb");
            if (!t) return FS_NOTFOUND;
            SvcObj* f = svcobj_new("IFile", file_disp, sizeof(FileData));
            FileData* fd = f->data;
            fd->f = t;
            fd->mode = mode;
            pm_init(&fd->lk);
            strcpy(fd->path, hp);
            f->destroy = file_destroy;
            out_obj(c, f);
            return 0;
        }
        case 9: {
            if ((rc = req_path(c, 0, fs, p, hp, sizeof hp))) return (int)rc;
            if (!plat_is_dir(hp)) return FS_NOTFOUND;
            SvcObj* d = svcobj_new("IDirectory", dir_disp, sizeof(DirData));
            DirData* dd = d->data;
            dd->filter = in32(c, 0);
            if (!dd->filter) dd->filter = 3;
            strcpy(dd->path, hp);
            dd->ok = !plat_dir_open(&dd->d, hp);
            d->destroy = dir_destroy;
            out_obj(c, d);
            return 0;
        }
        case 10: return 0;
        case 11: out64(c, 4ull << 30); return 0;
        case 12: out64(c, 8ull << 30); return 0;
        case 14: {
            u8 z[0x20];
            memset(z, 0, sizeof z);
            outbytes(c, z, 0x20);
            return 0;
        }
        case 15: return 0;
        case 16: out64(c, 0); return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* hostfs_new(const char* root) {
    SvcObj* o = svcobj_new("IFileSystem", fs_disp, sizeof(FsData));
    FsData* d = o->data;
    strncpy(d->root, root, 1023);
    size_t l = strlen(d->root);
    while (l && (d->root[l - 1] == '/' || d->root[l - 1] == '\\')) d->root[--l] = 0;
    mkdirs(d->root);
    return o;
}

/* a link inside is removed, not followed */
static void rm_rf(const char* p, int keep) {
    PDir d;
    if (!plat_dir_open(&d, p)) {
        while (plat_dir_next(&d)) {
            char q[1600];
            snprintf(q, sizeof q, "%s/%s", p, d.name);
            if (d.is_dir && is_dir_link(q))
#if defined(_WIN32)
                RemoveDirectoryA(q); /* a junction goes like an empty folder */
#else
                remove(q);
#endif
            else if (d.is_dir)
                rm_rf(q, 0);
            else
                remove(q);
        }
        plat_dir_close(&d);
    }
    if (!keep) host_rmdir(p);
}

static int savereader(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0) {
        out64(c, 0);
        return 0;
    }
    return stub_ok(o, c);
}
/* system archives (NgWord, fonts...) aren't available, hand out an empty but valid RomFS */
static void emptyrom_init(void) {
    u8* b = fsp.emptyrom;
    u64 H[10] = {0x50, 0x50, 4, 0x54, 0x18, 0x6C, 4, 0x70, 0, 0x200};
    memcpy(b, H, 0x50);
    u32 dh = 0;
    memcpy(b + 0x50, &dh, 4);
    u32 root[6] = {0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0};
    memcpy(b + 0x54, root, 0x18);
    u32 fh = 0xFFFFFFFFu;
    memcpy(b + 0x6C, &fh, 4);
}
static int emptystorage(SvcObj* o, IpcCtx* c) {
    (void)o;
    switch (c->cmd) {
        case 0: {
            u64 off = in64(c, 0), sz = in64(c, 8), a, bs;
            buf_out(c, 0, &a, &bs);
            if (sz > bs) sz = bs;
            u8* d = GP(a);
            for (u64 i = 0; i < sz; i++) d[i] = off + i < sizeof fsp.emptyrom ? fsp.emptyrom[off + i] : 0;
            return 0;
        }
        case 4: out64(c, sizeof fsp.emptyrom); return 0;
        case 5: return query_range(c);
    }
    return RC(2, 6300);
}
/* save data folders, by SaveDataType */
static const char* const g_save_kinds[] = {"system", "user", "bcat", "device", "temp", "cache", "sysbcat"};
/* Earlier versions read the save type from the wrong byte, which is always 0, so every save went to save/system.
   An Account save (save/user) that doesn't exist yet takes over that folder, once */
static void save_migrate(const char* dst) {
    char src[1200];
    snprintf(src, sizeof src, "%ssave/system", g_datadir);
    if (plat_is_dir(dst) || !plat_is_dir(src)) return;
    PDir d;
    int any = 0;
    if (!plat_dir_open(&d, src)) {
        any = plat_dir_next(&d);
        plat_dir_close(&d);
    }
    if (!any) return;
    if (rename(src, dst))
        WARN("fs: could not move the saves in %s to %s\n", src, dst);
    else
        LOG("fs: moved the saves in %s to %s, where account saves belong\n", src, dst);
}
static int fsp_disp(SvcObj* o, IpcCtx* c) {
    char p[1200];
    switch (c->cmd) {
        case 1: return 0;
        case 18:
            snprintf(p, sizeof p, "%ssdmc", g_datadir);
            out_obj(c, hostfs_new(p));
            return 0;
        case 51:
        case 52:
        case 53: {
            /* in: u8 space @0, then the attribute @8 = { u64 program_id, uid[16], u64 system_save_id, u8 type, u8 rank,
               u16 index }: the type is at 8 + 0x20 */
            u8 type = in8(c, 8 + 0x20);
            const char* kind = type < sizeof g_save_kinds / sizeof g_save_kinds[0] ? g_save_kinds[type] : "system";
            snprintf(p, sizeof p, "%ssave/%s", g_datadir, kind);
            pm_lock(&fsp.savelock);
            if (type == 1) save_migrate(p);
            SvcObj* fs = hostfs_new(p);
            pm_unlock(&fsp.savelock);
            out_obj(c, fs);
            return 0;
        }
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 28:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37: return 0;
        case 60:
        case 61:
        case 62: out_obj(c, svcobj_new("ISaveDataInfoReader", savereader, 0)); return 0;
        case 200: {
            if (!fsp.rtotal) {
                WARN("fs: the game requests the RomFS but there is none (put the romfs folder in %s)\n", g_datadir);
            }
            out_obj(c, svcobj_new("IStorage(romfs)", romstorage, 0));
            return 0;
        }
        case 202: {
            u64 id = in64(c, 8);
            LOG("fs: system data %016llx (not available on PC: provided empty)\n", (unsigned long long)id);
            out_obj(c, svcobj_new("IStorage(system)", emptystorage, 0));
            return 0;
        }
        case 203: return RC(2, 1001);
        case 400: out_obj(c, svc_generic("IDeviceOperator")); return 0;
        case 500: out_obj(c, svc_generic("IEventNotifier")); return 0;
        case 1003:
        case 1004:
        case 1006:
        case 1007:
        case 1011:
        case 1012:
        case 1013: return 0;
        case 1005: out32(c, 0); return 0;
        case 1008: out_obj(c, svc_generic("IMultiCommitManager")); return 0; /* OpenMultiCommitManager */
        case 1009: {                                                         /* GetAndClearMemoryReportInfo */
            u8 z[0x80];
            memset(z, 0, sizeof z);
            outbytes(c, z, sizeof z);
            return 0;
        }
        case 620:
        case 630:
        case 631:
        case 632:
        case 800: return 0;
        case 8:
        case 11:
        case 17:
        case 19:
        case 31: return RC(2, 1001);
        case 2:
        case 7: return RC(2, 1001);
    }
    return stub_ok(o, c);
}
static SvcObj* new_fsp(void) { return svcobj_new("fsp-srv", fsp_disp, 0); }
static int fsppr_disp(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
static SvcObj* new_fsppr(void) { return svcobj_new("fsp-pr", fsppr_disp, 0); }

void svc_fs_init(void) {
    pm_init(&fsp.savelock);
    emptyrom_init();
    svc_register("fsp-srv", new_fsp);
    svc_register("fsp-pr", new_fsppr);
    svc_register("fsp-ldr", new_fsppr);
}
