# Architecture and dependency plan (v2)

Decisions that are fixed: keep the existing Linux kernel and its drivers; original desktop; original `.aether`
package format and manager; Linux ELF compatibility first. Do not start a new kernel.

```
 apps (ELF, .aether packages)           Aether desktop clients: shell, settings, notify  (own toolkit)
        |                                         |  proto.h (SOCK_SEQPACKET + memfd)
  glibc 2.39 runtime (temporary)          aether-compositor = WM + software compositor
        |                                         |  KMS dumb buffers + evdev
 ------------------- Linux kernel (existing) + its drivers --------------------
        ^
 init/service manager (Aether)   aether-pm (Aether, libsodium)   aether-import (host tool)
```

## Modules and the dependencies between them
| Module | Depends on | Must NOT depend on |
|---|---|---|
| `src/pm/{util,version,verify,pkgfile,repo,resolve,fsops,db}` + `main.c` | libsodium (static, pinned) | GUI, compat |
| `src/gui/{gfx,font8x16,appmeta,proto,input,display,wm}` + `compositor.c` | kernel uapi headers only | package manager |
| `src/gui/{ui,shell,settings,notify}` | gfx, proto, appmeta | package manager code (Settings only *reads* the DB directory listing) |
| `tools/aether-import`, `tools/build-compat-repo.sh` | readelf, aether-pm binary | runtime |
| `tools/build-rootfs.sh` | the above, as black boxes (`COMPAT=1`, `GUI=1`) | - |

The GUI and the package manager share no code. The only link is a file layout convention:
`/usr/share/aether/apps/*.app` (a package may install one) and `var/lib/aether/pm/db` (read by Settings).

## Replacement path for temporary components
| Temporary | Replace by | Seam that makes it replaceable |
|---|---|---|
| Ubuntu kernel image + modules (test stand-in) | Aether `make kernel` build with `config/kernel.fragment` | `KERNEL=` variable; modules staged by `stage-gui-modules.sh` |
| Imported glibc 2.39 `.deb` contents | glibc built from source as native `.aether` packages | `config/compat-runtime.lock` -> package repo; consumers depend only on `libc6`/sonames |
| BusyBox userland | native tools, package by package | base rootfs is not special-cased by the PM |
| Software KMS dumb-buffer rendering | GPU-accelerated renderer behind `display.h` | `display_present()` is the only scanout call |
