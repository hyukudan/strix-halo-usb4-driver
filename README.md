# Strix Halo USB4 driver: source capture and audit series

Experimental USB4STREAM/DMA-BUF and Thunderbolt core development for Strix Halo.
Pinned upstream Linux revision: `72d3fcf802c45d00b300f25b848a93c3a2bd7c7e`
(7.3-rc5).

## Audited development result

The original capture remains immutable. The fixes are additive patches:

- `patches/series`: apply every entry in order to the pinned upstream tree.
- `patches/audit-series`: only the follow-up patches, for use after the capture.
- `tools/prepare_overlay.py`: verify capture hashes and create a separate overlay
  containing the audit fixes. **Raw `overlay/` alone does not include the fixes.**
- `tools/usb4_diagnose.py`: read-only directional link rates, lane counts,
  controller mapping and optional kernel logs.
- `tests/` and `.github/workflows/audit.yml`: hardware-free regression checks,
  reconstruction checks and a pinned-kernel object-build workflow.

```sh
python3 tools/prepare_overlay.py audited-overlay
USB4_SOURCE_TREE="$PWD/audited-overlay" python3 -m unittest discover -s tests -v
python3 tools/usb4_diagnose.py --logs > usb4-report.json
```

Read [the initial audit and validation limits](docs/AUDIT-2026-10-16.md),
[queue/fence/session-recovery additions](docs/QUEUES-AND-RECOVERY.md), and
[BUILD.md](BUILD.md) before building. Patch 0005 adds opt-in queues of up to eight
operations per direction, waits outside the device mutex, per-slot sync_file
fences, finite DMA deadlines and fail-closed session recovery. It preserves the
legacy ioctl layouts and does not reset the whole AMD NHI or retrain the cable.

The audit does not claim a measured bandwidth improvement or that the failing
physical link now negotiates 40 Gb/s. A sysfs speed of 20 Gb/s **per lane** with
two lanes already represents a 40 Gb/s raw link in that direction. Application
throughput is a separate measurement. Local simulator tests are not a substitute
for the pinned-kernel build and two-host GPU/DMA integrity testing.

## Immutable capture

Private source backup of the local USB4STREAM/DMA-BUF extension and Thunderbolt
core changes, captured in read-only mode on 2026-10-16. The source tree was not a
Git checkout; `patches/0001-usb4-current-source.patch` records its actual difference
from the pinned upstream revision. Original capture contents:

- `overlay/`: exact captured bytes of the modified/new driver and UAPI files.
- `patches/0001-usb4-current-source.patch`: the combined original change. Apply
  this once as the first series entry, never together with reference patches.
- `provenance/source-manifest.json`: original base/final hashes and source revision.
- `provenance/reference-patches/`: original attributed MSI-X correspondence,
  already incorporated into patch 0001.
- `config/usb4-gpu.fragment`: relevant options, not a complete boot configuration.

The capture includes GPU DMA-BUF import lifecycle, GPU ring framing/reuse,
aligned imported windows, runtime DMA credits, selected core teardown/notification
reverts, and posted MSI-X acknowledgement/ring-arm fixes. The original manifest
and patch remain authoritative for the capture's exact scope, not for the later
audited result.

The running kernel observed during capture was
`7.3.0-rc5-strix-gpu-bosgame-20261014`; this does not independently prove which
source bytes produced a loaded module. No driver build, reload, installation,
reboot, configuration change or new performance test was performed to create
that original backup. No binaries, signing keys or private experiment logs are
included. The audit's separate validation status is recorded in its report.

See [ATTRIBUTIONS.md](ATTRIBUTIONS.md) for licensing and original patch provenance.
