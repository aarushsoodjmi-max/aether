# Aether OS — System Architecture

> **Document status:** Living architecture document  
> **Last updated:** 1 November 2026  
> **Project:** Aether OS  
> **Architecture direction:** An independently maintained Linux distribution and software ecosystem built on the upstream Linux kernel.

---

## 1. Vision and scope

Aether OS is an independent Linux distribution whose long-term goal is to own and maintain its userspace and the software ecosystem around it.

Aether will use the Linux kernel rather than attempting to build a kernel from scratch. The project will progressively replace temporary or third-party userspace components with Aether-maintained implementations where doing so is practical.

The intended result is a coherent operating system with its own:

- Core boot and system-service layer
- Shell, terminal and command-line utilities
- Package manager and software repositories
- Desktop environment and system settings
- Installer, recovery and system-management tools
- Developer SDK, build tools and applications
- Build, testing, release and update infrastructure

This is a long-term direction, not a claim that all of these components exist today.

### Design principles

1. **Linux foundation:** Track and integrate the upstream Linux kernel instead of maintaining a separate kernel implementation.
2. **Independent userspace:** Make Aether responsible for the user-facing and system-level environment above the kernel.
3. **Incremental replacement:** Use established components when necessary, then replace them individually without destabilising the whole system.
4. **Reproducible builds:** Make system images and packages buildable from documented source inputs.
5. **Security by design:** Verify packages, minimise privileged code, and make updates and recovery predictable.
6. **Hardware practicality:** Support real hardware as well as virtual machines; keep early development testable in QEMU.
7. **No host-system damage:** Build and test Aether in isolated directories, virtual machines or dedicated test environments. Never overwrite the developer's host OS.

---

## 2. High-level system architecture

Aether is organised into layers. Each layer has a defined responsibility and communicates with the layers around it through documented interfaces.

```text
+-----------------------------------------------------------+
|                    AETHER APPLICATIONS                    |
|       User applications | Developer tools | Utilities     |
+-----------------------------------------------------------+
|                    AETHER DESKTOP                          |
|  Desktop shell | Compositor | Settings | Display services |
+-----------------------------------------------------------+
|                   AETHER COMMAND                           |
|       Shell | Terminal | CLI tools | User utilities        |
+-----------------------------------------------------------+
|                   AETHER PACKAGES                          |
|     Package manager | Repositories | Updates | Signing     |
+-----------------------------------------------------------+
|                     AETHER CORE                            |
|       Init | Service management | System configuration    |
|       Device and session integration | System services     |
+-----------------------------------------------------------+
|                 USERSPACE FOUNDATION                       |
|    C library | Runtime | Core libraries | Base utilities   |
|        (initially selected/integrated components)           |
+-----------------------------------------------------------+
|                    LINUX KERNEL                             |
|       Process scheduling | Memory | Drivers | Networking    |
|       Filesystems | Security mechanisms | IPC               |
+-----------------------------------------------------------+
|                       HARDWARE                              |
|          CPU | RAM | Storage | GPU | Network | Peripherals  |
+-----------------------------------------------------------+
```

The diagram describes the intended logical architecture. It does not imply that every layer has been implemented.

### Boot flow

The intended boot sequence is:

1. Firmware initialises the machine and transfers control to a bootloader.
2. The bootloader loads the Linux kernel and the initial root filesystem.
3. The Linux kernel initialises hardware and its internal subsystems.
4. The kernel executes Aether's initial userspace process, `/init`.
5. Aether Core mounts and prepares system filesystems, discovers or integrates devices, and starts the required system services.
6. The system reaches a usable console or graphical login.
7. User sessions start the shell, desktop environment and applications as requested.

The current prototype has demonstrated the kernel-to-`/init`-to-shell portion of this sequence. The complete production boot and service-management flow remains future work.

---

## 3. Main subsystems

### 3.1 Linux kernel

**Responsibility:** Hardware abstraction and core operating-system mechanisms.

