/*****************************************************************************
 * window.c: generic window management
 *****************************************************************************
 * Copyright (C) 2009 Laurent Aimar
 * Copyright © 2009-2021 Rémi Denis-Courmont
 *
 * Authors: Laurent Aimar <fenrir _AT_ videolan _DOT_ org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <vlc_common.h>
#include <vlc_vout_window.h>
#include <vlc_modules.h>
#include "inhibit.h"
#include <libvlc.h>

typedef struct
{
    vout_window_t wnd;
    module_t *module;
    vlc_inhibit_t *inhibit;
    bool inhibit_windowed;
    bool active;
    bool fullscreen;
    vlc_mutex_t lock;
} window_t;

static int vout_window_start(void *func, bool forced, va_list ap)
{
    int (*activate)(vout_window_t *) = func;
    vout_window_t *wnd = va_arg(ap, vout_window_t *);

    int ret = activate(wnd);
    if (ret)
        vlc_objres_clear(VLC_OBJECT(wnd));
    (void) forced;
    return ret;
}

vout_window_t *vout_window_New(vlc_object_t *obj, const char *module,
                               const vout_window_owner_t *owner)
{
    window_t *w = vlc_custom_create(obj, sizeof(*w), "window");
    vout_window_t *window = &w->wnd;

    memset(&window->handle, 0, sizeof(window->handle));
    window->info.has_double_click = false;
    window->sys = NULL;
    assert(owner != NULL);
    window->owner = *owner;

    int dss = var_InheritInteger(obj, "disable-screensaver");

    w->inhibit = NULL;
    w->inhibit_windowed = dss == 1;
    w->active = false;
    w->fullscreen = false;
    vlc_mutex_init(&w->lock);

    w->module = vlc_module_load(window, "vout window", module, false,
                                vout_window_start, window);
    if (!w->module) {
        vlc_object_delete(window);
        return NULL;
    }

    /* Hook for screensaver inhibition */
    if (dss > 0) {
        vlc_inhibit_t *inh = vlc_inhibit_Create(VLC_OBJECT(window));

        vlc_mutex_lock(&w->lock);
        w->inhibit = inh;
        vlc_mutex_unlock(&w->lock);
    }
    return window;
}

int vout_window_Enable(vout_window_t *window,
                       const vout_window_cfg_t *restrict cfg)
{
    if (window->ops->enable != NULL) {
        int err = window->ops->enable(window, cfg);
        if (err)
            return err;
    }

    vout_window_SetInhibition(window, true);
    return VLC_SUCCESS;
}

void vout_window_Disable(vout_window_t *window)
{
    vout_window_SetInhibition(window, false);

    if (window->ops->disable != NULL)
        window->ops->disable(window);
}

void vout_window_Delete(vout_window_t *window)
{
    if (!window)
        return;

    window_t *w = container_of(window, window_t, wnd);

    if (w->inhibit != NULL) {
        vlc_inhibit_t *inh = w->inhibit;

        assert(!w->active);
        vlc_mutex_lock(&w->lock);
        w->inhibit = NULL;
        vlc_mutex_unlock(&w->lock);

        vlc_inhibit_Destroy(inh);
    }

    if (window->ops->destroy != NULL)
        window->ops->destroy(window);

    vlc_objres_clear(VLC_OBJECT(window));
    vlc_object_delete(window);
}

static void vout_window_UpdateInhibitionUnlocked(vout_window_t *window)
{
    window_t *w = container_of(window, window_t, wnd);
    unsigned flags = VLC_INHIBIT_NONE;

    vlc_mutex_assert(&w->lock);

    if (w->active && (w->inhibit_windowed || w->fullscreen))
        flags = VLC_INHIBIT_VIDEO;
    if (w->inhibit != NULL)
        vlc_inhibit_Set(w->inhibit, flags);
}

void vout_window_SetInhibition(vout_window_t *window, bool enabled)
{
    window_t *w = container_of(window, window_t, wnd);

    vlc_mutex_lock(&w->lock);
    w->active = enabled;

    vout_window_UpdateInhibitionUnlocked(window);
    vlc_mutex_unlock(&w->lock);
}

void vout_window_ReportWindowed(vout_window_t *window)
{
    window_t *w = container_of(window, window_t, wnd);

    if (!w->inhibit_windowed) {
        vlc_mutex_lock(&w->lock);
        w->fullscreen = false;

        vout_window_UpdateInhibitionUnlocked(window);
        vlc_mutex_unlock(&w->lock);
    }

    if (window->owner.cbs->windowed != NULL)
        window->owner.cbs->windowed(window);
}

void vout_window_ReportFullscreen(vout_window_t *window, const char *id)
{
    window_t *w = container_of(window, window_t, wnd);

    if (!w->inhibit_windowed) {
        vlc_mutex_lock(&w->lock);
        w->fullscreen = true;
        vout_window_UpdateInhibitionUnlocked(window);
        vlc_mutex_unlock(&w->lock);
    }

    if (window->owner.cbs->fullscreened != NULL)
        window->owner.cbs->fullscreened(window, id);
}
