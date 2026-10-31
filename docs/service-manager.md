# Aether Service Manager

PID 1 is `/init` (src/core/init.c, built with src/core/service.c and
src/core/service_order.c). It is a small supervisor, not a shell script.

## Service files

`/etc/aether/services/*.conf` (sources: `config/services/`). One `KEY=VALUE`
per line; `#` comments and blank lines allowed. Unknown or duplicate keys,
over-long lines and invalid values reject the whole file (it is skipped and
reported; boot continues).

| Key            | Required | Meaning |
|----------------|----------|---------|
| `NAME`         | yes | `[A-Za-z0-9_-]`, max 63 chars, unique |
| `EXEC`         | yes | absolute path of the program (no arguments; use a wrapper script) |
| `RESTART`      | no  | `always` (default) or `never` |
| `MAX_RESTARTS` | no  | 0-100, default 5 |
| `REQUIRES`     | no  | comma-separated service names, max 8 |

`console` is special: it owns the system console (runs under
`setsid cttyhack`). Every other service has stdin from /dev/null and
stdout/stderr appended to `/var/log/aether.log`. Nothing may require `console`;
it is always started last.

## Behaviour

* Start order: dependencies first (topological), console last. A service whose
  required service failed to start is not started. `REQUIRES` means *started
  before*, not *ready*.
* Restart: after an exit the service restarts 1 s later while
  `restart_count < MAX_RESTARTS`. The budget resets after 60 s of uptime.
  Exit status / signal is logged.
* Recovery: if there is no valid console, the dependency graph is invalid, or
  the console exhausts its restarts, init starts `/bin/sh` as `recovery` on the
  console. Exit of the recovery shell powers the system off.
  (Invalid graph: only the recovery shell is started.)
* Shutdown: services are stopped in reverse start order. Each gets SIGTERM to
  its process group, then SIGKILL after 5 s. Stray processes are killed, then
  `sync()` and `reboot(2)`.
* Control signals to PID 1: SIGTERM / SIGINT / SIGUSR2 power off, SIGUSR1
  reboots. `aetherctl poweroff|reboot` (also `/bin/poweroff`, `/bin/reboot`)
  sends these and refuses to run when PID 1 is not the Aether init.

## Runtime control

`aetherctl list | status NAME | start NAME | stop NAME | restart NAME` talk to
init over a root-only UNIX socket (`/run/aether.sock`, peer uid checked). A
service stopped by the operator is not auto-restarted; `start` resets its
restart budget. The console service cannot be stopped. `stop`/`restart` block
init for up to the 5 s grace period of that service.

## Filesystems

`/etc/aether/fstab` (`SOURCE TARGET TYPE [OPTIONS]`, `#` comments) is mounted
in order after the pseudo filesystems; options `ro nosuid nodev noexec noatime
sync` are flags, anything else is passed to the filesystem as data. Target
directories are created. Invalid lines and failed mounts are reported and
skipped. At shutdown, after services stop, mounts are undone in reverse order
(a busy mount is remounted read-only). Switching the root to a disk
(`switch_root`) is not implemented yet.

## Tools

* `aetherctl check [DIR]` validates a service directory (also run by
  `tools/build-rootfs.sh` before packing).
* `service-status NAME` reads `/run/aether-NAME.pid` and `/proc`.

## Not implemented yet

Readiness notification, per-service arguments/environment/users, log rotation,
service control at runtime (start/stop/restart by name), filesystem remounts
and unmounting at shutdown (the root is an initramfs).