The Linux kernel provides:

- Process and thread scheduling
- Virtual memory and memory protection
- Device drivers
- Filesystem support
- Networking
- Inter-process communication
- Kernel security mechanisms

**Project decision:** Use and maintain a suitable upstream Linux kernel version. Kernel configuration, patches and release integration may be maintained by Aether, but writing a new kernel is not a project milestone.

**Current status:** A custom-built Linux kernel has successfully booted in QEMU with the Aether prototype root filesystem. The last recorded successful prototype boot reported Linux 7.2.10 on x86_64.

**Future work:**
- Define supported kernel versions and update policy.
- Maintain Aether kernel configuration and any necessary patches.
- Test boot, storage, networking and hardware support.
- Establish a repeatable kernel build and release process.

### 3.2 Aether Core

**Responsibility:** Bring up and manage the operating system after the kernel starts userspace.

Planned responsibilities:

- Initial process (`/init`)
- Mounting and preparing system filesystems
- System initialisation
- Service supervision and lifecycle management
- System configuration
- Device and session integration
- Logging and system diagnostics
- Shutdown and reboot coordination

**Current status:** A minimal prototype `/init` exists. It mounts `devtmpfs`, `proc` and `sysfs`, prints the Aether boot banner and starts a shell. A complete init and service-management system has not yet been implemented.

**Future work:**
- Separate early boot from normal system initialisation.
- Define service configuration and dependency handling.
- Implement service start, stop, restart and supervision.
- Add structured logging and failure recovery.
- Define system configuration interfaces.

### 3.3 Aether Command

**Responsibility:** Provide the command-line interface for users and administrators.

Planned components:

- Aether Shell
- Aether Terminal
- Core command-line utilities
- System administration commands
- Interactive command help and documentation

**Current status:** The prototype provides a working shell through BusyBox. This is a temporary foundation, not the final Aether command environment.

**Future work:**
- Design the Aether shell's command syntax and behaviour.
- Implement or adopt a terminal emulator.
- Build Aether-maintained command-line utilities incrementally.
- Establish scripting, completion, configuration and help conventions.
- Keep compatibility with common Linux command-line workflows where useful.

### 3.4 Aether Packages

**Responsibility:** Install, remove, update and verify software across Aether systems.

Planned components:

- Package manager CLI
- Package metadata and dependency resolver
- Package build recipes
- Repository metadata and indexes
- Package signing and verification
- Upgrade and rollback mechanisms

**Current status:** No Aether package manager or production repository system has been completed. Existing prototype components are being used to build the initial root filesystem.

**Future work:**
- Define a package format and metadata schema.
- Design dependency resolution and version constraints.
- Create a reproducible package build pipeline.
- Establish signed repositories and trusted signing keys.
- Support installation, removal, upgrades and integrity checks.
- Design safe system upgrades and recovery from failed updates.

### 3.5 Aether Desktop

**Responsibility:** Provide the graphical user experience.

Planned components:

- Desktop shell and session
- Wayland compositor
- Window and workspace management
- Settings application
- Notifications and status interfaces
- Display, input and session integration
- Desktop utilities

**Current status:** The Aether desktop environment has not been implemented.

**Future work:**
- Establish graphical-session requirements.
- Select or integrate temporary graphics and session components.
- Develop Aether desktop components incrementally.
- Implement settings and system-control interfaces.
- Test performance and usability on supported hardware.

### 3.6 Aether Development

**Responsibility:** Provide tools and infrastructure for building Aether and developing software for it.

Planned components:

- SDK and development headers
- Compiler and build-tool integration
- Package creation tools
- Debugging and diagnostic utilities
- Documentation and API references
- Application development conventions

**Current status:** Development currently relies on the host Linux toolchain and existing build utilities. A complete Aether SDK does not exist.

**Future work:**
- Define the supported toolchain and target ABI.
- Create SDK packages and build environments.
- Document system APIs and application interfaces.
- Add developer tooling and package validation.
- Make it possible to build third-party software for Aether.

