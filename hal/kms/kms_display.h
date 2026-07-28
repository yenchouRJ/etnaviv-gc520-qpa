/*
 * kms_display.h — minimal legacy-KMS display helper for MA35D1 (ma35 drm).
 *
 * SUMMARY.md Phase 3: the "display half" of the accelerated 2D pipeline,
 * kept deliberately separate from the paint path (libetna2d) so each half
 * is verifiable on its own.
 *
 * The ma35 driver advertises DRIVER_GEM|MODESET|ATOMIC with GEM-DMA (CMA,
 * contiguous) dumb buffers + PRIME. This helper uses the *legacy* modeset
 * path (drmModeSetCrtc / drmModePageFlip) which is sufficient for a
 * fullscreen scanout benchmark and simpler than atomic.
 *
 * Buffer-sharing direction (dictated by the HW — display is contiguous,
 * no display-side IOMMU; etnaviv has its own MMU v2):
 *
 *   1. kms_open()                       → card0, connector, crtc, mode
 *   2. kms_fb_create()                  → contiguous dumb scanout BO + FB id
 *   3. kms_fb_export_dmabuf()           → dma-buf fd  (feed to etnaviv)
 *   4. (libetna2d renders INTO the BO via the imported surface)
 *   5. kms_set_crtc() / kms_page_flip() → on panel, zero copy
 *
 * All scanout buffers are DRM_FORMAT_XRGB8888 (32bpp), matching the ARGB
 * surfaces libetna2d renders. The panel ignores the top (alpha) byte.
 */
#ifndef KMS_DISPLAY_H
#define KMS_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct kms_display;

/* One scanout framebuffer: a contiguous dumb BO + its DRM FB id. */
struct kms_fb {
	uint32_t fb_id;      /* drmModeAddFB2 id (pass to set_crtc/page_flip) */
	uint32_t handle;     /* GEM handle                                    */
	uint32_t width, height;
	uint32_t stride;     /* bytes                                         */
	uint64_t size;       /* bytes                                         */
	void    *map;        /* CPU mmap (NULL until kms_fb_map)              */
};

/* Open card0, pick the first connected connector + a usable crtc, and
 * cache its preferred mode. path e.g. "/dev/dri/card0". */
int  kms_open(const char *path, struct kms_display **out);
void kms_close(struct kms_display *d);

/* Mode geometry of the selected output. */
int kms_width(const struct kms_display *d);
int kms_height(const struct kms_display *d);
int kms_refresh(const struct kms_display *d);   /* Hz, 0 if unknown */

/* The raw DRM fd (for advanced callers / drmHandleEvent). */
int kms_fd(const struct kms_display *d);

/* Create a scanout framebuffer sized to the current mode. */
int  kms_fb_create(struct kms_display *d, struct kms_fb *fb);
void kms_fb_destroy(struct kms_display *d, struct kms_fb *fb);

/* CPU-map the framebuffer (write-combine via the dumb map offset).
 * Returns fb->map or NULL. */
void *kms_fb_map(struct kms_display *d, struct kms_fb *fb);

/* PRIME-export the framebuffer's BO to a dma-buf fd. The returned fd is
 * owned by the caller (close() it, or pass to etna2d_surface_from_dmabuf
 * which imports by dup). Returns fd >= 0 or -1. */
int kms_fb_export_dmabuf(struct kms_display *d, const struct kms_fb *fb);

/* Legacy modeset: make fb the active scanout on the selected crtc/connector.
 * Call once before the first frame (and after any mode change). */
int kms_set_crtc(struct kms_display *d, const struct kms_fb *fb);

/* Queue a page flip to fb; returns immediately. The flip completes at the
 * next vblank. Use kms_wait_flip() to block for the completion event. */
int kms_page_flip(struct kms_display *d, const struct kms_fb *fb);

/* Block until the pending page-flip event arrives (drmHandleEvent).
 * Returns 0 on the flip, -1 on error. */
int kms_wait_flip(struct kms_display *d);

#ifdef __cplusplus
}
#endif

#endif /* KMS_DISPLAY_H */
