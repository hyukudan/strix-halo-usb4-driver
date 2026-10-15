# Strix Halo USB4 driver source backup

Private source backup of the local USB4STREAM/DMA-BUF extension and Thunderbolt
core changes, captured in read-only mode on 2026-10-16. The source tree was not a
Git checkout; the combined patch records its actual difference from pinned Linux
revision `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`.

Contents:

- `overlay/`: exact current bytes of the modified/new driver and UAPI files.
- `patches/0001-usb4-current-source.patch`: the complete combined change against
  the pinned upstream revision; apply this once, without the reference patches.
- `provenance/source-manifest.json`: base/final file hashes and source revision.
- `provenance/reference-patches/`: original attributed MSI-X patch correspondence
  retained for provenance, already incorporated into the combined patch.
- `config/usb4-gpu.fragment`: relevant build options, not a complete boot config.

The changes include the experimental GPU DMA-BUF UAPI/import lifecycle, GPU ring
framing/reuse, aligned imported-window handling, runtime DMA credits, selected
core teardown/notification reverts, and posted MSI-X acknowledgement/ring-arm
fixes. The manifest and patch are authoritative for exact scope.

The running kernel observed during capture was
`7.3.0-rc5-strix-gpu-bosgame-20261014`; this source backup does not independently
prove which source bytes produced a loaded module. No driver build, reload,
installation, reboot, configuration change or new performance test was performed
to create this repository. No binaries, signing keys or private experiment logs
are included.

See [BUILD.md](BUILD.md) for reconstruction/build instructions and
[ATTRIBUTIONS.md](ATTRIBUTIONS.md) for source licensing and patch provenance.
