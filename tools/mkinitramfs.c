/*
 * mkinitramfs - deterministic "newc" cpio writer for Aether.
 *
 *   mkinitramfs -r ROOTDIR -o OUT.cpio [-n PATH:TYPE:MODE:MAJOR:MINOR]...
 *
 * Packs ROOTDIR (entries sorted by path, uid/gid 0, mtime fixed to
 * SOURCE_DATE_EPOCH or 0) so the same tree always yields the same bytes.
 * -n adds a device node without needing root (TYPE is c or b, MODE octal),
 * e.g. -n dev/console:c:600:5:1. Needs no cpio binary and no root.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

typedef struct {
    char *path;        /* path inside the archive ("." for the root) */
    char *src;         /* path on disk, NULL for synthesized nodes */
    unsigned mode;
    unsigned rmaj, rmin;
} Entry;

static Entry *entries;
static size_t n_entries, cap_entries;
static unsigned long mtime_value;

static void die(const char *msg, const char *arg)
{
    fprintf(stderr, "mkinitramfs: %s%s%s%s%s\n", msg, arg ? " '" : "",
            arg ? arg : "", arg ? "'" : "", errno ? strerror(errno) : "");
    exit(1);
}

static void add(const char *path, const char *src, unsigned mode,
                unsigned rmaj, unsigned rmin)
{
    if (n_entries == cap_entries) {
        cap_entries = cap_entries ? cap_entries * 2 : 64;
        entries = realloc(entries, cap_entries * sizeof(*entries));
        if (!entries)
            die("out of memory", NULL);
    }
    Entry *e = &entries[n_entries++];
    e->path = strdup(path);
    e->src = src ? strdup(src) : NULL;
    e->mode = mode;
    e->rmaj = rmaj;
    e->rmin = rmin;
    if (!e->path || (src && !e->src))
        die("out of memory", NULL);
}

static int cmp_entry(const void *a, const void *b)
{
    return strcmp(((const Entry *)a)->path, ((const Entry *)b)->path);
}

static void walk(const char *disk, const char *arc)
{
    DIR *d = opendir(disk);
    if (!d)
        die("cannot open directory ", disk);

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        char dpath[PATH_MAX], apath[PATH_MAX];
        if (snprintf(dpath, sizeof(dpath), "%s/%s", disk, de->d_name) >=
                (int)sizeof(dpath) ||
            snprintf(apath, sizeof(apath), "%s/%s", arc, de->d_name) >=
                (int)sizeof(apath)) {
            errno = ENAMETOOLONG;
            die("path too long: ", dpath);
        }

        struct stat st;
        if (lstat(dpath, &st) != 0)
            die("cannot stat ", dpath);

        if (S_ISDIR(st.st_mode)) {
            add(apath, dpath, st.st_mode & (S_IFMT | 07777), 0, 0);
            walk(dpath, apath);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            add(apath, dpath, st.st_mode & (S_IFMT | 07777), 0, 0);
        } else {
            errno = 0;
            die("unsupported file type (use -n for device nodes): ", dpath);
        }
    }
    closedir(d);
}

static void put_hex(FILE *out, unsigned long v)
{
    fprintf(out, "%08lX", v & 0xffffffffUL);
}

static unsigned long written;

static void pad4(FILE *out)
{
    while (written % 4) {
        fputc(0, out);
        written++;
    }
}

