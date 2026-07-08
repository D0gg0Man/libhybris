/****************************************************************************************
 **
 ** Copyright (C) 2013-2022 Jolla Ltd.
 ** Copyright (C) 2024 Jollyboys Ltd.
 ** All rights reserved.
 **
 ** This file is part of Wayland enablement for libhybris
 **
 ** You may use this file under the terms of the GNU Lesser General
 ** Public License version 2.1 as published by the Free Software Foundation
 ** and appearing in the file license.lgpl included in the packaging
 ** of this file.
 **
 ** This library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public
 ** License version 2.1 as published by the Free Software Foundation
 ** and appearing in the file license.lgpl included in the packaging
 ** of this file.
 **
 ** This library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ****************************************************************************************/


#include <android-config.h>
#include <hardware/gralloc.h>
#include "wayland_window.h"
#include <wayland-egl-backend.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <time.h>
#include <hybris/gralloc/gralloc.h>
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "logging.h"

/* gralloc HAL pixel formats (may already be defined via hardware/gralloc.h) */
#ifndef HAL_PIXEL_FORMAT_RGBA_8888
#define HAL_PIXEL_FORMAT_RGBA_8888 1
#endif
#ifndef HAL_PIXEL_FORMAT_RGBX_8888
#define HAL_PIXEL_FORMAT_RGBX_8888 2
#endif
#ifndef HAL_PIXEL_FORMAT_RGB_565
#define HAL_PIXEL_FORMAT_RGB_565 4
#endif
#ifndef GRALLOC_USAGE_SW_READ_OFTEN
#define GRALLOC_USAGE_SW_READ_OFTEN 0x00000003
#endif

#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=2 || ANDROID_VERSION_MAJOR>=5
extern "C" {
#include <sync/sync.h>
}
#endif

static void
buffer_create_sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
   struct wl_callback **created_callback = static_cast<struct wl_callback **>(data);

   *created_callback = NULL;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener buffer_create_sync_listener = {
   buffer_create_sync_callback
};

/* Create wnb->wlbuffer as an ARGB8888 wl_shm buffer + keep a CPU mapping in
 * shm_data. For compositors whose only CPU buffer path is wl_shm (KWin's
 * software/QPainter backend). Enabled per-client via env HYBRIS_WL_SHM. */
int WaylandNativeWindowBuffer::wlbuffer_from_shm(struct wl_shm *shm,
                                                 struct wl_event_queue *queue)
{
    int w = this->width, h = this->height;
    if (w <= 0 || h <= 0)
        return -1;
    unsigned long dst_stride = (unsigned long)w * 4;
    unsigned long size = dst_stride * (unsigned long)h;

    int fd = (int)syscall(SYS_memfd_create, "hybris-wl-shm", 0);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    void *data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return -1; }

    struct wl_shm *shm_wrapper = (struct wl_shm *) wl_proxy_create_wrapper(shm);
    wl_proxy_set_queue((struct wl_proxy *) shm_wrapper, queue);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm_wrapper, fd, (int32_t)size);
    /* HYBRIS_WL_SHM_XRGB: mark shm buffers opaque (XRGB) so software
     * compositors take the fast copy path instead of per-pixel blending.
     * "1" applies to every buffer; a larger value is an area threshold in
     * pixels; "auto" decides per FRAME by sampling the rendered alpha
     * (opaque frames -> XRGB fast path, translucent overlays -> ARGB), with
     * update_shm() setting shm_fmt_want and shm_apply_format() recreating
     * the wl_buffer from the same memfd when the state flips. */
    uint32_t shmfmt = WL_SHM_FORMAT_ARGB8888;
    {
        const char *x = getenv("HYBRIS_WL_SHM_XRGB");
        if (x && strcmp(x, "auto") != 0) {
            long thr = atol(x);
            if (thr <= 1 || (long)w * (long)h >= thr)
                shmfmt = WL_SHM_FORMAT_XRGB8888;
        }
    }
    this->wlbuffer = wl_shm_pool_create_buffer(pool, 0, w, h,
                                               (int32_t)dst_stride, shmfmt);
    wl_shm_pool_destroy(pool);
    wl_proxy_wrapper_destroy(shm_wrapper);

    this->shm_data = data;
    this->shm_size = size;
    this->shm_fd = fd;      /* kept open for shm_apply_format() recreation */
    this->shm_fmt = shmfmt;
    this->shm_fmt_want = shmfmt;
    if (getenv("HYBRIS_WL_SHM_DEBUG"))
        fprintf(stderr, "hybris_wl_shm: created shm buffer %dx%d stride=%lu fmt=%d wlbuffer=%p\n",
                w, h, dst_stride, this->format, (void*)this->wlbuffer);
    return this->wlbuffer ? 0 : -1;
}

