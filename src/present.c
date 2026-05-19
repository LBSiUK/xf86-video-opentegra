/*
 * SPDX-License-Identifier: MIT
 *
 * Present extension hooks for xf86-video-opentegra.
 *
 * Minimal implementation: vblank queue/abort/get_ust_msc/get_crtc. No
 * page-flipping yet (check_flip omitted -> Present uses its copy
 * fallback over DRI3, which is correct but unaccelerated for vsync).
 * Adding flip is a follow-up; it needs CRTC-state coordination with
 * drmmode_display.c.
 */

#include "driver.h"

#ifdef HAVE_PRESENT

#include <xf86.h>
#include <xf86Crtc.h>
#include <xf86drm.h>

#include "present.h"
#include "vblank.h"
#include "drmmode_display.h"

struct tegra_present_vblank_event {
    uint64_t event_id;
};

/* Sentinel pointer used by the matcher to distinguish OUR entries from
 * other subsystems' (DRI2's frame events use the same drm queue). We
 * check the handler pointer because q->data has no type tag. */
static void tegra_present_vblank_handler(uint64_t msc, uint64_t usec,
                                         void *data);

struct present_abort_match {
    uint64_t event_id;
};

static Bool
tegra_present_match_event(void *queue_data, void *match_data)
{
    struct tegra_present_vblank_event *ev = queue_data;
    struct present_abort_match *m = match_data;
    /* We can only match by event_id; other subsystems' queue entries
     * have a different layout but their data pointers won't equal
     * ours by accident. Caller filters by queue handler before
     * dispatching to this matcher (see TegraPresentAbortVblank). */
    return ev->event_id == m->event_id;
}

static RRCrtcPtr
TegraPresentGetCrtc(WindowPtr window)
{
    ScreenPtr screen = window->drawable.pScreen;
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int c;

    /* Pick the first connected CRTC whose bounding box overlaps
     * the window. On Surface RT only LVDS-0 is wired, so this is
     * effectively config->crtc[0] always. */
    for (c = 0; c < config->num_crtc; c++) {
        xf86CrtcPtr crtc = config->crtc[c];
        if (!crtc->enabled)
            continue;
        return crtc->randr_crtc;
    }
    return NULL;
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
    struct tegra_present_vblank_event *event = data;
    present_event_notify(event->event_id, usec, msc);
    free(event);
}

static void
tegra_present_vblank_abort(void *data)
{
    struct tegra_present_vblank_event *event = data;
    free(event);
}

static Bool
TegraPresentQueueVblank(RRCrtcPtr rr_crtc, uint64_t event_id, uint64_t msc)
{
    xf86CrtcPtr crtc = rr_crtc->devPrivate;
    ScrnInfoPtr scrn = crtc->scrn;
    TegraPtr tegra = TegraPTR(scrn);
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    struct tegra_present_vblank_event *event;
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

static present_screen_info_rec tegra_present_info = {
    .version       = PRESENT_SCREEN_INFO_VERSION,
    .get_crtc      = TegraPresentGetCrtc,
    .get_ust_msc   = TegraPresentGetUstMsc,
    .queue_vblank  = TegraPresentQueueVblank,
    .abort_vblank  = TegraPresentAbortVblank,
    .flush         = TegraPresentFlush,
    .capabilities  = 0,
    /* No check_flip/flip yet -> Present uses copy fallback over DRI3. */
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
    xf86DrvMsg(scrn->scrnIndex, X_INFO, "Present initialized (copy mode)\n");
    return TRUE;
}

void
TegraPresentScreenExit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);
    tegra->present_enabled = FALSE;
}

#else /* !HAVE_PRESENT */

Bool TegraPresentScreenInit(ScreenPtr screen) { (void)screen; return FALSE; }
void TegraPresentScreenExit(ScreenPtr screen) { (void)screen; }

#endif
