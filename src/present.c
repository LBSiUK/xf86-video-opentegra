/*
 * SPDX-License-Identifier: MIT
 *
 * Present extension hooks for xf86-video-opentegra.
 *
 * Implements the vblank queue (queue/abort/get_ust_msc/get_crtc) plus
 * page-flipping (check_flip/flip/unflip). A flippable Present request
 * becomes a zero-copy scanout swap via legacy drmModePageFlip; anything
 * check_flip rejects falls back to Present's copy path over DRI3, which
 * is correct but unaccelerated for vsync.
 *
 * Surface RT wires a single CRTC (LVDS-0), so the flip bookkeeping uses
 * a single static slot and legacy (non-atomic) KMS, matching the
 * drmModeAddFB/drmModeSetCrtc style already in drmmode_display.c.
 */

#include "driver.h"

#ifdef HAVE_PRESENT

#include <string.h>

#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "present.h"
#include "vblank.h"
#include "drmmode_display.h"
#include "exa/exa.h"

/*
 * One event struct for both vblank and flip queue entries. event_id MUST
 * stay the first member: tegra_present_match_event() identifies our
 * entries by reading it through a cast (q->data has no type tag).
 */
struct tegra_present_event {
    uint64_t  event_id;     /* MUST be first — abort matcher reads this */
    ScreenPtr screen;       /* set for flip events; unused for vblank */
    uint32_t  flip_fb_id;   /* DRM fb created for this flip; 0 = front BO */
    PixmapPtr flip_pixmap;  /* pixmap ref held for this flip; NULL = none */
};

/*
 * What the kernel is scanning out right now. fb_id == 0 means the
 * drmmode front BO is on screen (no active flip). One slot suffices for
 * the single Surface RT CRTC.
 */
static struct {
    uint32_t  fb_id;
    PixmapPtr pixmap;
} tegra_present_onscreen;

struct present_abort_match {
    uint64_t event_id;
};

static Bool
tegra_present_match_event(void *queue_data, void *match_data)
{
    struct tegra_present_event *ev = queue_data;
    struct present_abort_match *m = match_data;
    /* We can only match by event_id; other subsystems' queue entries
     * have a different layout but their data pointers won't equal
     * ours by accident. Caller filters by queue handler before
     * dispatching to this matcher (see TegraPresentAbortVblank). */
    return ev->event_id == m->event_id;
}

static xf86CrtcPtr
tegra_present_first_crtc(ScrnInfoPtr scrn)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int c;

    /* On Surface RT only LVDS-0 is wired, so this is config->crtc[0]. */
    for (c = 0; c < config->num_crtc; c++) {
        if (config->crtc[c]->enabled)
            return config->crtc[c];
    }
    return NULL;
}

static RRCrtcPtr
TegraPresentGetCrtc(WindowPtr window)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(window->drawable.pScreen);
    xf86CrtcPtr crtc = tegra_present_first_crtc(scrn);

    return crtc ? crtc->randr_crtc : NULL;
}

static int
TegraPresentGetUstMsc(RRCrtcPtr rr_crtc, uint64_t *ust, uint64_t *msc)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    return tegra_get_crtc_ust_msc(crtc, ust, msc);
}

static void
tegra_present_vblank_handler(uint64_t msc, uint64_t usec, void *data)
{
    struct tegra_present_event *event = data;
    present_event_notify(event->event_id, usec, msc);
    free(event);
}

static void
tegra_present_vblank_abort(void *data)
{
    struct tegra_present_event *event = data;
    free(event);
}

static Bool
TegraPresentQueueVblank(RRCrtcPtr rr_crtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    ScrnInfoPtr scrn = crtc->scrn;
    TegraPtr tegra = TegraPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    struct tegra_present_event *event;
    drmVBlank vbl;
    uint32_t seq;
    int ret;

    event = calloc(1, sizeof(*event));
    if (!event)
        return FALSE;
    event->event_id = event_id;

    seq = tegra_drm_queue_alloc(crtc, event,
                                tegra_present_vblank_handler,
                                tegra_present_vblank_abort);
    if (!seq) {
        free(event);
        return FALSE;
    }

    vbl.request.type     = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT |
                           (drmmode_crtc->crtc_pipe > 0
                                ? DRM_VBLANK_SECONDARY : 0);
    vbl.request.sequence = tegra_crtc_msc_to_kernel_msc(crtc, msc);
    vbl.request.signal   = (unsigned long)seq;

    ret = drmWaitVBlank(tegra->fd, &vbl);
    if (ret) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "Present: drmWaitVBlank failed: %s\n",
                   strerror(errno));
        tegra_drm_abort_seq(scrn, seq);
        return FALSE;
    }

    return TRUE;
}

static void
TegraPresentAbortVblank(RRCrtcPtr rr_crtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    ScrnInfoPtr scrn = crtc->scrn;
    struct present_abort_match m = { .event_id = event_id };
    (void)msc;

    /* Best-effort: tegra_drm_abort matches the first entry where the
     * matcher returns TRUE. If no Present event is queued under this
     * id, this is a no-op (Present's caller treats abort as advisory). */
    tegra_drm_abort(scrn, tegra_present_match_event, &m);
}