/* Recreate the wl_buffer (same memfd/pixels) when update_shm() wants a
 * different opacity format than the current wl_buffer carries. Called from
 * finishSwap() BEFORE attach: this buffer is not referenced by any pending
 * commit, so destroying the old wl_buffer is safe. Returns 1 if recreated. */
int WaylandNativeWindowBuffer::shm_apply_format(struct wl_shm *shm,
                                                struct wl_event_queue *queue)
{
    if (this->shm_fd < 0 || !this->shm_data || this->shm_fmt_want == this->shm_fmt)
        return 0;
    struct wl_shm *shm_wrapper = (struct wl_shm *) wl_proxy_create_wrapper(shm);
    wl_proxy_set_queue((struct wl_proxy *) shm_wrapper, queue);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm_wrapper, this->shm_fd,
                                                  (int32_t)this->shm_size);
    struct wl_buffer *nb = wl_shm_pool_create_buffer(pool, 0, this->width, this->height,
                                                     (int32_t)((unsigned long)this->width * 4),
                                                     this->shm_fmt_want);
    wl_shm_pool_destroy(pool);
    wl_proxy_wrapper_destroy(shm_wrapper);
    if (!nb)
        return 0;
    if (this->wlbuffer)
        wl_buffer_destroy(this->wlbuffer);
    this->wlbuffer = nb;
    this->shm_fmt = this->shm_fmt_want;
    return 1;
}

/* Copy this frame's gralloc contents into the shm mirror, converting the
 * gralloc pixel layout to ARGB8888 (R/B swizzle for RGBA/RGBX). */
