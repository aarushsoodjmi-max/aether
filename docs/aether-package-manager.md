# `.aether` package format and `aether-pm`

## Format v1 (strict, versioned)
```
"AETHERPK" | u32le format=1 | u32le meta_len | keyid[8] | Ed25519 signature[64] | meta (canonical text) | payload
```
Total length must equal `88 + meta_len + sum(file sizes)` exactly: truncation and trailing data are rejected.
The signature covers `"aether-pkg-v1\0" || meta`, and `meta` contains the SHA-256 of every file, so one signature authenticates the whole package.
Metadata (one `key value` per line, single spaces, printable ASCII only, unknown keys rejected):
`name version arch(x86_64|any) libc(none|glibc-X.Y) summary essential depends conflicts provides needs-soname provides-soname`
and the manifest: `dir MODE PATH`, `file MODE SIZE SHA256 PATH`, `link TARGET PATH`.
Versions use the dpkg ordering algorithm. Constraints: `<< <= = >= >>`. Builds are deterministic (byte-identical for identical input).
Rejected at parse time: absolute paths, `..`/empty components, setuid/setgid/sticky modes, duplicate paths, devices/fifos/sockets,
and any path under `bin sbin lib lib64` (usr-merge aliases), `var/lib/aether/pm` (database), `etc/aether/keys` (trust store), `proc sys dev run tmp`.

## Trust model
- Cryptography is libsodium only: Ed25519 signatures, SHA-256. Domain separation prefixes distinguish package and index signatures.
- Trusted keys are the `*.pub` files in `etc/aether/keys/`. A repository is `INDEX` + `INDEX.sig`; the index lists every package's metadata, size and SHA-256.
- `update` verifies the index signature, then refuses an index older than the last one seen (replay/rollback protection, timestamp stored per repo; `--force` overrides).
- `install` checks: download size+SHA-256 against the signed index -> package metadata equals the index entry -> embedded signature (if present) with a trusted key -> every file hash again while staging.
- Local files must carry a trusted signature unless `--allow-unsigned` (loud warning).
- Not done: key revocation/rotation, index expiry, binding a key to a specific repository, a "package signed by A may only provide names X" policy.

## Commands
Unprivileged: `inspect FILE [--files]`, `verify FILE`, `list`, `info`, `files`, `owner PATH`, `check [NAME]` (content hashes, modes, types).
Authoring: `keygen`, `build INFO TREE OUT [--key]`, `index DIR --name N --key K [--timestamp T]`.
Privileged (root for `--root /`): `update`, `install TARGET...` (name, `name=version`, or a file), `upgrade`, `remove [--force]`, `recover`. `--dry-run` shows the plan.

## Dependency resolution (`resolve.c`, pure logic)
Backtracking solver over versioned depends, provides (virtual names), conflicts, soname needs, and libc-ABI exclusion; prefers the installed version, then the newest;
bounded search budget; explains failures (`nothing provides shared library X`, `libc ABI mismatch`, `A conflicts with B`). Removal is refused while anything still requires the package or if it is `essential`.
Not supported: "or" alternatives, optional dependencies, multiple-arch.

## Transactions and recovery (`fsops.c`)
1. Every path is opened component-by-component with `openat(O_NOFOLLOW)`: a package can never write through a symlink. Pre-flight refuses overwrites of unowned or foreign-owned files and file collisions inside one transaction.
2. New content is staged as `NAME.aether-new` (hash-checked, fsync'd). Replaced objects are renamed to `NAME.aether-old` (same directory => atomic).
3. A write-ahead journal records intent before each step. Until `txn/COMMIT` exists recovery **rolls back**; after it, recovery **rolls forward** (cleanup + directory removal). Package DB writes use the same mechanism, so database and files cannot diverge.
4. Every mutating command first runs recovery. The process ignores SIGINT/SIGTERM/SIGHUP during the transaction.
5. Evidence: the process is killed (`_exit(99)`) after every single journal record of an install (18 points), upgrade (22) and remove (11); after `recover`, `check` is clean, no staging files remain, and the installed set is exactly the old or exactly the new one in all 51 cases.

## Rollback strategy
Automatic rollback of any interrupted/failed transaction (above). There is **no snapshot rollback of a committed upgrade**; to go back, reinstall an older version that is still in a configured repository (`aether-pm install NAME=OLDVER`) - the package cache keeps downloaded files.

## Known limitations
No compression; no maintainer scripts/triggers (so no loader-cache refresh, no service enable); no config-file preservation (an upgrade replaces files); `check` does not detect unowned extra files; `--root /` safety checks assume a single admin; `file://` repositories are tested, **`http(s)://` (via wget/curl, no shell) is not**; files are root-owned semantics only; `syncfs` is used at commit (coarse but safe). The legacy `aether-pkg` (`.apkg`, newc cpio) is still in the tree for the bootstrap image and will be retired once the base system is packaged.
