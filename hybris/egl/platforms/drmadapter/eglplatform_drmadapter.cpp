/*
 * Copyright (c) 2026 D0gg0Man
 * Copyright (c) 2026 Furi Labs
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <android-config.h>
#include <ws.h>
#include <malloc.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libdrm/drm_fourcc.h>

extern "C" {
#include <eglplatformcommon.h>
}

#include "logging.h"

#include <hybris/gralloc/gralloc.h>
#include <hardware/gralloc.h>
#include <hybris/hwc2/hwc2_compatibility_layer.h>
#include <hybris/hwcomposerwindow/hwcomposer.h>

static hwc2_compat_device_t *hwc2_dev = NULL;
static hwc2_compat_display_t *hwc2_disp = NULL;
static hwc2_compat_layer_t *hwc2_layer = NULL;
static int hwc2_ready = 0;
static int32_t frame_w = 0;
static int32_t frame_h = 0;
static pthread_mutex_t hwc2_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Exported getters so external consumers (GLVND vendor wrapper, shims)
 * can reuse the same HWC2 objects instead of opening a second client. */
extern "C" hwc2_compat_display_t *drmadapter_get_hwc2_display(void) {
    return hwc2_disp;
}

extern "C" hwc2_compat_layer_t *drmadapter_get_hwc2_layer(void) {
    return hwc2_layer;
}

extern "C" void drmadapter_get_dimensions(int32_t *w, int32_t *h)
{
    if (w)
        *w = frame_w;
    if (h)
        *h = frame_h;
}

static void on_hotplug(HWC2EventListener *s, int32_t q, hwc2_display_t d,
                       bool connected, bool primary)
{
    (void)s;
    (void)q;
    (void)d;
    (void)connected;
    (void)primary;
}

static void on_vsync(HWC2EventListener *s, int32_t q, hwc2_display_t d, int64_t t)
{
    (void)s;
    (void)q;
    (void)d;
    (void)t;
}

static void on_refresh(HWC2EventListener *s, int32_t q, hwc2_display_t d)
{
    (void)s;
    (void)q;
    (void)d;
}

static void *init_hwc2(void *ud)
{
    HWC2EventListener *l = NULL;
    HWC2DisplayConfig *cfg = NULL;

    (void)ud;

    pthread_mutex_lock(&hwc2_mutex);

    if (hwc2_ready) {
        pthread_mutex_unlock(&hwc2_mutex);
        return NULL;
    }

    hybris_gralloc_initialize(0);

    hwc2_dev = hwc2_compat_device_new(false);

    if (!hwc2_dev) {
        HYBRIS_ERROR("drmadapter: hwc2_compat_device_new failed");
        goto out;
    }

    l = (HWC2EventListener *)calloc(1, sizeof(*l));

    if (!l) {
        HYBRIS_ERROR("drmadapter: failed to allocate HWC2 listener");
        goto out;
    }

    l->on_vsync_received = on_vsync;
    l->on_hotplug_received = on_hotplug;
    l->on_refresh_received = on_refresh;

    hwc2_compat_device_register_callback(hwc2_dev, l, 0);
    hwc2_compat_device_on_hotplug(hwc2_dev, 0, true);

    hwc2_disp = hwc2_compat_device_get_display_by_id(hwc2_dev, 0);

    if (!hwc2_disp) {
        HYBRIS_ERROR("drmadapter: no HWC2 display 0");
        goto out;
    }

    cfg = hwc2_compat_display_get_active_config(hwc2_disp);

    if (cfg) {
        frame_w = cfg->width;
        frame_h = cfg->height;
        free(cfg);
        cfg = NULL;
    }

    if (frame_w <= 0 || frame_h <= 0) {
        HYBRIS_ERROR("drmadapter: could not query display dimensions");
        goto out;
    }

    hwc2_compat_display_set_power_mode(hwc2_disp, 2);
    hwc2_compat_display_set_vsync_enabled(hwc2_disp, 1);

    hwc2_layer = hwc2_compat_display_create_layer(hwc2_disp);

    hwc2_compat_layer_set_composition_type(hwc2_layer, 4);
    hwc2_compat_layer_set_blend_mode(hwc2_layer, 1);
    hwc2_compat_layer_set_source_crop(hwc2_layer, 0, 0, frame_w, frame_h);
    hwc2_compat_layer_set_display_frame(hwc2_layer, 0, 0, frame_w, frame_h);
    hwc2_compat_layer_set_visible_region(hwc2_layer, 0, 0, frame_w, frame_h);

    hwc2_ready = 1;

out:
    if (cfg)
        free(cfg);

    pthread_mutex_unlock(&hwc2_mutex);

    return NULL;
}

