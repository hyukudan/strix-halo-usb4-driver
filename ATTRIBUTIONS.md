# Attribution and licensing

Linux base: https://github.com/torvalds/linux/tree/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
The copied/modified files preserve their original copyright and SPDX notices.
`COPYING`, `LICENSES/preferred/GPL-2.0` and
`LICENSES/exceptions/Linux-syscall-note` are retained from that same revision.
The kernel extension is GPL-2.0-only; the experimental UAPI carries
GPL-2.0 WITH Linux-syscall-note. Existing upstream files retain their own tags.

The project DMA-BUF extension originated in the local
`hyukudan/strix-halo-engine-lab` source, including kernel extension files last
committed there at `9aa96d512691a7ccfb18ab0b205ee92e3a54adaf`, followed by local
driver fixes. This backup captures the current source bytes, not merely that
historical project revision. Original USB4STREAM Intel notices remain present.

Two MSI-X fixes were adapted from CiruStrixLink's Linux 7.2.2 patch series:
https://github.com/ciru-ai/CiruStrixLink/tree/master/kernel/linux-7.2.2

- Jonathan Yates, "Flush posted MSI-X interrupt clears", patch mail object
  `0b1c43d0ab12af459479635a06e8288c4ae765d5`.
- "Clear stale MSI-X status before ring arm", patch mail object
  `4c9c48479c2a6d17ccd04619bbfcdb9ec7572869`, attributed in its original header
  to Codex USB4STREAM Analysis.

Their original headers, author/sign-off text and public links are retained in
`provenance/reference-patches/`. They are provenance copies, not additional
patches to apply after the combined snapshot. Public source inspection and
private audit correspondence are not redistributed here.
