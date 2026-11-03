# Aether Init (PID 1) - implementation and verified status

> Companion to `architecture.md` (section 3.2, Aether Core) and `service-manager.md`. Evidence levels: `STATUS.md`.
> The core configuration loaded by `config.c` (hostname, banner, fstab switch) is applied after the boot filesystems
> and console are set up; it is not changed by the init hardening described here.


Source: `src/core/init.c` (PID 1, event loop, shutdown), `src/core/initlib.c` (PID 1-independent helpers, unit-tested),
`src/core/service*.c` (service files, ordering, spawning). The service manager itself is described in `service-manager.md`.
The original shell-script prototype is kept unchanged at `src/core/init` and shipped as `/init.legacy`.

## Process lifecycle
1. Refuse to run unless PID 1 (exit 1 with a message; this is the only exit before the event loop).
2. Console: fds 0-2 that the kernel did not provide are opened on `/dev/console` (`O_NOCTTY`); after `/dev` is mounted the step is repeated with `/dev/null` as fallback so a later `open()` can never land on fd 0-2. Failures go to `/dev/kmsg`.
3. Boot filesystems (`ainit_boot_mounts`): `/dev` (devtmpfs), `/proc`, `/sys`, `/run` (tmpfs). Each mount point is created if missing; a target that is already a mount point of the right type (st_dev differs from its parent, statfs magic matches) is not mounted again; a target occupied by another filesystem is reported and left alone. `/dev` and `/proc` are critical.
4. Core configuration (`config.c`, never fatal), hostname, signal mask (SIGCHLD, SIGTERM, SIGINT, SIGUSR1, SIGUSR2 blocked and read through a `signalfd`), `/etc/aether/fstab`, banner.
5. Service load/order (`service-manager.md`), start, then the poll loop: signalfd, control socket, delayed restarts, `waitpid(-1, WNOHANG)`.
6. Shutdown on SIGTERM/SIGINT/SIGUSR2 (power off) or SIGUSR1 (reboot): stop services in reverse order (SIGTERM to the process group, SIGKILL after the grace period), kill and reap strays, `sync`, unmount, `reboot(2)`.

## Signal handling
No signal handler is installed at all: every signal PID 1 cares about is blocked and consumed synchronously from the signalfd, so there is no async-signal-safety surface. Signal -> action mapping is `ainit_action_for_signal()`.

## Shell supervision and recovery
The `console` service (BusyBox `sh` under `setsid cttyhack`) is never `exec`ed over PID 1. After it exits, `ainit_restart_decide()` grants at most `MAX_RESTARTS` restarts, 1 s apart; the budget only resets after 60 s of uptime, so a shell that dies instantly cannot loop forever. When the budget is exhausted, or when there is no valid console, an invalid dependency graph, or a critical mount failure, init starts `/bin/sh` as `recovery` on the console. If the recovery shell cannot be spawned, init keeps running and retries every 5 s. If it cannot be *executed* (exit 126/127) or exits, init reports it and powers off (existing, documented behaviour).
If `signalfd()` is unavailable, `emergency_supervise()` runs only a recovery shell (bounded restarts, then PID 1 idles) and still reacts to the power signals via `sigtimedwait()`. PID 1 does not exit on any initialisation error.

## Build integration
`make` builds `build/bin/aether-init` (static, with `initlib.c`); `tools/build-rootfs.sh` installs it as `/init` and the prototype as `/init.legacy`. Select at boot: `tools/run-qemu.sh` (`INIT=aether` default, `INIT=legacy`) or `EXTRA="rdinit=/init.legacy"`. Static linking works in the build environment (glibc static); BusyBox is the existing pinned static build.

## Verified status
Evidence levels as in `STATUS.md`. Run on the development host (Linux 6.18 kernel, not the Aether kernel), `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror`.

| Item | compiled | unit | integration (namespace) | QEMU |
|---|---|---|---|---|
| Mount-point creation, already-mounted detection, mismatch, failure (`initlib`) | yes | yes (57 checks incl. ASan/UBSan, real mounts in a private mount namespace) | yes (`/sys`, `/run` created and mounted, `/proc` not remounted, tmpfs stand-in for `/dev`) | **no** |
| Real `devtmpfs` mount by PID 1 | yes | - | **no** (test builds mount a tmpfs: a devtmpfs would expose host devices) | **no** |
| Console attach / fallback / failure | yes | yes (forked child) | **no** (test build skips it: it would touch the host console) | **no** |
| Child reaping, orphans, no zombies | yes | yes | yes | no |
| Signal -> action mapping | yes | yes | yes (SIGTERM, `aetherctl poweroff/reboot`) | no |
| Restart policy / crash-loop limit | yes | yes | yes (service and console loops, recovery fallback) | no |
| Critical mount failure -> recovery only | yes | - | yes (`/dev` made unmountable) | no |
| Emergency supervisor (no signalfd) | yes | - | yes, via a simulated failure (`AETHER_FORCE_EMERGENCY`) | no |
| `/init.legacy` shipped and unchanged | - | - | file checked only | **not booted** |
| Initramfs contains the new `/init` and `/init.legacy` | - | - | yes (archive parsed) | - |

`tests/qemu-init-test.sh` (`make qemu-init-test`) boots both inits in QEMU. **It has not been run**: no QEMU and no kernel were available. Nothing in this section is hardware-tested.

## Remaining limitations
- No QEMU result exists for this change; the real `devtmpfs`, console and shell paths are verified only by reading code and by the namespace stand-ins above.
- The recovery shell exiting powers the system off; init does not respawn it.
- No `switch_root`, no readiness notification, no per-service arguments (see `service-manager.md`).
- The `aether-pm` suites were not run for this change (its pinned libsodium could not be downloaded in the development sandbox); `aether-pm` is unrelated to init.
