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
cd "linux-$pin"
while IFS= read -r patch; do
    git apply --check "$backup_repo/patches/$patch" || exit 1
    git apply "$backup_repo/patches/$patch" || exit 1
done < "$backup_repo/patches/series"
```

Apply all series entries, not just 0001. Do not reapply anything from
`provenance/reference-patches/`: those changes are already in 0001.

Alternatively, generate the audited overlay and copy it onto a **clean copy of
the same pinned upstream tree**, instead of applying the series:

```sh
python3 "$backup_repo/tools/prepare_overlay.py" /new/non-existing/audited-overlay
cp -a /new/non-existing/audited-overlay/. /path/to/clean-pinned-linux/
```

The materializer verifies original capture hashes, applies follow-up patches
outside any surrounding Git repository context and refuses to overwrite an
existing output directory. Never combine these two reconstruction methods.
Copying raw `overlay/` alone or applying only 0001 reproduces the original backup
**without** the audit fixes. To intentionally reproduce that historical capture,
use exactly one of those historical methods and compare its files against
`provenance/source-manifest.json`.

## Hardware-free regression tests

From the driver repository checkout:

```sh
python3 tools/prepare_overlay.py audited-overlay
USB4_SOURCE_TREE="$PWD/audited-overlay" python3 -m unittest discover -s tests -v
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
    --set-str LOCALVERSION '-strix-usb4-audit-20261016'
make O=../build-usb4 olddefconfig
make O=../build-usb4 -j8
```

Use a distinct LOCALVERSION so a new build cannot be mistaken for the captured
kernel. Preserve the configuration, source commit/series, compiler version and
module hashes used in each hardware comparison.

Once the matching **full kernel build** exists, affected modules can be rebuilt:

```sh
make O=../build-usb4 -j8 drivers/thunderbolt/thunderbolt.ko \
    drivers/thunderbolt/thunderbolt_stream.ko \
    drivers/net/thunderbolt/thunderbolt_net.ko
```

The CI workflow separately performs a smaller object-compilation check with GPU
mode enabled and disabled; it is not a full kernel/module build, modpost proof,
or hardware qualification. See the audit report and actual check results for
what has run successfully. Initial Actions runs did not execute any job steps,
so the existence of the workflow must not be mistaken for a successful build.

GPU mode remains explicitly experimental. Controller completion alone does not
establish GPU visibility or remote application acknowledgment. Do not unload or
reset a controller while DMA users remain active. Retained/quarantined DMA
resources are not safe to free just because a timeout elapsed.
