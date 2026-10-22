# Attribution and licensing

Linux base: https://github.com/torvalds/linux/tree/72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
The copied/modified files preserve their original copyright and SPDX notices.
`COPYING`, `LICENSES/preferred/GPL-2.0` and
`LICENSES/exceptions/Linux-syscall-note` are retained from that same revision.
The kernel extension is GPL-2.0-only; the experimental UAPI carries
GPL-2.0 WITH Linux-syscall-note. Existing upstream files retain their own tags.

The DMA-BUF GPU stream extension originated in the
[`hyukudan/strix-halo-engine-lab`](https://github.com/hyukudan/strix-halo-engine-lab)
source and was then developed here. The USB4STREAM configfs streams build on
the original Intel USB4STREAM code; its notices remain present.

Two MSI-X fixes in 0001 were adapted from CiruStrixLink's Linux 7.2.2 patch
series: https://github.com/ciru-ai/CiruStrixLink/tree/master/kernel/linux-7.2.2

- Jonathan Yates, "thunderbolt: Flush posted MSI-X interrupt clears", patch
  mail object `0b1c43d0ab12af459479635a06e8288c4ae765d5`. Its original header,
  author and sign-off are retained in `provenance/reference-patches/`.
- "thunderbolt: Clear stale MSI-X status before ring arm", patch mail object
  `4c9c48479c2a6d17ccd04619bbfcdb9ec7572869` in that series, which links
  Benjamin S. Berman's upstream submission:
  https://patchew.org/linux/20260428015521.3454006-1-benjamin.s.berman@gmail.com/

The provenance copy is not an additional patch to apply after the series.

`patches/kernel-backports/` holds an unmodified upstream Linux fix
(`e6895021a328`, drm/amdkfd) with its original authorship.
