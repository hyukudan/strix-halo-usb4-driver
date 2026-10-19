# Upstream backports for the 7.3-rc5 build

Unmodified upstream commits, applied to the pinned kernel after `patches/series`.
They are not part of the USB4 series and are not used by `prepare_overlay.py`.

| Patch | Upstream | Why |
|---|---|---|
| 0001-drm-amdkfd-fix-dma_buf-reference-leak-in-get_dmabuf_info.patch | e6895021a328 (v7.3-rc7, Cc: stable) | On the error path of `AMDKFD_IOC_GET_DMABUF_INFO` (metadata buffer allocation failure) the dma_buf reference is not dropped; the ioctl is on the import path the GPU stream uses. Successful calls do not leak. |

Checked 2026-10-17: applies with `git apply --check` on 72d3fcf8 (7.3-rc5).
Not taken: f145d5b3f740 (SDMA doorbell alignment) only affects the
aqua_vanjaram doorbell table, not Strix Halo.

Drop this directory when rebasing onto a release that already contains them.
