> Superseded in part by `architecture-v2.md` (desktop, `.aether` package manager, ELF compatibility). Evidence status: `STATUS.md`.

# Aether OS

## Goal
An independently maintained Linux distribution and software ecosystem.

## Foundation
- Linux kernel
- Independent userspace
- Independent build and release infrastructure

## Components
- Aether Core: boot, init, system services
- Aether Command: shell, terminal, utilities
- Aether Packages: package manager and repositories
- Aether Desktop: compositor, shell, settings
- Aether Development: SDK, build tools, applications

## First milestone
Boot a custom-built Linux kernel in QEMU
with a minimal root filesystem and Aether init.

## Constraints
- Never overwrite the host OS
- Keep builds reproducible
- Replace temporary components incrementally
