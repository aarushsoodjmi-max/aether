# Aether Core configuration (`src/core/config.{c,h}`)

> Design and implementation notes for the core configuration foundation. Companion to `architecture.md`.

First configuration-management foundation. `/etc/aether/` was already the home of the service files
(`services/*.conf`), `fstab` and `network/*.conf`; this adds one small file per core area next to them and a
module that parses, validates and defaults them. It is plain C (libc only), links into init and into the
`aether-config` tool, and has no Python or other runtime dependency.

## Files

| File | Keys | Used by init today? |
|---|---|---|
| `/etc/aether/system.conf` | `HOSTNAME`, `SYSTEM_NAME` | **yes**: `sethostname(2)`, boot banner |
| `/etc/aether/boot.conf` | `BANNER`, `MOUNT_FSTAB` | **yes**: banner on/off, mount `/etc/aether/fstab` on/off |
| `/etc/aether/services.conf` | `DEFAULT_RESTART`, `DEFAULT_MAX_RESTARTS`, `STOP_GRACE_MS` | no (parsed, validated and reported only) |
| `/etc/aether/logging.conf` | `FILE`, `LEVEL`, `MAX_SIZE_KB`, `ROTATE_KEEP` | no (parsed, validated and reported only) |

Format: `KEY=VALUE`, one per line, the same style as the service files. Blank lines are ignored; `#` starts a
comment only at the beginning of a line (so `SYSTEM_NAME=Aether #1` keeps its `#`). Keys are case-sensitive.
There is no quoting and no inline comment.

| Setting | Default | Valid values |
|---|---|---|
| `HOSTNAME` | `aether` | 1-63 chars, letters/digits/`-`/`.`; labels must not start or end with `-` |
| `SYSTEM_NAME` | `Aether OS` | 1-64 printable ASCII chars, no leading/trailing space |
| `BANNER`, `MOUNT_FSTAB` | `yes` | `yes` or `no` |
| `DEFAULT_RESTART` | `always` | `always` or `never` |
| `DEFAULT_MAX_RESTARTS` | `5` | 0-100 |
| `STOP_GRACE_MS` | `5000` | 100-60000 |
| `FILE` (logging) | `/var/log/aether.log` | absolute path of letters/digits/`.`/`_`/`-`/`/`, no `.`/`..` components, at most 127 chars |
| `LEVEL` | `info` | `error`, `warn`, `info`, `debug` |
| `MAX_SIZE_KB` | `1024` | 0 (no limit) to 1048576 |
| `ROTATE_KEEP` | `3` | 0-20 |

The defaults equal what init did before configuration existed (hostname `aether`, the same banner, fstab mounted,
the service manager's own restart defaults, `AETHER_GRACE_MS`, `/var/log/aether.log`). `tests/test_config.c`
checks the restart defaults against `service_load()`; `STOP_GRACE_MS` mirrors the `AETHER_GRACE_MS` macro in
`init.c` by convention only (init does not read it yet).

## Failure model (what "never crash the boot" means here)

* `aether_config_load()` has no failure return. It always leaves a fully valid `AetherConfig` (defaults first,
  files on top) and returns the number of problems found.
* **Per-setting fallback.** A bad line (no `=`, unknown key, bad value, duplicate key, line over 255 bytes) is
  skipped and reported; every other line in the file still applies. A duplicate keeps the first value.
* **Whole-file rejection** only for files that cannot be read safely: not a regular file (directory, FIFO,
  device; a FIFO is never blocked on), over 64 KiB, unreadable, or containing a NUL byte. Nothing from such a
  file is applied.
* A missing file is normal, not a problem: defaults apply and nothing is printed.
* Problems are collected in an `AetherConfigReport` (file, line, message; at most 32 stored, all counted) and
  printed by init to the console as `Aether: system.conf:1: HOSTNAME: ...; default kept`. Text copied from a file
  into a message is sanitised so a hostile file cannot inject terminal escape sequences.
* The library never writes while loading and never prints on its own.

## Never overwriting user configuration

* Boot only reads. Missing files are *not* created at boot; in-memory defaults are used.
* `aether_config_install_defaults()` / `aether-config init` create a missing file with
  `open(O_CREAT|O_EXCL)`. If anything already exists at the path (file, directory, even a dangling symlink) it is
  left alone and reported as kept; symlinks are never written through. A file that fails to write completely is
  removed again, but only if that very call created it.
* `tools/build-rootfs.sh` uses `aether-config init` on the freshly assembled rootfs, then `aether-config check`
  before packing (same pattern as `aetherctl check` for services). The generated files are the single source of
  the shipped defaults; there is no second copy in `config/`.

## Init integration (`src/core/init.c`)

Right after devtmpfs/proc/sysfs, init calls `aether_config_load(AETHER_CONFIG_DIR, ...)` and prints the issues.
Then: `sethostname()` uses `HOSTNAME`; `/etc/aether/fstab` is mounted only if `MOUNT_FSTAB=yes`; the banner is
printed only if `BANNER=yes`, with `SYSTEM_NAME` upper-cased as its title line and `HOSTNAME` on the `Hostname:`
line. With default configuration the console output is the same set of lines as before. The service manager,
console/recovery logic and shutdown path are untouched.

## `aether-config`

`aether-config check [DIR]` (exit 1 if any problem), `show [DIR]` (effective values as `section.key=value`),
`get section.key [DIR]`, `init [DIR]` (create missing files only). `DIR` defaults to `/etc/aether`. It is
installed as `/bin/aether-config`. The same functions (`aether_config_parse`, `_format`, `_get`, the
`aether_config_valid_*` validators, `_install_defaults`) are the interface for a future CLI or graphical Settings
tool; this task adds none of those.

## Tests and evidence (what was actually run)

| Level | Result |
|---|---|
| compiled | clean with the project's `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` |
| unit (`tests/test_config.c`, `make unit`) | 73 checks: defaults, validators, parsing, invalid input, per-setting fallback, limits, NUL/oversized/FIFO/directory/symlink files, report bounds and sanitising, format/get, no-overwrite install. Also run once under ASan+UBSan and against a one-off 300 000-input random parser fuzz (config always valid afterwards); those two runs are ad hoc and not wired into `make` |
| CLI (`tests/test_config_cli.sh`) | 23 checks on throw-away directories |
| integration (`tests/integration-init.sh`) | 50 checks, 7 new (section 2e): the real test-build init as PID 1 in a PID+mount+UTS namespace applies `HOSTNAME`/`SYSTEM_NAME`/`BANNER`/`MOUNT_FSTAB`, survives missing/invalid/unusable config, and does not rewrite the user's file. The 43 existing checks still pass |
| QEMU / real hardware | **not run** for this change |

## Not implemented (do not assume)

* `services.conf` and `logging.conf` are validated but not consumed: the service manager still uses its compiled-in
  defaults and logs to `/var/log/aether.log` as before.
* No API to *modify* configuration (comment-preserving edit, atomic replace); `init` only creates missing files.
* The hostname is applied once at boot; there is no reload. `/etc/hosts` and `/etc/os-release` (written by the
  `COMPAT=1` image build) and the legacy shell `src/core/init` still contain `aether` literally and are not
  derived from `system.conf`.
* No per-service overrides, includes, or drop-in directories.