### 3.7 Installer, recovery and system utilities

**Responsibility:** Make Aether installable, maintainable and recoverable on real systems.

**Current status:** No production installer or recovery environment has been completed.

**Future work:**
- Define disk and filesystem installation strategies.
- Build an installer with explicit target-disk confirmation.
- Provide boot repair and recovery tools.
- Support system diagnostics and logs.
- Design backup, rollback and update-recovery workflows.

---

## 4. Repository and source organisation

The project uses a component-oriented source layout. The intended top-level organisation is:

```text
aether/
├── docs/
│   ├── architecture.md
│   ├── development.md
│   └── decisions/
├── config/
│   ├── kernel/
│   ├── build/
│   └── release/
├── src/
│   ├── core/
│   ├── command/
│   ├── packages/
│   ├── desktop/
│   └── development/
├── tools/
│   ├── build/
│   ├── test/
│   └── image/
└── build/
    ├── kernel/
    ├── rootfs/
    └── images/
```

This is the intended logical organisation. Actual generated build artefacts, vendored sources and temporary development files may differ as the repository evolves. Generated files should not be treated as maintained source code.

### Directory responsibilities

| Path | Responsibility |
|---|---|
| `docs/` | Architecture, decisions, developer guides and project documentation |
| `config/` | Version-controlled build, kernel and release configuration |
| `src/core/` | Aether Core source |
| `src/command/` | Shell, terminal and command-line utilities |
| `src/packages/` | Package manager and package-related source |
| `src/desktop/` | Desktop environment source |
| `src/development/` | SDK and developer tooling |
| `tools/` | Build, testing and image-generation scripts |
| `build/` | Generated kernel, root filesystem and image outputs |

Third-party source trees should be clearly identified and version-pinned. Aether-maintained code should remain distinguishable from upstream projects and temporary integrations.

---

## 5. Current implementation status

**Status snapshot: 1 November 2026.** This section records the last known development state. It is not an automated CI report; a component marked as working refers to the recorded prototype state.

| Component | Current state | Notes |
|---|---|---|
| Linux kernel | Prototype boot successful | Custom-built Linux 7.2.10 booted in QEMU |
| Initial root filesystem | Prototype working | Minimal BusyBox-based root filesystem |
| Aether `/init` | Minimal implementation working | Mounts devtmpfs, proc and sysfs, then starts a shell |
| Console shell | Working in prototype | BusyBox shell; temporary implementation |
| QEMU testing | Used successfully | Main early boot-testing environment |
| BusyBox integration | In progress | A build issue involving the `tc` applet was addressed in configuration; successful completion after that change is not yet confirmed |
| Aether Core services | Not implemented | Beyond the minimal init script |
| Aether Shell | Not implemented | BusyBox is temporary |
| Aether Terminal | Not implemented | Planned |
| Aether package manager | Not implemented | Planned |
| Aether repositories | Not implemented | Planned |
| Aether Desktop | Not implemented | Planned |
| Aether SDK | Not implemented | Host toolchain currently used |
| Installer and recovery | Not implemented | Planned |
| Release infrastructure | Not implemented | Planned |

### Verified prototype behaviour

The recorded working prototype has:

- Booted a custom Linux kernel in QEMU.
- Loaded a minimal root filesystem.
- Executed `/init` successfully after fixing the earlier missing-init problem.
- Mounted the basic virtual filesystems needed by the prototype.
- Started a BusyBox shell.
- Accepted shell commands and displayed the root filesystem structure.

The prototype is an important proof of the boot path, but it is not yet a general-purpose or installable operating system.

### Development environment

The recorded development host is an Arch Linux x86_64 system. QEMU, GCC, Make and cpio are available in the development environment. The host is a build and test machine, not the Aether target operating system.

---

## 6. Build and release architecture

The intended build pipeline is:

