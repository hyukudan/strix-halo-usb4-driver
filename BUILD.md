# Reconstruct, test and build

Use a separate workspace and a known-good fallback kernel. This is an in-tree
patch series, not a standalone out-of-tree module distribution. None of these
instructions installs modules, writes firmware, reloads a driver or reboots.
The complete target configuration, module linkage/signing, initramfs and boot
selection are separate deployment requirements.

## Reconstruct the audited source

From this repository's root, download the **pinned** complete upstream tree into
a separate workspace. The original archive hash is retained for verification:

```sh
backup_repo=$(pwd)
pin=72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
mkdir -p ../usb4-build-workspace
cd ../usb4-build-workspace
curl -fL "https://codeload.github.com/torvalds/linux/tar.gz/$pin" -o linux-pinned.tar.gz
echo 'f265c0eeb342d2f697c0ede885dcca5292e89bd6100ea3265fed409c589e537d  linux-pinned.tar.gz' | sha256sum -c -
tar -xf linux-pinned.tar.gz
"$backup_repo/tools/apply-series.sh" "linux-$pin"
cd "linux-$pin"
```

`tools/apply-series.sh` is the canonical procedure (also used by README and
CI): it stops with a non-zero status at the first patch that does not apply,
instead of continuing and reporting success on a partially patched tree.

Apply all series entries, not just 0001. Do not reapply anything from
`provenance/reference-patches/`: those changes are already in 0001.

Alternatively, generate the audited overlay and copy it onto a **clean copy of
the same pinned upstream tree**, instead of applying the series:

```sh
python3 "$backup_repo/tools/prepare_overlay.py" /new/non-existing/audited-overlay \
    --kernel-source /path/to/clean-pinned-linux
cp -a /new/non-existing/audited-overlay/. /path/to/clean-pinned-linux/
```

`--kernel-source` supplies the pinned upstream `tb.c`, `tb.h` and USB4NET
`main.c` that the capture lacks (hash-checked, never modified). The
materializer copies exactly the files listed in
`provenance/source-manifest.json` (an extra, missing or symlinked entry under
`overlay/` is an error), verifies original capture hashes, applies follow-up patches
outside any surrounding Git repository context and refuses to overwrite an
existing output directory. Never combine these two reconstruction methods.
Copying raw `overlay/` alone or applying only 0001 reproduces the original backup
**without** the audit fixes. To intentionally reproduce that historical capture,
use exactly one of those historical methods and compare its files against
`provenance/source-manifest.json`.

## Hardware-free regression tests

From the driver repository checkout:

```sh
python3 tools/prepare_overlay.py audited-overlay --kernel-source /path/to/clean-pinned-linux
USB4_SOURCE_TREE="$PWD/audited-overlay" USB4_KERNEL_SOURCE=/path/to/clean-pinned-linux \
    python3 -m unittest discover -s tests -v
```

Requires Python 3, Git and a C compiler with UndefinedBehaviorSanitizer support.
Tests compile selected actual source functions against small mocked list/register
interfaces and exercise the real shared UAPI helpers. They do not emulate NHI
hardware, GPU visibility, interrupt timing or concurrent DMA races.

## Configure and compile the complete kernel

The original build observation used GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1). Typical
dependencies include a C toolchain, make, flex, bison, pkg-config, libelf and
OpenSSL development headers, dwarves and libdw development headers. Start from
a complete known-good configuration for the target's storage, filesystems and
GPU; the fragment alone does not describe a bootable kernel.

From the reconstructed complete Linux source:

```sh
mkdir -p ../build-usb4
cp /path/to/target-base.config ../build-usb4/.config
scripts/kconfig/merge_config.sh -m -O ../build-usb4 ../build-usb4/.config \
    "$backup_repo/config/usb4-gpu.fragment"
scripts/config --file ../build-usb4/.config \
    --set-str LOCALVERSION "-strix-gpu-bosgame-$(date +%Y%m%d)a"
make O=../build-usb4 olddefconfig
python3 "$backup_repo/tools/check_kconfig.py" ../build-usb4/.config \
    --kernelrelease "$(make -s O=../build-usb4 kernelrelease)"
make O=../build-usb4 -j8
```

Every build needs its own LOCALVERSION under `-strix-gpu-bosgame-` (the lab
modprobe policy and service match that prefix). The fragment only carries a
placeholder, which `check_kconfig.py` rejects together with the captured
`-strix-gpu-bosgame-20261014`, an existing `/lib/modules/<kernelrelease>` and
any fragment option that Kconfig dropped. Preserve the configuration, source
commit/series, compiler version and module hashes used in each hardware
comparison.

Once the matching **full kernel build** exists, affected modules can be rebuilt:

```sh
make O=../build-usb4 -j8 drivers/thunderbolt/thunderbolt.ko \
    drivers/thunderbolt/thunderbolt_stream.ko \
    drivers/net/thunderbolt/thunderbolt_net.ko
```

The CI workflow builds and links every USB4 consumer (`thunderbolt.ko`,
`thunderbolt_stream.ko`, `thunderbolt_net.ko`, through `modules_prepare` and
modpost) with GPU mode enabled and disabled, fails on any warning that `W=1`
reports in this series' files, and compiles the engine's HIP target when the
runner has ROCm. It is not a full `vmlinux` build of the target configuration
or a hardware qualification; `tools/validate_native.py --execute` is the full
offline build.

GPU mode remains explicitly experimental. Controller completion alone does not
establish GPU visibility or remote application acknowledgment. Do not unload or
reset a controller while DMA users remain active. Retained/quarantined DMA
resources are not safe to free just because a timeout elapsed.