void WaylandNativeWindowBuffer::update_shm(void)
{
    if (!this->shm_data || !this->handle)
        return;
    int w = this->width, h = this->height;

    static const bool prof = getenv("HYBRIS_WL_SHM_PROF") != NULL;
    struct timespec tp0, tp1, tp2;
    if (prof) clock_gettime(CLOCK_MONOTONIC, &tp0);

    void *src = NULL;
    if (hybris_gralloc_lock(this->handle, GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, w, h, &src) != 0 || !src)
        return;
    if (prof) clock_gettime(CLOCK_MONOTONIC, &tp1);
    unsigned long dst_stride = (unsigned long)w * 4;
    unsigned char *s = (unsigned char *)src;
    unsigned char *d = (unsigned char *)this->shm_data;
    int swizzle = (this->format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                   this->format == HAL_PIXEL_FORMAT_RGBX_8888);

    /* 16-bit configs happen (EGL sorts smaller color buffers first when the
     * app requests no explicit sizes): expand RGB565 to the ARGB8888 mirror.
     * Reading such a buffer with the 32-bit path runs 2x past every row --
     * garbage on screen, then a fault past the buffer end. */
    if (this->format == HAL_PIXEL_FORMAT_RGB_565) {
        unsigned long src_stride = (unsigned long)this->stride * 2;
        for (int y = 0; y < h; y++) {
            const uint16_t *sp = (const uint16_t *)(s + (unsigned long)y * src_stride);
            uint32_t *dp = (uint32_t *)(d + (unsigned long)y * dst_stride);
            for (int x = 0; x < w; x++) {
                uint16_t p = sp[x];
                uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                dp[x] = 0xFF000000u |
                        (((r << 3) | (r >> 2)) << 16) |
                        (((g << 2) | (g >> 4)) << 8) |
                        ((b << 3) | (b >> 2));
            }
        }
        goto copied;
    }

    {
    unsigned long src_stride = (unsigned long)this->stride * 4;  /* stride is in pixels */
    /* Pass 1: stream the gralloc buffer (write-combined GPU memory) into the
     * cached shm mirror with sequential memcpy. Structured/strided reads (ld4)
     * stall badly on WC memory, but a linear memcpy runs at full burst speed. */
    if (src_stride == dst_stride) {
        memcpy(d, s, dst_stride * (unsigned long)h);        /* contiguous fast path */
    } else {
        for (int y = 0; y < h; y++)
            memcpy(d + (unsigned long)y * dst_stride,
                   s + (unsigned long)y * src_stride, dst_stride);
    }
    }

    /* Pass 2: swap R<->B in place on the now-cached mirror (fast). */
    if (swizzle) {
        unsigned long npx = (unsigned long)w * (unsigned long)h;
        unsigned long i = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
        unsigned char *db = d;
        for (; i + 16 <= npx; i += 16) {
            uint8x16x4_t px = vld4q_u8(db + i * 4);
            uint8x16_t r = px.val[0];
            px.val[0] = px.val[2];
            px.val[2] = r;
            vst4q_u8(db + i * 4, px);
        }
#endif
        uint32_t *dp = (uint32_t *)d;
        for (; i < npx; i++) {
            uint32_t p = dp[i];                             /* mem B,G,R,A after memcpy of R,G,B,A */
            dp[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
    }
copied:
    /* "auto" opacity: sample the mirror's alpha on a sparse grid; a frame
     * with every sampled alpha == 255 is submitted as XRGB (compositor fast
     * copy path), anything translucent as ARGB (correct blending). Formats
     * without alpha (RGBX/RGB565) are opaque by definition. */
    {
        static int aut = -1;
        if (aut < 0) {
            const char *x = getenv("HYBRIS_WL_SHM_XRGB");
            aut = (x && strcmp(x, "auto") == 0) ? 1 : 0;
        }
        if (aut) {
            int opaque = 1;
            if (this->format == HAL_PIXEL_FORMAT_RGBA_8888) {
                const uint32_t *px = (const uint32_t *)d;
                for (int gy = 0; gy < 16 && opaque; gy++) {
                    unsigned long row = ((unsigned long)gy * (h - 1) / 15) * (unsigned long)w;
                    for (int gx = 0; gx < 16; gx++) {
                        /* mirror holds ARGB words: alpha = top byte */
                        if ((px[row + (unsigned long)gx * (w - 1) / 15] >> 24) != 0xFF) {
                            opaque = 0;
                            break;
                        }
                    }
                }
            }
            this->shm_fmt_want = opaque ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;
        }
    }
    if (prof) clock_gettime(CLOCK_MONOTONIC, &tp2);
    hybris_gralloc_unlock(this->handle);

    if (prof) {
        static double accL = 0.0, accC = 0.0; static int n = 0;
        static struct timespec tfirst;
        if (n == 0) tfirst = tp0;
        accL += (tp1.tv_sec - tp0.tv_sec) * 1e3 + (tp1.tv_nsec - tp0.tv_nsec) / 1e6;
        accC += (tp2.tv_sec - tp1.tv_sec) * 1e3 + (tp2.tv_nsec - tp1.tv_nsec) / 1e6;
        if (++n >= 30) {
            double wall = (tp2.tv_sec - tfirst.tv_sec) * 1e3 + (tp2.tv_nsec - tfirst.tv_nsec) / 1e6;
            fprintf(stderr, "hybris_wl_shm: lock avg %.2f ms | copy avg %.2f ms | %.1f fps (%dx%d swizzle=%d)\n",
                    accL / n, accC / n, n * 1000.0 / wall, w, h, swizzle);
            accL = 0.0; accC = 0.0; n = 0;
        }
    }
}

void WaylandNativeWindowBuffer::wlbuffer_from_native_handle(struct android_wlegl *android_wlegl,
                                                            struct wl_display *display,
                                                            struct wl_event_queue *queue)
{
    struct wl_array ints;
    int *ints_data;
    struct android_wlegl_handle *wlegl_handle;

    wl_array_init(&ints);
    ints_data = (int*) wl_array_add(&ints, handle->numInts*sizeof(int));
    memcpy(ints_data, handle->data + handle->numFds, handle->numInts*sizeof(int));

    struct android_wlegl *android_wlegl_wrapper = (struct android_wlegl *) wl_proxy_create_wrapper(android_wlegl);
    wl_proxy_set_queue((struct wl_proxy *) android_wlegl_wrapper, queue);

    wlegl_handle = android_wlegl_create_handle(android_wlegl_wrapper, handle->numFds, &ints);

    wl_array_release(&ints);

    for (int i = 0; i < handle->numFds; i++) {
        android_wlegl_handle_add_fd(wlegl_handle, handle->data[i]);
    }

    wlbuffer = android_wlegl_create_buffer(android_wlegl_wrapper,
            width, height, stride,
            format, (uint32_t)usage, wlegl_handle);

    android_wlegl_handle_destroy(wlegl_handle);

    wl_proxy_wrapper_destroy(android_wlegl_wrapper);

    struct wl_display *display_wrapper = (struct wl_display *) wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *) display_wrapper, queue);

    creation_callback = wl_display_sync(display_wrapper);
    wl_callback_add_listener(creation_callback, &buffer_create_sync_listener, &creation_callback);

    wl_proxy_wrapper_destroy(display_wrapper);
}