```text
Source + Versioned Configuration
              |
              v
      Dependency Preparation
              |
              v
       Component Builds
              |
              v
       Package Assembly
              |
              v
       Root Filesystem
              |
              v
       Bootable System Image
              |
              v
      Automated Validation
              |
              v
       Versioned Release
```

### Build requirements

- Record upstream source versions and checksums.
- Keep build configuration under version control.
- Separate source files from generated artefacts.
- Make build steps scriptable and repeatable.
- Record build logs and failure information.
- Test bootable images in QEMU before hardware testing.
- Avoid modifying the host system during image creation.

### Release requirements

A future release process should include:

- Versioned kernel and userspace components.
- Package and repository metadata.
- Cryptographic package and repository verification.
- Release notes and known issues.
- Image integrity checks.
- Upgrade and recovery instructions.
- A documented support policy.

---

## 7. Security architecture

Security is a cross-cutting requirement rather than a standalone subsystem.

Planned security controls include:

1. **Package trust:** Verify signed repository metadata and package integrity before installation.
2. **Least privilege:** Keep system services and administrative operations at the minimum privilege required.
3. **Process isolation:** Use Linux security mechanisms and appropriate service restrictions.
4. **Update safety:** Validate updates and provide a recovery path for failed upgrades.
5. **Build integrity:** Pin sources, verify inputs and preserve build provenance.
6. **Secure defaults:** Prefer conservative service exposure and explicit user consent for destructive operations.
7. **Vulnerability response:** Track upstream kernel and userspace security advisories and publish fixes.

These are design requirements. They should not be interpreted as features already present in the prototype.

---

## 8. Compatibility and platform strategy

### Initial development target

- Architecture: x86_64
- Test platform: QEMU virtual machine
- Kernel: Upstream Linux with Aether-maintained configuration
- Initial userspace: Minimal BusyBox-based environment

### Future platform work

- Broader x86_64 hardware support
- Storage, networking, graphics and input-device validation
- Bootloader and firmware compatibility
- Additional architectures only after the core system and build pipeline are stable

The project should define a supported-hardware policy before making compatibility claims.

---

## 9. Future development roadmap

The roadmap is ordered by dependency. Dates are intentionally not promised; completion should depend on working, testable milestones.

### Phase 0 — Bootable foundation

**Goal:** Maintain a repeatable minimal Aether boot.

- [x] Establish the project direction and component boundaries.
- [x] Build and boot a Linux kernel in QEMU.
- [x] Create a minimal root filesystem.
- [x] Fix the initial missing-`/init` boot failure.
- [x] Run a minimal Aether init script.
- [x] Reach a working BusyBox shell.
- [ ] Confirm a clean, repeatable BusyBox build after the `tc` configuration change.
- [ ] Automate kernel and initramfs builds.
- [ ] Document a reproducible QEMU boot procedure.

### Phase 1 — Aether Core

**Goal:** Replace the prototype init script with a maintainable system foundation.

- Design the early-boot and normal-boot stages.
- Implement Aether Core init.
- Define service configuration and supervision.
- Add logging, shutdown and reboot handling.
- Define system configuration interfaces.
- Keep the system bootable throughout the transition.

### Phase 2 — Base userspace and Aether Command

**Goal:** Establish a consistent command-line environment.

- Evaluate and pin the initial userspace foundation.
- Define shell requirements and command conventions.
- Develop the Aether shell.
- Introduce Aether-maintained utilities incrementally.
- Establish terminal and scripting conventions.
- Document compatibility expectations.

### Phase 3 — Aether Packages

**Goal:** Make the system independently maintainable through its own package workflow.

- Design package metadata and format.
- Implement package installation and removal.
- Add dependency resolution.
- Build package repositories.
- Introduce signing and verification.
- Add upgrades, rollback and package validation.

### Phase 4 — System integration

**Goal:** Move from a minimal boot prototype to a usable base operating system.

