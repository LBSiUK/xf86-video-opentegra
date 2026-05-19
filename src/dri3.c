/*
 * SPDX-License-Identifier: MIT
 *
 * DRI3 hooks for xf86-video-opentegra.
 *
 * Skeleton: open_client is real (hands the client a render-node fd, which
 * is what Mesa needs at GLX init). pixmap_from_fd / fd_from_pixmap and the
 * modifier/format queries are stubs to be fleshed out next.
 */

#include "driver.h"

#ifdef HAVE_DRI3

#include <fcntl.h>
#include <unistd.h>

#include <xf86.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <exa.h>

#include "dri3.h"
#include "exa/exa.h"

static int
TegraDRI3OpenClient(ClientPtr client, ScreenPtr screen,
                    RRProviderPtr provider, int *out)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);
    char *render_node;
    int fd;

    render_node = drmGetRenderDeviceNameFromFd(tegra->fd);
    if (!render_node)
        return BadAlloc;

    fd = open(render_node, O_RDWR | O_CLOEXEC);
    free(render_node);
    if (fd < 0)
        return BadAlloc;

    *out = fd;
    return Success;
}

static PixmapPtr
TegraDRI3PixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                      CARD16 width, CARD16 height,
                      const CARD32 *strides, const CARD32 *offsets,
                      CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);
    struct drm_tegra_bo *imported_bo = NULL;
    struct tegra_pixmap *priv;
    PixmapPtr pixmap;
    int err;

    /* Single-plane formats only for now (no multi-planar YUV, etc). */
    if (num_fds != 1 || offsets[0] != 0 ||
        (modifier != DRM_FORMAT_MOD_LINEAR &&
         modifier != DRM_FORMAT_MOD_INVALID)) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3: pixmap_from_fds rejected "
                   "(num_fds=%u offset=%lu modifier=0x%llx)\n",
                   num_fds, (unsigned long)offsets[0],
                   (unsigned long long)modifier);
        return NULL;
    }

    err = drm_tegra_bo_from_dmabuf(&imported_bo, tegra->drm, fds[0], 0);
    if (err) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3: drm_tegra_bo_from_dmabuf failed: %d\n", err);
        return NULL;
    }

    /* Allocate a normal DRI-marked pixmap; we'll swap its bo for the
     * imported one. CreatePixmap with TEGRA_DRI_USAGE_HINT forces an
     * immediate BO allocation through the accelerated path, which is
     * the same shape we need. */
    pixmap = screen->CreatePixmap(screen, width, height, depth,
                                  TEGRA_DRI_USAGE_HINT);
    if (!pixmap) {
        drm_tegra_bo_unref(imported_bo);
        return NULL;
    }

    priv = exaGetPixmapDriverPrivate(pixmap);
    if (!priv || priv->type != TEGRA_EXA_PIXMAP_TYPE_BO || !priv->bo) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3: scratch pixmap had unexpected type %u\n",
                   priv ? priv->type : 0);
        drm_tegra_bo_unref(imported_bo);
        screen->DestroyPixmap(pixmap);
        return NULL;
    }

    /* Release the EXA-allocated BO and replace it with the imported one. */
    drm_tegra_bo_unref(priv->bo);
    priv->bo = imported_bo;
    priv->dri = true;
    priv->offscreen = true;
    priv->accel = true;
    drm_tegra_bo_forbid_caching(priv->bo);

    /* Set the stride the consumer specified (in case it differs from
     * our hardware-aligned default). */
    pixmap->devKind = strides[0];

    return pixmap;
}

static int
TegraDRI3FdsFromPixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                       uint32_t *strides, uint32_t *offsets,
                       uint64_t *modifier)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    struct tegra_pixmap *priv = exaGetPixmapDriverPrivate(pixmap);
    uint32_t fd = 0;
    int err;

    if (!priv) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3: fds_from_pixmap: no private\n");
        return 0;
    }

    /* Force allocation of deferred pixmaps so we have a BO to export. */
    TegraEXAThawPixmap(pixmap);

    if (priv->type != TEGRA_EXA_PIXMAP_TYPE_BO || !priv->bo) {
        xf86DrvMsg(scrn->scrnIndex, X_INFO,
                   "DRI3: fds_from_pixmap: unsupported pixmap "
                   "(type=%u after thaw)\n",
                   priv->type);
        return 0;
    }

    /* Don't export a buffer that has in-flight GPU writes — the consumer
     * may read it before fences resolve, producing tearing/junk. */
    TEGRA_PIXMAP_WAIT_WRITE_FENCES(priv);

    err = drm_tegra_bo_to_dmabuf(priv->bo, &fd);
    if (err) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3: drm_tegra_bo_to_dmabuf failed: %d\n", err);
        return 0;
    }

    /* Prevent BO caching from reusing this buffer behind our backs. */
    drm_tegra_bo_forbid_caching(priv->bo);
    priv->dri = true;

    fds[0]     = (int)fd;
    strides[0] = pixmap->devKind;
    offsets[0] = 0;
    *modifier  = DRM_FORMAT_MOD_LINEAR;

    return 1;
}

static int
TegraDRI3GetFormats(ScreenPtr screen, CARD32 *num_formats, CARD32 **formats)
{
    static const CARD32 supported[] = {
        DRM_FORMAT_XRGB8888,
        DRM_FORMAT_ARGB8888,
    };
    (void)screen;
    *num_formats = sizeof(supported) / sizeof(supported[0]);
    *formats = (CARD32 *)supported;
    return TRUE;
}

static int
TegraDRI3GetModifiers(ScreenPtr screen, uint32_t format,
                      uint32_t *num_modifiers, uint64_t **modifiers)
{
    static const uint64_t linear_only[] = { DRM_FORMAT_MOD_LINEAR };
    (void)screen; (void)format;
    *num_modifiers = 1;
    *modifiers = (uint64_t *)linear_only;
    return TRUE;
}

static int
TegraDRI3GetDrawableModifiers(DrawablePtr draw, uint32_t format,
                              uint32_t *num_modifiers, uint64_t **modifiers)
{
    static const uint64_t linear_only[] = { DRM_FORMAT_MOD_LINEAR };
    (void)draw; (void)format;
    *num_modifiers = 1;
    *modifiers = (uint64_t *)linear_only;
    return TRUE;
}

static const dri3_screen_info_rec tegra_dri3_info = {
    .version                = 2,
    .open_client            = TegraDRI3OpenClient,
    .pixmap_from_fds        = TegraDRI3PixmapFromFds,
    .fds_from_pixmap        = TegraDRI3FdsFromPixmap,
    .get_formats            = TegraDRI3GetFormats,
    .get_modifiers          = TegraDRI3GetModifiers,
    .get_drawable_modifiers = TegraDRI3GetDrawableModifiers,
};

Bool
TegraDRI3ScreenInit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);

    if (tegra->dri3_enabled)
        return TRUE;

    if (!dri3_screen_init(screen, &tegra_dri3_info)) {
        xf86DrvMsg(scrn->scrnIndex, X_WARNING,
                   "DRI3 init failed\n");
        return FALSE;
    }

    tegra->dri3_enabled = TRUE;
    xf86DrvMsg(scrn->scrnIndex, X_INFO, "DRI3 initialized\n");
    return TRUE;
}

void
TegraDRI3ScreenExit(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    TegraPtr tegra = TegraPTR(scrn);

    tegra->dri3_enabled = FALSE;
}

#else /* !HAVE_DRI3 */

Bool TegraDRI3ScreenInit(ScreenPtr screen) { (void)screen; return FALSE; }
void TegraDRI3ScreenExit(ScreenPtr screen) { (void)screen; }

#endif