void WaylandNativeWindow::resize(unsigned int width, unsigned int height)
{
    lock();
    this->m_defaultWidth = m_width = width;
    this->m_defaultHeight = m_height = height;
    unlock();
}

void WaylandNativeWindow::resize_callback(struct wl_egl_window *egl_window, void *)
{
    TRACE("%dx%d",egl_window->width,egl_window->height);
    ((WaylandNativeWindow *) egl_window->driver_private)->resize(egl_window->width, egl_window->height);
}

void WaylandNativeWindow::destroy_window_callback(void *data)
{
    WaylandNativeWindow *native = (WaylandNativeWindow*)data;

    native->lock();
    native->m_window = 0;
    native->unlock();
}

void WaylandNativeWindow::lock()
{
    pthread_mutex_lock(&this->mutex);
}

void WaylandNativeWindow::unlock()
{
    pthread_mutex_unlock(&this->mutex);
}

void
WaylandNativeWindow::sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
    int *done = static_cast<int *>(data);

    *done = 1;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
    WaylandNativeWindow::sync_callback
};

static void check_fatal_error(struct wl_display *display)
{
    int error = wl_display_get_error(display);

    if (error == 0)
        return;

    fprintf(stderr, "Wayland display got fatal error %i: %s\n", error, strerror(error));

    if (errno != 0)
        fprintf(stderr, "Additionally, errno was set to %i: %s\n", errno, strerror(errno));

    fprintf(stderr, "The display is now unusable, aborting.\n");
    abort();
}

WaylandNativeWindow::WaylandNativeWindow(struct wl_egl_window *window,
                                         struct wl_display *display,
                                         android_wlegl *wlegl)
    : m_android_wlegl(wlegl)
{
    HYBRIS_TRACE_BEGIN("wayland-platform", "create_window", "");
    this->m_window = window;
    this->m_window->driver_private = (void *) this;
    this->m_display = display;
    this->m_width = window->width;
    this->m_height = window->height;
    this->m_defaultWidth = window->width;
    this->m_defaultHeight = window->height;
    this->m_window->resize_callback = resize_callback;
    this->m_window->destroy_window_callback = destroy_window_callback;
    this->frame_callback = NULL;
    this->wl_queue = wl_display_create_queue(display);
    this->wl_dpy_wrapper = (struct wl_display *) wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *) wl_dpy_wrapper, wl_queue);
    this->wl_surface_wrapper = (struct wl_surface *) wl_proxy_create_wrapper(m_window->surface);
    wl_proxy_set_queue((struct wl_proxy *) wl_surface_wrapper, wl_queue);
    this->m_format = 1;

    const_cast<int&>(ANativeWindow::minSwapInterval) = 0;
    const_cast<int&>(ANativeWindow::maxSwapInterval) = 1;
    // This is the default as per the EGL documentation
    this->m_swap_interval = 1;
    {
        const char *e = getenv("HYBRIS_SWAP_INTERVAL");
        if (e) {
            int v = atoi(e);
            if (v >= 0) this->m_swap_interval = v > 1 ? 1 : v;
        }
    }

    m_usage = GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    /* wl_shm mirror path reads every frame back on the CPU; ask gralloc for
     * CPU-cacheable buffers so that readback isn't a write-combined stall. */
    if (getenv("HYBRIS_WL_SHM"))
        m_usage |= GRALLOC_USAGE_SW_READ_OFTEN;
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
    m_queueReads = 0;
    m_freeBufs = 0;
    m_damage_rects = NULL;
    m_damage_n_rects = 0;
    WaylandNativeWindow::setBufferCount(3);
    HYBRIS_TRACE_END("wayland-platform", "create_window", "");
}

