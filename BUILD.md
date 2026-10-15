# Reconstruct and build

These are manual instructions for a separate build workspace. They were not run
as part of this backup. Full kernel builds require the matching complete upstream
tree; the small overlay is not an out-of-tree module distribution.

The observed source build used GCC 15.2.0 (Ubuntu package 15.2.0-16ubuntu1).
Typical dependencies are a C toolchain, make, flex, bison, pkg-config, libelf and
OpenSSL development headers, dwarves and libdw development headers. Retain the
configuration required by the target machine's storage, filesystem and GPU.

## Reconstruct the exact source delta

```sh
pin=72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
curl -fL "https://codeload.github.com/torvalds/linux/tar.gz/$pin" -o linux-pinned.tar.gz
echo 'f265c0eeb342d2f697c0ede885dcca5292e89bd6100ea3265fed409c589e537d  linux-pinned.tar.gz' | sha256sum -c -
tar -xf linux-pinned.tar.gz
backup_repo=$(pwd)
cd "linux-$pin"
git apply --check "$backup_repo/patches/0001-usb4-current-source.patch"
git apply "$backup_repo/patches/0001-usb4-current-source.patch"
```

Alternatively copy the overlay onto that same pinned tree. Do not combine the
two reconstruction methods or reapply the provenance reference patches. Verify
the resulting changed files against `provenance/source-manifest.json`.

## Configure and compile

Start from a complete target kernel configuration; the fragment alone does not
describe a bootable kernel. From the reconstructed source tree:

```sh
mkdir -p ../build-usb4
cp /path/to/target-base.config ../build-usb4/.config
scripts/kconfig/merge_config.sh -m -O ../build-usb4 ../build-usb4/.config     "$backup_repo/config/usb4-gpu.fragment"
make O=../build-usb4 olddefconfig
make O=../build-usb4 -j8
```

Once the matching full kernel build exists, individual affected module targets
can be rebuilt with the same config/toolchain:

```sh
make O=../build-usb4 -j8 drivers/thunderbolt/thunderbolt.ko     drivers/thunderbolt/thunderbolt_stream.ko     drivers/net/thunderbolt/thunderbolt_net.ko
```

Module ABI, full configuration, symbol versions, signing/trust policy, initramfs
and bootloader selection are separate deployment requirements. This repository
does not provide an install/reload/reboot script or private signing material.
The GPU mode is experimental and explicit; controller completion alone does not
establish GPU visibility or acknowledgement by the remote application.
