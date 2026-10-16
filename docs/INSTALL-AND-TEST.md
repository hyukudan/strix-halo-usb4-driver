# Build and test the integrated driver and multi-slot engine

Use current `main` in both `hyukudan/strix-halo-usb4-driver` and
`hyukudan/strix-halo-engine-lab`. Source and regressions are integrated; this is
an experimental build, not a signed, installed or hardware-qualified release.
Keep a known-good boot entry and leave the IOMMU enabled.

## 1. Full offline compilation (no target hardware required)

Obtain a clean Linux Git checkout at
`72d3fcf802c45d00b300f25b848a93c3a2bd7c7e` and a ROCm development toolchain.
The validator prints the plan by default. Add `--execute` to run it locally:

```sh
python3 tools/validate_native.py \
  --engine-repo /absolute/path/strix-halo-engine-lab \
  --kernel-checkout /absolute/path/pristine-pinned-linux \
  --output /absolute/path/new-usb4-native-build \
  --hip-compiler /opt/rocm/llvm/bin/clang++ \
  --gpu-off-matrix --jobs 2 --execute
```

The output must not exist and must be outside the input checkouts. The tool
archives the genuine pinned Git tree, applies all nine patches, checks the
materialized implementation hashes, builds `vmlinux` and all modules for GPU-on
and GPU-off configurations, then builds `strix-gpu-queue-visibility`. It saves
individual command logs and changes `build.json` to `passed` only after every
command actually succeeds. No downloads, Actions, installation or GPU execution
are performed. Source changes during the build invalidate its result.

The validation kernels use `x86_64_defconfig` plus the required USB4 options.
They are compile coverage, **not a machine-specific boot configuration**. Before
installation, build the patched tree again with the target host's known-working
configuration merged with `config/usb4-gpu.fragment`, a distinct LOCALVERSION,
and the required boot/storage/security settings. Follow the host distribution's
package, initramfs, Secure Boot signing and bootloader process. Do not replace the
known-good boot entry. No generic installation command here assumes those local
settings. Rebuild all Thunderbolt/USB4NET consumers: the internal layouts changed.

## 2. Portable regression reproduction

```sh
python3 tools/prepare_overlay.py audited-overlay \
  --kernel-source /absolute/path/pristine-pinned-linux
export USB4_SOURCE_TREE="$PWD/audited-overlay"
export USB4_KERNEL_SOURCE=/absolute/path/pristine-pinned-linux
python3 -m unittest discover -s tests -v
```

In the engine checkout, point to that same materialized source and the driver
checkout containing the canonical test headers:

```sh
export USB4_DRIVER_REPO=/absolute/path/strix-halo-usb4-driver
python3 -m unittest discover -s tests -p 'test_*gpu*.py' -v
```

The 81/101/12 delivery totals refer to the portable package suites. The repository
also retains original reconstruction tests, while bundle-only context tests skip
outside their package. Do not interpret a different suite size as native coverage.

## 3. Build the current multi-slot executable

From the engine checkout:

```sh
cmake -S lab/gpu_queue -B build/gpu-queue -G Ninja \
  -DSTRIX_ENABLE_HIP_RUNTIME=ON -DBUILD_TESTING=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang++ \
  -DCMAKE_HIP_ARCHITECTURES=gfx1151
cmake --build build/gpu-queue --target strix-gpu-queue-visibility -j2
build/gpu-queue/strix-gpu-queue-visibility --help
```

Use `docs/GPU-QUEUE-INTEGRATION-2026-10-16.md` in the engine for the complete
identity, device, control-tunnel, payload and peer commands. Select
`--queue-depth 8 --windows 4 --full-duplex` to request eight TX and eight RX
operations per completed batch window, subject to the observed ring capacity.
Do not substitute the older C1 executable for this route.

## 4. Runtime gates

Deliberately configure the intended `gpu_direct` endpoint, preserve exclusive
configuration ownership, verify the actually loaded module/GPU identities on
both hosts, and use a fresh agreed epoch. First run the configuration-only path,
then one-way transfer, then duplex and reuse; compare both actual GPU reports.
Controller completion is not remote GPU-consumer completion. Failed/unknown
DMA or GPU ownership blocks reuse and can retain resources.

Group reset is explicit. Including USB4NET can interrupt traffic and drop
packets; its control path must be physically independent of the controller being
reset. A same-link SSH tunnel is not independent. Unknown consumers remain a
veto rather than being reset by force.

No native build success, GPU cache guarantee, unplug/suspend qualification or
40 Gb/s repair is implied by source publication. Preserve failed build/run logs
and stop on errors instead of disabling the IOMMU or bypassing the guards.