WaylandNativeWindow::~WaylandNativeWindow()
{
    destroyBuffers();
    if (frame_callback)
        wl_callback_destroy(frame_callback);
    wl_proxy_wrapper_destroy(wl_surface_wrapper);
    wl_proxy_wrapper_destroy(wl_dpy_wrapper);
    wl_event_queue_destroy(wl_queue);
    if (m_window) {
        m_window->driver_private = NULL;
        m_window->resize_callback = NULL;
        m_window->destroy_window_callback = NULL;
    }
}

void WaylandNativeWindow::frame() {
    HYBRIS_TRACE_BEGIN("wayland-platform", "frame_event", "");

    this->frame_callback = NULL;

    HYBRIS_TRACE_END("wayland-platform", "frame_event", "");
}

// overloads from BaseNativeWindow
int WaylandNativeWindow::setSwapInterval(int interval) {
    TRACE("interval:%i", interval);

    if (interval < 0)
        interval = 0;
    if (interval > 1)
        interval = 1;

    /* HYBRIS_SWAP_INTERVAL overrides the app-requested interval. With 0 the
     * swap path stops blocking on the compositor's frame callback, so the
     * client's next frame (render + shm copy) overlaps the compositor's
     * composite of the previous one instead of serializing behind it;
     * throttling falls back to buffer availability. */
    {
        static int forced = -2;
        if (forced == -2) {
            const char *e = getenv("HYBRIS_SWAP_INTERVAL");
            forced = e ? atoi(e) : -1;
            if (forced > 1) forced = 1;
        }
        if (forced >= 0)
            interval = forced;
    }

    HYBRIS_TRACE_BEGIN("wayland-platform", "swap_interval", "=%d", interval);

    lock();
    m_swap_interval = interval;
    unlock();

    HYBRIS_TRACE_END("wayland-platform", "swap_interval", "");

    return 0;
}

void WaylandNativeWindow::releaseBuffer(struct wl_buffer *buffer)
{
    std::list<WaylandNativeWindowBuffer *>::iterator it;

    it = fronted.begin();

    for (; it != fronted.end(); ++it)
    {
        if ((*it)->wlbuffer == buffer)
            break;
    }
    assert(it != fronted.end());



    WaylandNativeWindowBuffer* wnb = *it;
    fronted.erase(it);
    HYBRIS_TRACE_COUNTER("wayland-platform", "fronted.size", "%lu", fronted.size());

    for (it = m_bufList.begin(); it != m_bufList.end(); ++it)
    {
        if ((*it) == wnb)
            break;
    }
    assert(it != m_bufList.end());
    HYBRIS_TRACE_BEGIN("wayland-platform", "releaseBuffer", "-%p", wnb);
    wnb->busy = 0;

    ++m_freeBufs;
    HYBRIS_TRACE_COUNTER("wayland-platform", "m_freeBufs", "%i", m_freeBufs);
    for (it = m_bufList.begin(); it != m_bufList.end(); ++it)
    {
        (*it)->youngest = 0;
    }
    wnb->youngest = 1;


    HYBRIS_TRACE_END("wayland-platform", "releaseBuffer", "-%p", wnb);
}

int WaylandNativeWindow::lockBuffer(BaseNativeWindowBuffer* buffer){
    WaylandNativeWindowBuffer *wnb = (WaylandNativeWindowBuffer*) buffer;
    (void)wnb;
    HYBRIS_TRACE_BEGIN("wayland-platform", "lockBuffer", "-%p", wnb);
    HYBRIS_TRACE_END("wayland-platform", "lockBuffer", "-%p", wnb);
    return NO_ERROR;
}