static void
TegraPresentFlush(WindowPtr window)
{
    /* Kernel handles BO sync via implicit fences; nothing to flush
     * server-side. */
    (void)window;
}

/*
 * Drop the fb + pixmap ref we were holding for the previously
 * scanned-out flip, and record what the kernel just put on screen.
 * Called from the flip handler once a flip (or unflip) has completed,
 * so the old buffer is provably no longer being scanned out.
 */
static void
tegra_present_swap_onscreen(ScreenPtr screen, uint32_t new_fb_id,
                            PixmapPtr new_pixmap)
{
    TegraPtr tegra = TegraPTR(xf86ScreenToScrn(screen));

    if (tegra_present_onscreen.fb_id)
        drmModeRmFB(tegra->fd, tegra_present_onscreen.fb_id);
    if (tegra_present_onscreen.pixmap)
        (*screen->DestroyPixmap)(tegra_present_onscreen.pixmap);

    tegra_present_onscreen.fb_id  = new_fb_id;
    tegra_present_onscreen.pixmap = new_pixmap;
}

static void
tegra_present_flip_handler(uint64_t msc, uint64_t usec, void *data)
{
    struct tegra_present_event *event = data;

    present_event_notify(event->event_id, usec, msc);
    tegra_present_swap_onscreen(event->screen, event->flip_fb_id,
                                event->flip_pixmap);
    free(event);
}

static void
tegra_present_flip_abort(void *data)
{
    struct tegra_present_event *event = data;
    TegraPtr tegra = TegraPTR(xf86ScreenToScrn(event->screen));

    /* The flip never reached the kernel completion path (server
     * teardown / failed submit). Drop the fb and the pixmap ref taken
     * in TegraPresentFlip; tegra_present_onscreen is left untouched —
     * it still describes whatever the kernel last scanned out. */
    if (event->flip_fb_id)
        drmModeRmFB(tegra->fd, event->flip_fb_id);
    if (event->flip_pixmap)
        (*event->screen->DestroyPixmap)(event->flip_pixmap);
    free(event);
}

static Bool
TegraPresentCheckFlip(RRCrtcPtr rr_crtc, WindowPtr window, PixmapPtr pixmap,
                      Bool sync_flip)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    ScrnInfoPtr scrn = crtc->scrn;
    TegraPtr tegra = TegraPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    struct tegra_pixmap *priv;
    (void)window;

    /* Async (tearing) flips are not supported; let Present copy. */
    if (!sync_flip)
        return FALSE;

    /* Single, enabled, powered-on, un-rotated CRTC only. */
    if (!crtc->enabled || drmmode_crtc->rotate_fb_id != 0)
        return FALSE;
    if (drmmode_crtc->dpms_mode != DPMSModeOn)
        return FALSE;

    /* The flipped pixmap must cover the whole scanout at matching
     * depth/bpp — a page-flip replaces the entire framebuffer. */
    if (pixmap->drawable.width  != scrn->virtualX ||
        pixmap->drawable.height != scrn->virtualY ||
        pixmap->drawable.depth  != scrn->depth ||
        pixmap->drawable.bitsPerPixel != scrn->bitsPerPixel)
        return FALSE;

    /* It must resolve to a real Tegra BO once thawed... */
    TegraEXAThawPixmap(pixmap);
    priv = exaGetPixmapDriverPrivate(pixmap);
    if (!priv || priv->type != TEGRA_EXA_PIXMAP_TYPE_BO || !priv->bo)
        return FALSE;

    /* ...with a stride the display controller can scan out. The Tegra
     * dc is pitch-linear and alignment-sensitive, so the pixmap pitch
     * must match the front BO exactly; otherwise fall back to copy. */
    if (pixmap->devKind != (int)tegra->drmmode.front_bo->pitch)
        return FALSE;

    return TRUE;
}