extern "C" void drmadapterws_init_module(struct ws_egl_interface *egl_iface)
{
    eglplatformcommon_init(egl_iface);
    setenv("EGL_PLATFORM", "hwcomposer", 0);

    pthread_t t;
    pthread_create(&t, NULL, init_hwc2, NULL);
    pthread_detach(t);
}

extern "C" struct _EGLDisplay *drmadapterws_GetDisplay(EGLNativeDisplayType native)
{
    struct _EGLDisplay *dpy = new _EGLDisplay;
    dpy->display_id = EGL_DEFAULT_DISPLAY;
    dpy->dpy = EGL_NO_DISPLAY;
    return dpy;
}

extern "C" void drmadapterws_releaseDisplay(struct _EGLDisplay *dpy)
{
    delete dpy;
}

extern "C" void drmadapterws_Terminate(struct _EGLDisplay *dpy)
{
    (void)dpy;
}

extern "C" void drmadapterws_eglInitialized(struct _EGLDisplay *dpy)
{
    (void)dpy;
}

static struct ANativeWindow *hwc_native_win = NULL;

static void present_cb(void *ud, struct ANativeWindow *w,
                       struct ANativeWindowBuffer *buf)
{
    (void)ud;
    (void)w;

    if (!hwc2_ready || !hwc2_disp || !hwc2_layer || !buf)
        return;

    uint32_t nt = 0, nr = 0;
    hwc2_compat_display_validate(hwc2_disp, &nt, &nr);
    if (nt)
        hwc2_compat_display_accept_changes(hwc2_disp);

    hwc2_compat_layer_set_buffer(hwc2_layer, 0, buf, -1);
    hwc2_compat_display_set_client_target(hwc2_disp, 0, buf, -1,
                                          HAL_DATASPACE_UNKNOWN);

    int32_t fence = -1;
    hwc2_compat_display_present(hwc2_disp, &fence);
    HWCNativeBufferSetFence(buf, -1);
    if (fence >= 0)
        close(fence);
}

extern "C" EGLNativeWindowType drmadapterws_CreateWindow(EGLNativeWindowType win,
                                                         struct _EGLDisplay *display)
{
    (void)win;
    (void)display;

    /* HWC2 init runs in a background thread - wait for it */
    int retries = 0;
    while (!hwc2_ready && retries < 100) {
        usleep(50000);
        retries++;
    }

    if (!hwc2_ready)
        init_hwc2(NULL);

    if (!hwc_native_win && hwc2_disp && frame_w > 0)
        hwc_native_win = HWCNativeWindowCreate(frame_w, frame_h,
                                               HAL_PIXEL_FORMAT_RGBA_8888,
                                               present_cb, NULL);
    return (EGLNativeWindowType)hwc_native_win;
}

extern "C" void drmadapterws_DestroyWindow(EGLNativeWindowType win)
{
    (void)win;
}

extern "C" __eglMustCastToProperFunctionPointerType
drmadapterws_eglGetProcAddress(const char *procname)
{
    return eglplatformcommon_eglGetProcAddress(procname);
}

extern "C" void drmadapterws_passthroughImageKHR(EGLContext *ctx, EGLenum *target,
                                                 EGLClientBuffer *buffer,
                                                 const EGLint **attrib_list)
{
    eglplatformcommon_passthroughImageKHR(ctx, target, buffer, attrib_list);
}

extern "C" const char *drmadapterws_eglQueryString(EGLDisplay dpy, EGLint name,
                                                   const char *(*real_eglQueryString)(EGLDisplay, EGLint))
{
    return eglplatformcommon_eglQueryString(dpy, name, real_eglQueryString);
}

extern "C" void drmadapterws_prepareSwap(EGLDisplay dpy, EGLNativeWindowType win,
                                         EGLint *damage_rects, EGLint damage_n_rects)
{
    (void)dpy;
    (void)win;
    (void)damage_rects;
    (void)damage_n_rects;
}

extern "C" void drmadapterws_finishSwap(EGLDisplay dpy, EGLNativeWindowType win)
{
    (void)dpy;
    (void)win;
}

extern "C" void drmadapterws_setSwapInterval(EGLDisplay dpy, EGLNativeWindowType win,
                                             EGLint interval)
{
    (void)dpy;
    (void)win;
    (void)interval;
}

struct ws_module ws_module_info = {
    drmadapterws_init_module,
    drmadapterws_GetDisplay,
    drmadapterws_Terminate,
    drmadapterws_CreateWindow,
    drmadapterws_DestroyWindow,
    drmadapterws_eglGetProcAddress,
    drmadapterws_passthroughImageKHR,
    drmadapterws_eglQueryString,
    drmadapterws_prepareSwap,
    drmadapterws_finishSwap,
    drmadapterws_setSwapInterval,
    drmadapterws_releaseDisplay,
    drmadapterws_eglInitialized,
};
