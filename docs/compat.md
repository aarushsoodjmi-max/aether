# Linux ELF compatibility

## Audit of the supplied project (before this work)
| Item | Finding |
|---|---|
| Root filesystem | initramfs with BusyBox 1.37.0 **statically linked** |
| libc / dynamic linker | none: no `libc.so.6`, no `ld-linux-x86-64.so.2` anywhere |
| Layout | `/bin`, `/sbin` as real directories; no `/lib`, `/lib64`, `/usr/lib`, no `ld.so.cache`, no NSS config |
| Consequence | **No dynamically linked ELF program could start.** Static BusyBox gives no application compatibility. |

## libc / ABI decision
| Option | Verdict |
|---|---|
| **glibc (chosen)** | Almost all prebuilt Linux binaries target glibc (`PT_INTERP=/lib64/ld-linux-x86-64.so.2`, symbol versions `GLIBC_2.x`, NSS, gconv). Only glibc gives compatibility with existing binaries. |
| musl | Smaller, but binaries built for glibc do not run on it (different loader, no symbol versioning, no NSS). Rejected for the compatibility goal. Can coexist later only as a *separate* ABI, never mixed in one root. |
| static-only | What exists today; no compatibility. |

**Initial ABI: x86_64, glibc 2.39** (Ubuntu 24.04 build, imported from pinned `.deb`s, SHA-512 verified,
`config/compat-runtime.lock`). This is a **temporary upstream import**: it is not built from source by Aether and the
URLs are pinned by hash but not mirrored (superseded archive versions can disappear).

## Layout
usr-merged FHS: `/bin /sbin /lib /lib64` are symlinks into `/usr/...`. Packages may not own those aliases
(`path_allowed_for_package`), which is what prevents a package from silently replacing the loader through `/lib64`.
`/etc/ld.so.cache` is generated at image-build time by the pinned static `ldconfig.real`; there is **no post-install
trigger yet**, so installing a package that adds a library at runtime does not refresh the cache (default search dirs still work).

## Library and dependency resolution
- Every `.aether` package declares `libc glibc-X.Y | none`; the resolver refuses to put two different glibc ABIs in one system.
- Shared-library needs are explicit: `needs-soname` / `provides-soname`, derived automatically by `aether-import` from `DT_NEEDED`/`DT_SONAME`/`PT_INTERP`.
- The resolver treats a soname like a virtual package, so `app` pulls in whichever package provides `libfoo.so.2`.
- File ownership is tracked; installing over an existing, unowned or foreign file is refused. Core libraries cannot be overwritten silently.
- `aether-import package` refuses a package whose ELF files need a newer `GLIBC_x.y` than the package declares.

## Import method (repeatable)
```
tools/aether-import inspect PATH --root ROOT     # interp, needed, soname, runpath, max GLIBC_ version; resolves against ROOT
tools/aether-import package --name N --version V --libc glibc-2.39 --tree DIR --out-dir OUT --pm build/bin/aether-pm --key K
tools/build-compat-repo.sh                       # runtime + tests -> signed repository build/compat/repo
make compat-initramfs                            # installs them INTO the rootfs with aether-pm itself
```
`inspect` was verified to pass resolvable binaries and flag the deliberately broken `missing_dep` binary.

## Test suite (small ELF programs first) - `tests/compat/`
17 checks, all pass in a chroot **and** in QEMU: dynamic hello; argv/env/`execve`; signals (handlers, masks, pending,
`sigsuspend`, `SIGCHLD`, `SIGKILL`, `SIGPIPE`, SEGV on an alternate stack, real-time signals with payload); fork/exec/pipes/
`posix_spawn`/shebang/EACCES-without-x-bit/sessions/orphan reparenting; pthreads, mutex/condvar/barrier/semaphore/TLS; FHS paths
(`/proc/self/*`, `/dev/*`, mmap, rename, symlink loops, privilege drop); epoll/eventfd/timerfd/inotify/`SCM_RIGHTS`/memfd/getrandom;
NSS (`getpwnam`, `getaddrinfo`), locale, **iconv/gconv**, time zones, libm; C++ (exceptions through libgcc_s, `std::thread`);
a static glibc binary; `DT_NEEDED` + `RUNPATH $ORIGIN`; `dlopen`; `LD_PRELOAD`; loader `--library-path`/`--list`/`LD_DEBUG`;
a missing dependency (must exit 127 with the loader's message, not crash).

## Honest limits - what is NOT compatible or not shown
- **No complex application has been run.** Only the 14 small test programs plus BusyBox. Do not read this as "Linux apps run".
- **Linux GUI applications cannot display**: there is no X11 server, no XWayland, no Wayland. A Wayland-compat layer on top of the Aether compositor is the realistic next step; not started.
- 32-bit ELF (`i386`) not tested; musl-linked and non-x86_64 binaries unsupported.
- No `ld.so.cache` refresh trigger; no locale data beyond `C`; no tzdata (UTC only); `/etc/resolv.conf` not provisioned.
- `ldconfig` from `libc-bin` is a shell wrapper that needs `/bin/sh` (BusyBox) - the dependency is real but not declared yet.
- Kernel features beyond what the Ubuntu test kernel provides were not checked; Aether's own kernel config is unverified against this userspace.