int WaylandNativeWindow::readQueue(bool block)
{
    int ret = 0;

    if (++m_queueReads == 1) {
        if (block) {
            ret = wl_display_dispatch_queue(m_display, wl_queue);
        } else {
            ret = wl_display_dispatch_queue_pending(m_display, wl_queue);
        }

        // all threads waiting on the false branch will wake and return now, so we
        // can safely set m_queueReads to 0 here instead of relying on every thread
        // to decrement it. This prevents a race condition when a thread enters readQueue()
        // before the one in this thread returns.
        // The new thread would go in the false branch, and there would be no thread in the
        // true branch, blocking the new thread and any other that will call readQueue in
        // the future.
        m_queueReads = 0;

        pthread_cond_broadcast(&cond);

        if (ret < 0) {
            TRACE("wl_display_dispatch_queue returned an error");
            check_fatal_error(m_display);
            return ret;
        }
    } else if (block) {
        while (m_queueReads > 0) {
            pthread_cond_wait(&cond, &mutex);
        }
    }

    return ret;
}

int WaylandNativeWindow::cancelBuffer(BaseNativeWindowBuffer* buffer, int fenceFd){
    std::list<WaylandNativeWindowBuffer *>::iterator it;
    WaylandNativeWindowBuffer *wnb = (WaylandNativeWindowBuffer*) buffer;

    lock();
    HYBRIS_TRACE_BEGIN("wayland-platform", "cancelBuffer", "-%p", wnb);

    /* Check first that it really is our buffer */
    for (it = m_bufList.begin(); it != m_bufList.end(); ++it)
    {
        if ((*it) == wnb)
            break;
    }
    assert(it != m_bufList.end());

    wnb->busy = 0;
    ++m_freeBufs;
    HYBRIS_TRACE_COUNTER("wayland-platform", "m_freeBufs", "%i", m_freeBufs);

    for (it = m_bufList.begin(); it != m_bufList.end(); ++it)
    {
        (*it)->youngest = 0;
    }
    wnb->youngest = 1;

    if (m_queueReads != 0) {
        // Some thread is waiting on wl_display_dispatch_queue(), possibly waiting for a wl_buffer.release
        // event. Since we have now cancelled a buffer push an artificial event so that the dispatch returns
        // and the thread can notice the cancelled buffer. This means there is a delay of one roundtrip,
        // but I don't see other solution except having one dedicated thread for calling wl_display_dispatch_queue().
        wl_callback_destroy(wl_display_sync(wl_dpy_wrapper));
    }

    HYBRIS_TRACE_END("wayland-platform", "cancelBuffer", "-%p", wnb);
    unlock();

    return 0;
}

unsigned int WaylandNativeWindow::width() const {
    TRACE("value:%i", m_width);
    return m_width;
}

unsigned int WaylandNativeWindow::height() const {
    TRACE("value:%i", m_height);
    return m_height;
}

unsigned int WaylandNativeWindow::format() const {
    TRACE("value:%i", m_format);
    return m_format;
}

unsigned int WaylandNativeWindow::defaultWidth() const {
    TRACE("value:%i", m_defaultWidth);
    return m_defaultWidth;
}

unsigned int WaylandNativeWindow::defaultHeight() const {
    TRACE("value:%i", m_defaultHeight);
    return m_defaultHeight;
}

unsigned int WaylandNativeWindow::queueLength() const {
    TRACE("WARN: stub");
    return 1;
}

unsigned int WaylandNativeWindow::type() const {
    TRACE("");
#if ANDROID_VERSION_MAJOR>=4 && ANDROID_VERSION_MINOR>=3 || ANDROID_VERSION_MAJOR>=5
    /* https://android.googlesource.com/platform/system/core/+/bcfa910611b42018db580b3459101c564f802552%5E!/ */
    return NATIVE_WINDOW_SURFACE;
#else
    return NATIVE_WINDOW_SURFACE_TEXTURE_CLIENT;
#endif
}

unsigned int WaylandNativeWindow::transformHint() const {
    TRACE("WARN: stub");
    return 0;
}

/*
 * returns the current usage of this window
 */
unsigned int WaylandNativeWindow::getUsage() const {
    return m_usage;
}

