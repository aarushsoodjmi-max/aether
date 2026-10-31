# Kernel: what was actually used

The supplied project builds a kernel from source (`make kernel`, `config/kernel.fragment`). No kernel source is available in the
sandbox, so that path remains **unrun and unverified**.
For every QEMU test a **prebuilt Ubuntu `7.0.0-38-generic`** image and its modules were downloaded by `tools/setup-test-env.sh`
(isolated apt state, extracted with `dpkg -x`, host untouched) and used as a stand-in. Consequences found while doing so:
- Ubuntu builds the GPU (`bochs`, `virtio-gpu`) and PS/2 mouse (`psmouse`) drivers as **modules**; the session script loads them with `busybox insmod`.
  A custom Aether kernel should build the needed ones in (add to `config/kernel.fragment`) and drop this step.
- The kernel/module versions must match exactly; `tools/stage-gui-modules.sh` takes them from the same package set.
- QEMU runs without KVM (TCG), so timings are meaningless for the laptop.