static Bool
TegraPresentFlip(RRCrtcPtr rr_crtc, uint64_t event_id, uint64_t target_msc,
                 PixmapPtr pixmap, Bool sync_flip)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    ScrnInfoPtr scrn = crtc->scrn;
    TegraPtr tegra = TegraPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    struct tegra_present_event *event;
    struct tegra_pixmap *priv;
    uint32_t handle, fb_id = 0;
    uint32_t seq;
    int ret;

    /* Present already waited for target_msc via TegraPresentQueueVblank
     * before calling flip, so an immediate (next-vblank) page-flip is
     * the correct execution step. */
    (void)target_msc;
    (void)sync_flip;

    TegraEXAThawPixmap(pixmap);
    priv = exaGetPixmapDriverPrivate(pixmap);
    if (!priv || priv->type != TEGRA_EXA_PIXMAP_TYPE_BO || !priv->bo)
        return FALSE;

    /* Don't scan out a buffer with in-flight GPU writes. (A no-op for
     * DRI3-imported client buffers, which carry no opentegra fences.) */
    TEGRA_PIXMAP_WAIT_WRITE_FENCES(priv);

    if (drm_tegra_bo_get_handle(priv->bo, &handle))
        return FALSE;

    ret = drmModeAddFB(tegra->fd,
                       pixmap->drawable.width, pixmap->drawable.height,
                       pixmap->drawable.depth, pixmap->drawable.bitsPerPixel,
                       pixmap->devKind, handle, &fb_id);
    if (ret) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "Present: drmModeAddFB failed: %s\n", strerror(errno));
        return FALSE;
    }

    event = calloc(1, sizeof(*event));
    if (!event) {
        drmModeRmFB(tegra->fd, fb_id);
        return FALSE;
    }
    event->event_id    = event_id;
    event->screen      = scrn->pScreen;
    event->flip_fb_id  = fb_id;
    event->flip_pixmap = pixmap;

    seq = tegra_drm_queue_alloc(crtc, event, tegra_present_flip_handler,
                                tegra_present_flip_abort);
    if (!seq) {
        drmModeRmFB(tegra->fd, fb_id);
        free(event);
        return FALSE;
    }

    /* Pin the pixmap: its BO must survive until this flip is replaced.
     * The matching unref happens in tegra_present_swap_onscreen() (flip
     * completed) or tegra_present_flip_abort() (flip torn down). */
    pixmap->refcnt++;

    ret = drmModePageFlip(tegra->fd, drmmode_crtc->mode_crtc->crtc_id,
                          fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                          (void *)(uintptr_t)seq);
    if (ret) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "Present: drmModePageFlip failed: %s\n", strerror(errno));
        /* tegra_drm_abort_seq -> tegra_present_flip_abort drops the fb
         * and the pixmap ref taken just above, and frees the event. */
        tegra_drm_abort_seq(scrn, seq);
        return FALSE;
    }

    {
        static Bool logged;
        if (!logged) {
            xf86DrvMsg(scrn->scrnIndex, X_INFO,
                       "Present: page-flip path active\n");
            logged = TRUE;
        }
    }

    return TRUE;
}

static void
TegraPresentUnflip(ScreenPtr screen, uint64_t event_id)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);
    xf86CrtcPtr crtc = tegra_present_first_crtc(scrn);
    drmmode_crtc_private_ptr drmmode_crtc;
    struct tegra_present_event *event;
    uint32_t seq;
    int ret;

    /* Nothing flipped, or no usable CRTC: the front BO is already on
     * screen, so just acknowledge immediately. */
    if (!crtc || tegra_present_onscreen.fb_id == 0) {
        present_event_notify(event_id, 0, 0);
        return;
    }
    drmmode_crtc = crtc->driver_private;

    event = calloc(1, sizeof(*event));
    if (!event) {
        present_event_notify(event_id, 0, 0);
        return;
    }
    event->event_id    = event_id;
    event->screen      = screen;
    event->flip_fb_id  = 0;     /* completing this returns us to the front BO */
    event->flip_pixmap = NULL;

    seq = tegra_drm_queue_alloc(crtc, event, tegra_present_flip_handler,
                                tegra_present_flip_abort);
    if (!seq) {
        free(event);
        present_event_notify(event_id, 0, 0);
        return;
    }

    ret = drmModePageFlip(tegra->fd, drmmode_crtc->mode_crtc->crtc_id,
                          tegra->drmmode.fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                          (void *)(uintptr_t)seq);
    if (ret) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "Present: unflip drmModePageFlip failed: %s\n",
                   strerror(errno));
        tegra_drm_abort_seq(scrn, seq);
        present_event_notify(event_id, 0, 0);
        return;
    }
}

static present_screen_info_rec tegra_present_info = {
    .version       = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc      = TegraPresentGetCrtc,
    .get_ust_msc   = TegraPresentGetUstMsc,
    .queue_vblank  = TegraPresentQueueVblank,
    .abort_vblank  = TegraPresentAbortVblank,
    .flush         = TegraPresentFlush,
    .capabilities  = 0,
    .check_flip    = TegraPresentCheckFlip,
    .flip          = TegraPresentFlip,
    .unflip        = TegraPresentUnflip,
};

Bool
TegraPresentScreenInit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);

    if (tegra->present_enabled)
        return TRUE;

    if (!present_screen_init(screen, &tegra_present_info)) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING, "Present init failed\n");
        return FALSE;
    }

    tegra->present_enabled = TRUE;
    xf86DrvMsg(scrn->scrnIndex, X_INFO, "Present initialized (page-flip)\n");
    return TRUE;
}

void
TegraPresentScreenExit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);

    /* Drop a still-active flip fb. The pixmap itself is left to EXA/core
     * teardown — touching it here races screen close. */
    if (tegra_present_onscreen.fb_id)
        drmModeRmFB(tegra->fd, tegra_present_onscreen.fb_id);
    tegra_present_onscreen.fb_id  = 0;
    tegra_present_onscreen.pixmap = NULL;

    tegra->present_enabled = FALSE;
}

#else /* !HAVE_PRESENT */

Bool TegraPresentScreenInit(ScreenPtr screen) { (void)screen; return FALSE; }
void TegraPresentScreenExit(ScreenPtr screen) { (void)screen; }

#endif