int WaylandNativeWindow::setBuffersFormat(int format) {
    lock();
    if (format != m_format)
    {
        TRACE("old-format:x%x new-format:x%x", m_format, format);
        m_format = format;
        /* Buffers will be re-allocated when dequeued */
    } else {
        TRACE("format:x%x", format);
    }
    unlock();
    return NO_ERROR;
}

void WaylandNativeWindow::destroyBuffer(WaylandNativeWindowBuffer* wnb)
{
    TRACE("wnb:%p", wnb);

    assert(wnb != NULL);

    int ret = 0;
    while (ret != -1 && wnb->creation_callback)
        ret = wl_display_dispatch_queue(m_display, wl_queue);

    if (wnb->creation_callback) {
        wl_callback_destroy(wnb->creation_callback);
        wnb->creation_callback = NULL;
    }

    if (wnb->wlbuffer)
        wl_buffer_destroy(wnb->wlbuffer);
    wnb->wlbuffer = NULL;
    if (wnb->shm_data) {
        munmap(wnb->shm_data, wnb->shm_size);
        wnb->shm_data = NULL;
        wnb->shm_size = 0;
    }
    if (wnb->shm_fd >= 0) {
        close(wnb->shm_fd);
        wnb->shm_fd = -1;
    }
    wnb->common.decRef(&wnb->common);
    m_freeBufs--;
}

void WaylandNativeWindow::destroyBuffers()
{
    TRACE("");

    std::list<WaylandNativeWindowBuffer*>::iterator it = m_bufList.begin();
    for (; it!=m_bufList.end(); ++it)
    {
        destroyBuffer(*it);
    }
    m_bufList.clear();
    m_freeBufs = 0;
}

WaylandNativeWindowBuffer *WaylandNativeWindow::addBuffer() {
    WaylandNativeWindowBuffer *wnb;

#ifndef HYBRIS_NO_SERVER_SIDE_BUFFERS
    wnb = new ServerWaylandBuffer(m_width, m_height, m_format, m_usage, m_android_wlegl, wl_queue);
    wl_display_roundtrip_queue(m_display, wl_queue);
#else
    wnb = new ClientWaylandBuffer(m_width, m_height, m_format, m_usage);
#endif
    m_bufList.push_back(wnb);
    ++m_freeBufs;

    TRACE("wnb:%p width:%i height:%i format:x%x usage:x%x",
         wnb, wnb->width, wnb->height, wnb->format, wnb->usage);

    return wnb;
}


int WaylandNativeWindow::setBufferCount(int cnt) {
    TRACE("cnt:%d", cnt);

    if ((int)m_bufList.size() == cnt)
        return NO_ERROR;

    lock();

    if ((int)m_bufList.size() > cnt) {
        /* Decreasing buffer count, remove from beginning */
        std::list<WaylandNativeWindowBuffer*>::iterator it = m_bufList.begin();
        for (int i = 0; i <= (int)m_bufList.size() - cnt; i++ )
        {
            destroyBuffer(*it);
            ++it;
            m_bufList.pop_front();
        }

    } else {
        /* Increasing buffer count, start from current size */
        for (int i = (int)m_bufList.size(); i < cnt; i++)
            (void)addBuffer();

    }

    unlock();

    return NO_ERROR;
}




int WaylandNativeWindow::setBuffersDimensions(int width, int height) {
    lock();
    if (m_width != width || m_height != height)
    {
        TRACE("old-size:%ix%i new-size:%ix%i", m_width, m_height, width, height);
        m_width = width;
        m_height = height;
        /* Buffers will be re-allocated when dequeued */
    } else {
        TRACE("size:%ix%i", width, height);
    }
    unlock();
    return NO_ERROR;
}

int WaylandNativeWindow::setUsage(uint64_t usage) {
    usage |= GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
    if (getenv("HYBRIS_WL_SHM"))
        usage |= GRALLOC_USAGE_SW_READ_OFTEN;

    lock();
    if (usage != m_usage)
    {
        TRACE("old-usage:x%" PRIx64 " new-usage:x%" PRIx64, m_usage, usage);
        m_usage = usage;
        /* Buffers will be re-allocated when dequeued */
    } else {
        TRACE("usage:x%" PRIx64, usage);
    }
    unlock();
    return NO_ERROR;
}