- Integrate networking and storage configuration.
- Improve device management.
- Add user and session management.
- Establish service defaults.
- Expand QEMU and physical-hardware testing.
- Create automated system tests.

### Phase 5 — Aether Desktop

**Goal:** Provide a coherent graphical environment.

- Establish the graphical-session architecture.
- Integrate or develop a Wayland compositor.
- Build the desktop shell and settings.
- Add notifications and system utilities.
- Validate graphics, input and display behaviour.
- Package desktop components through Aether Packages.

### Phase 6 — Installer and recovery

**Goal:** Make Aether practical to install and maintain.

- Build bootable installation media.
- Implement safe disk selection and installation.
- Add boot repair and recovery workflows.
- Validate installation and upgrades in virtual machines.
- Test supported physical hardware.

### Phase 7 — Developer ecosystem

**Goal:** Enable development and distribution of software for Aether.

- Publish SDK and development headers.
- Provide package creation tools.
- Document system APIs and application conventions.
- Add debugging and profiling utilities.
- Support third-party application packaging.

### Phase 8 — Stable distribution

**Goal:** Establish a maintainable public operating-system release.

- Define release and support channels.
- Automate reproducible builds and validation.
- Publish signed repositories and images.
- Establish security update procedures.
- Maintain release documentation and upgrade paths.
- Build a sustainable contribution and maintenance process.

---

## 10. Deferred language project

Aether has a longer-term language idea: an original programming language intended to combine Python-like simplicity with C-like control.

The concept includes:

- Original syntax and brace conventions
- Zero-based indexing
- A configurable unsafe-runtime model
- High-level usability with lower-level control

**Current status:** Deferred. The language is not part of the initial operating-system boot or userspace milestone.

It should be reconsidered only after Aether Core, the base userspace, package infrastructure and development toolchain are sufficiently stable. The OS should not depend on an unfinished custom language.

---

## 11. Architectural decisions and open questions

The following decisions should be recorded as the project develops.

| Area | Current direction | Still to decide |
|---|---|---|
| Kernel | Upstream Linux | Long-term kernel tracking and patch policy |
| Initial platform | x86_64 | Additional architectures |
| Early testing | QEMU | Automated hardware test coverage |
| Initial userspace | BusyBox prototype | Long-term core utilities |
| C library | Not yet fixed | libc choice and ABI policy |
| Init and services | Aether Core | Service model and configuration format |
| Shell | Aether-maintained in the future | Syntax and scripting compatibility |
| Packages | Aether Packages | Package format and dependency model |
| Repositories | Aether-maintained in the future | Metadata, signing and mirror design |
| Desktop | Aether Desktop | Compositor and graphical stack |
| Filesystem layout | Linux-compatible foundation | Final system layout and update model |
| Custom language | Deferred | Revisit after the OS foundation matures |

Decisions should be documented with their reasoning, alternatives and migration implications. Avoid committing to complex infrastructure before a working requirement exists.

---

## 12. Definition of a mature Aether OS

Aether should be considered a mature distribution only when it can reliably:

- Boot and initialise on its documented supported hardware.
- Manage system services and user sessions.
- Provide a usable command-line environment.
- Install, verify, update and remove packages.
- Recover from failed updates and common boot failures.
- Provide a usable desktop experience if the desktop edition is released.
- Build system images and packages through documented infrastructure.
- Deliver security fixes and maintain supported releases.
- Support developers and third-party applications through stable interfaces.

A successful QEMU boot is the foundation of this work—not the finish line.

---

## 13. Project status summary

Aether has crossed the first meaningful technical milestone: a custom-built Linux kernel can boot a minimal root filesystem, execute the Aether init script and reach a working shell in QEMU.

The next priority is to make that foundation repeatable, then build Aether Core and a maintainable base userspace. The package manager, repositories, shell, desktop, installer, SDK and release infrastructure follow in dependency order.

The central architectural commitment remains unchanged:

**Aether uses Linux as its kernel foundation and builds an independently maintained operating-system ecosystem above it.**