static void emit(FILE *out, const Entry *e, unsigned long ino)
{
    struct stat st;
    unsigned long size = 0;
    char *linkbuf = NULL;
    FILE *in = NULL;

    if (e->src) {
        if (lstat(e->src, &st) != 0)
            die("cannot stat ", e->src);
        if (S_ISLNK(e->mode)) {
            linkbuf = malloc((size_t)st.st_size + 1);
            if (!linkbuf)
                die("out of memory", NULL);
            ssize_t n = readlink(e->src, linkbuf, (size_t)st.st_size);
            if (n < 0)
                die("cannot read link ", e->src);
            size = (unsigned long)n;
        } else if (S_ISREG(e->mode)) {
            if (st.st_size > 0xffffffffL)
                die("file too large: ", e->src);
            size = (unsigned long)st.st_size;
            in = fopen(e->src, "rb");
            if (!in)
                die("cannot open ", e->src);
        }
    }

    size_t namelen = strlen(e->path) + 1;
    unsigned nlink = S_ISDIR(e->mode) ? 2 : 1;

    fputs("070701", out);
    put_hex(out, ino);
    put_hex(out, e->mode);
    put_hex(out, 0);              /* uid */
    put_hex(out, 0);              /* gid */
    put_hex(out, nlink);
    put_hex(out, mtime_value);
    put_hex(out, size);
    put_hex(out, 0); put_hex(out, 0);          /* dev major/minor */
    put_hex(out, e->rmaj); put_hex(out, e->rmin);
    put_hex(out, namelen);
    put_hex(out, 0);              /* checksum */
    fputs(e->path, out);
    fputc(0, out);
    written += 110 + namelen;
    pad4(out);

    if (linkbuf) {
        fwrite(linkbuf, 1, size, out);
        free(linkbuf);
    } else if (in) {
        char buf[65536];
        unsigned long left = size;
        while (left) {
            size_t want = left < sizeof(buf) ? (size_t)left : sizeof(buf);
            size_t got = fread(buf, 1, want, in);
            if (got == 0)
                die("short read from ", e->src);
            fwrite(buf, 1, got, out);
            left -= got;
        }
        fclose(in);
    }
    written += size;
    pad4(out);
}

int main(int argc, char **argv)
{
    const char *root = NULL, *outpath = NULL;
    struct { char *spec; } nodes[64];
    int n_nodes = 0;
    int opt;

    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    mtime_value = epoch ? strtoul(epoch, NULL, 10) : 0;

    while ((opt = getopt(argc, argv, "r:o:n:")) != -1) {
        if (opt == 'r')
            root = optarg;
        else if (opt == 'o')
            outpath = optarg;
        else if (opt == 'n' && n_nodes < 64)
            nodes[n_nodes++].spec = optarg;
        else {
            fprintf(stderr, "Usage: mkinitramfs -r ROOT -o OUT "
                    "[-n PATH:TYPE:MODE:MAJOR:MINOR]...\n");
            return 2;
        }
    }
    if (!root || !outpath) {
        fprintf(stderr, "mkinitramfs: -r and -o are required\n");
        return 2;
    }

    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
        die("not a directory: ", root);

    add(".", root, S_IFDIR | (st.st_mode & 07777), 0, 0);
    walk(root, ".");

    for (int i = 0; i < n_nodes; i++) {
        char path[4001], type;
        unsigned mode, maj, min;
        char fmt[] = "%4000[^:]:%c:%o:%u:%u";

        if (sscanf(nodes[i].spec, fmt, path, &type, &mode, &maj, &min) != 5 ||
            (type != 'c' && type != 'b') || mode > 07777) {
            errno = 0;
            die("bad -n spec: ", nodes[i].spec);
        }
        for (size_t j = 0; j < n_entries; j++) {
            const char *p = entries[j].path;
            if (p[0] == '.' && p[1] == '/')
                p += 2;
            if (strcmp(p, path) == 0) {
                errno = 0;
                die("-n path already exists in rootfs: ", path);
            }
        }
        char apath[PATH_MAX];
        snprintf(apath, sizeof(apath), "./%s", path);
        add(apath, NULL, (type == 'c' ? S_IFCHR : S_IFBLK) | mode, maj, min);
    }

    qsort(entries, n_entries, sizeof(*entries), cmp_entry);

    FILE *out = fopen(outpath, "wb");
    if (!out)
        die("cannot create ", outpath);

    for (size_t i = 0; i < n_entries; i++) {
        /* newc stores names without the leading "./"; "." stays ".". */
        Entry e = entries[i];
        if (e.path[0] == '.' && e.path[1] == '/')
            e.path += 2;
        emit(out, &e, (unsigned long)i + 1);
    }

    Entry trailer = { "TRAILER!!!", NULL, 0, 0, 0 };
    emit(out, &trailer, 0);
    if (fclose(out) != 0)
        die("write failed: ", outpath);
    return 0;
}