#ifdef HYBRIS_NO_SERVER_SIDE_BUFFERS

void ClientWaylandBuffer::init(struct android_wlegl *android_wlegl,
                                     struct wl_display *display,
                                     struct wl_event_queue *queue)
{
    wlbuffer_from_native_handle(android_wlegl, display, queue);
}

#else // HYBRIS_NO_SERVER_SIDE_BUFFERS

static void ssb_ints(void *data, android_wlegl_server_buffer_handle *, wl_array *ints)
{
    ServerWaylandBuffer *wsb = static_cast<ServerWaylandBuffer *>(data);
    wl_array_copy(&wsb->ints, ints);
}

static void ssb_fd(void *data, android_wlegl_server_buffer_handle *, int fd)
{
    ServerWaylandBuffer *wsb = static_cast<ServerWaylandBuffer *>(data);
    int *ptr = (int *)wl_array_add(&wsb->fds, sizeof(int));
    *ptr = fd;
}

static void ssb_buffer(void *data, android_wlegl_server_buffer_handle *,
                       wl_buffer *buffer,
                       int32_t format,
                       int32_t stride)
{
    ServerWaylandBuffer *wsb = static_cast<ServerWaylandBuffer *>(data);

    native_handle_t *native;
    int numFds = wsb->fds.size / sizeof(int);
    int numInts = wsb->ints.size / sizeof(int32_t);

    native = native_handle_create(numFds, numInts);

    memcpy(&native->data[0], wsb->fds.data, wsb->fds.size);
    memcpy(&native->data[numFds], wsb->ints.data, wsb->ints.size);
    /* ownership of fds passed to native_handle_t */
    wsb->fds.size = 0;

    wsb->handle = NULL;
    wsb->format = format;
    wsb->stride = stride;

    int ret = hybris_gralloc_import_buffer(native, &wsb->handle);

    native_handle_close(native);
    native_handle_delete(native);

    if (ret) {
        fprintf(stderr,"failed to register buffer\n");
        return;
    }

    wsb->common.incRef(&wsb->common);
    wsb->m_buf = buffer;
}

static const struct android_wlegl_server_buffer_handle_listener server_handle_listener = {
    ssb_fd,
    ssb_ints,
    ssb_buffer,
};

ServerWaylandBuffer::ServerWaylandBuffer(unsigned int w,
                                         unsigned int h,
                                         int _format,
                                         uint64_t _usage,
                                         android_wlegl *android_wlegl,
                                         struct wl_event_queue *queue)
                   : WaylandNativeWindowBuffer()
                   , m_buf(0)
{
    ANativeWindowBuffer::width = w;
    ANativeWindowBuffer::height = h;
    usage = _usage;

    wl_array_init(&ints);
    wl_array_init(&fds);

    struct android_wlegl *android_wlegl_wrapper = (struct android_wlegl *) wl_proxy_create_wrapper(android_wlegl);
    wl_proxy_set_queue((struct wl_proxy *) android_wlegl_wrapper, queue);

    ssb = android_wlegl_get_server_buffer_handle(android_wlegl_wrapper, width, height, _format, (uint32_t)_usage);
    wl_proxy_set_queue((struct wl_proxy *) ssb, queue);
    android_wlegl_server_buffer_handle_add_listener(ssb, &server_handle_listener, this);

    wl_proxy_wrapper_destroy(android_wlegl_wrapper);
}

ServerWaylandBuffer::~ServerWaylandBuffer()
{
    if (m_buf)
        wl_buffer_destroy(m_buf);

    hybris_gralloc_release(handle, 1);
    wl_array_release(&ints);
    wl_array_release(&fds);
    android_wlegl_server_buffer_handle_destroy(ssb);
}

void ServerWaylandBuffer::init(android_wlegl *, wl_display *, wl_event_queue *queue)
{
    wlbuffer = m_buf;
    m_buf = 0;
    wl_proxy_set_queue((struct wl_proxy *) wlbuffer, queue);
}

#endif // HYBRIS_NO_SERVER_SIDE_BUFFERS

// vim: noai:ts=4:sw=4:ss=4:expandtab
