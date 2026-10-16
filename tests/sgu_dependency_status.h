/* SPDX-License-Identifier: GPL-2.0-only */
/* Test double for the kernel's completed-vs-successful fence distinction. */
#ifndef SGU_DEPENDENCY_STATUS_H
#define SGU_DEPENDENCY_STATUS_H
static int dma_fence_get_status(struct dma_fence *f)
{
    return dma_fence_is_signaled(f) ? (f->error ? f->error : 1) : 0;
}
#endif
