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

## 5. Lab streams and the layout file

`tools/lab/` holds the canonical lab setup: `usb4stream-lab.service` runs
`setup-usb4stream-lab` once at boot (root). It loads `thunderbolt-stream`,
creates the configfs groups `0-2.0` and `1-2.0` with streams `data0`/`data1`
(`ring_size=1024`, `throttling=8192`, `busy_poll=0`, `gpu_direct=0`, hop IDs
`-1` unless already negotiated) and then runs `usb4stream-describe` to write
`/run/usb4stream-layout.json`. If describe is missing or fails, it writes the
version-1 layout (`device` and `configfs` only).

`usb4stream-describe` (`tools/lab/usb4stream_describe.py`) is read-only and
needs no root: it reads PCI/Thunderbolt sysfs, the configfs stream groups and
`/dev/tbstreamN` permissions. It never opens a stream device (open allocates
rings and the last close sends CLOSE to the peer), so `openable` is derived:
device node present, read/write access for the caller, service bound to
`thunderbolt_stream`. Options: `--summary`, `--output FILE` (atomic), `--logs`
(explain controllers that failed probe from the kernel log), `--port-labels
FILE` (default `/etc/usb4stream-ports.json`, e.g.
`{"0000:c7:00.5": "rear", "0000:c7:00.6": "front"}`; keys may also be
`1022:158d` or the ACPI path), and `--merge A.json B.json` for the pair view.

Layout schema, version 2 (all version-1 keys unchanged):

```text
"<service>/<stream>": {            # top-level keys containing "/" are streams
  device, configfs,                # version 1
  service, stream, index, in_hopid, out_hopid, config{ring_size, ...},
  exists, attached, openable, openable_reason?, open_checked=false,
  local{nhi_pci, nhi_device_id, nhi_acpi_path, domain, usb4_port, port_label} | null,
  link{rx|tx{gbps_per_lane, lanes, gbps, raw_*}, gbps, full_speed} | null,
  remote{xdomain, route, device_name, vendor_name, unique_id} | null,
  cable: "A".."Z" | null
}
"version": 2
"topology": {
  host, tool, generated_at, summary, cables_total, cables_up, gbps_total,
  controllers[{pci, device_id, acpi_path, driver, domain, state: up|controller-down,
               host_router_uuid, generation, usb4_ports, port_label, kernel_log?}],
  cables[{id, key, state: up|no-peer|controller-down, local{...}, remote{...}|null,
          link{...}|null, streams[], devices[], streams_openable}],
  peers{<host>: {unique_ids, cables, gbps}}, warnings[], notes[]
}
```

Link speeds in sysfs are per lane: Gen3 x2 is `20 Gb/s x 2 = 40 Gb/s` per
direction, Gen2 x2 or Gen3 x1 is 20 Gb/s; `full_speed` is false and a warning
is emitted below Gen3 x2. Streams pair across hosts by stream name on the same
cable. The merged view (`--merge`) gives `links[]` with both ends,
`wiring` = `straight` (same NHI on both ends) or `crossed`, `wiring_by_label`
when port labels are configured (`rear-front`, ...), the `stream_pairs`
(local `tbstreamN` <-> remote `tbstreamM`), and `suspected_down_links` when
exactly one slot per host is unconnected.

Install (maintenance window, as root, on each node):

```sh
install -m 0755 tools/lab/usb4stream_describe.py /usr/local/sbin/usb4stream-describe
install -m 0755 tools/lab/setup-usb4stream-lab /usr/local/sbin/setup-usb4stream-lab
install -m 0644 tools/lab/usb4stream-lab.service /etc/systemd/system/usb4stream-lab.service
systemctl daemon-reload
systemctl restart usb4stream-lab.service   # stop stream users first
usb4stream-describe --summary
```

The service restart rewrites configfs attributes; do it with no open streams.
