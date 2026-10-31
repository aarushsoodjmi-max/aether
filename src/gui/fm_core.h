/* File-manager core for aether-files: real directory listing, path handling and formatting. No drawing, no UI: unit-tested on its own.
 * Everything is read from the actual filesystem with lstat/stat/opendir; errors are reported with the system's own message. */
#ifndef AETHER_FM_CORE_H
#define AETHER_FM_CORE_H
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
    char name[256]; char link[256];
    int is_dir;        /* a directory, or a symlink that resolves to one (both can be opened) */
    int is_link, broken;
    mode_t mode; off_t size; time_t mtime; uid_t uid; gid_t gid;
} FmEntry;
typedef struct { char path[4096]; FmEntry *e; int n; int hidden_skipped; } FmDir;

/* Load `path`. 0 on success (d is replaced). On failure returns -1, writes strerror text to err, and leaves d untouched. */
int fm_load(FmDir *d, const char *path, int show_hidden, char *err, size_t errlen);
void fm_free(FmDir *d);
void fm_normalize(char *out, size_t n, const char *path);          /* absolute path; collapses //, resolves . and .. textually (no symlink resolution) */
void fm_join(char *out, size_t n, const char *dir, const char *name);
void fm_parent(char *out, size_t n, const char *path);             /* "/" stays "/" */
void fm_format_size(char *out, size_t n, long long bytes);         /* "0 B", "1.5 KiB", ... */
void fm_format_mode(char out[11], mode_t m);                       /* "drwxr-xr-x" */
void fm_format_time(char *out, size_t n, time_t t);                /* "YYYY-MM-DD HH:MM" (UTC) */
const char *fm_kind(const FmEntry *e);                             /* "Folder", "Symbolic link", "File", "Device", ... */
/* Read up to cap-1 bytes of a regular file for preview. Returns bytes read, -1 on error (err filled). *is_text = 1 if it looks like text. */
long fm_preview(const char *path, char *out, size_t cap, int *is_text, char *err, size_t errlen);
#endif
