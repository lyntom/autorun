/*
 * OpenGL for the Switch display driver
 *
 * Copyright 2026 Wine-NX contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __SWITCH__

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"
#include "wine/opengl_driver.h"
#include "wine/debug.h"
#include "../../wine-nx-probe/source/osk.h"
#include "../../wine-nx-probe/source/fps_overlay.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* The runtime hands the screen's NWindow to one OpenGL surface at a time
 * (wine-nx-probe/source/runtime.c). */
extern void *wine_nx_gl_acquire_window( void );
extern void wine_nx_gl_release_window( void );
extern void wine_nx_runtime_trace( const char *msg ) __attribute__((weak));

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs nx_drawable_funcs;

struct nx_gl_drawable
{
    struct opengl_drawable base;
    BOOL screen;  /* this drawable shares the screen's EGL surface */
};

/* The one EGL surface on the screen's NWindow, and the drawables sharing it.
 * win32u gives a window a second drawable whenever another context is made
 * current on it while the first is still in use - wined3d does exactly that
 * when it replaces its caps context with a versioned one - so the drawables
 * of a window share the surface rather than each asking for the screen. */
static pthread_mutex_t nx_screen_mutex = PTHREAD_MUTEX_INITIALIZER;
static EGLSurface nx_screen_surface;
static struct opengl_drawable *nx_screen_drawable;
static unsigned int nx_screen_refs;
static int nx_screen_format;
static unsigned int nx_last_swapped_calls;
static unsigned long long nx_last_swap_tick;

static struct nx_gl_drawable *impl_from_opengl_drawable( struct opengl_drawable *base )
{
    return CONTAINING_RECORD( base, struct nx_gl_drawable, base );
}

static void nx_log( const char *format, ... )
{
    char buffer[256];
    va_list args;

    if (!&wine_nx_runtime_trace) return;
    va_start( args, format );
    vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    wine_nx_runtime_trace( buffer );
}

/* The EGL config of a pixel format, as win32u's egldrv chooses it for pbuffers. */
static EGLConfig nx_config_for_format( int format )
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void nx_drawable_destroy( struct opengl_drawable *base )
{
    struct nx_gl_drawable *gl = impl_from_opengl_drawable( base );
    EGLSurface surface = NULL;

    if (!gl->screen)
    {
        /* win32u destroys the EGL surface after this callback. */
        return;
    }

    /* The screen's surface outlives this drawable while another one shares it;
     * take it away from win32u either way, so the framebuffer only gets the
     * NWindow back once the surface is really gone. */
    base->surface = NULL;

    pthread_mutex_lock( &nx_screen_mutex );
    if (nx_screen_refs && !--nx_screen_refs)
    {
        surface = nx_screen_surface;
        nx_screen_surface = NULL;
        nx_screen_drawable = NULL;
    }
    pthread_mutex_unlock( &nx_screen_mutex );

    if (!surface) return;
    funcs->p_eglDestroySurface( egl->display, surface );
    wine_nx_gl_release_window();
}

static BOOL nx_drawable_swap( struct opengl_drawable *base );

static void nx_drawable_flush( struct opengl_drawable *base, UINT flags )
{
    TRACE( "drawable %s, flags %#x\n", debugstr_opengl_drawable( base ), flags );

    if (flags & GL_FLUSH_INTERVAL) funcs->p_eglSwapInterval( egl->display, abs( base->interval ) );
    if (flags & (GL_FLUSH_PRESENT | GL_FLUSH_FORCE_SWAP)) nx_drawable_swap( base );
}

/* Frames presented and the time inside eglSwapBuffers, for [PROGRESS]. */
extern unsigned long long horizon_interrupt_time(void);
unsigned int wine_nx_gl_swaps;
unsigned long long wine_nx_gl_swap_time;  /* 100 ns */

/* The floating keyboard (wine-nx-probe/source/osk.c), put into the back
 * buffer just before it is shown: its picture in a texture of each context's
 * own, blitted from a framebuffer of its own onto the window's. Everything
 * the blit and the upload depend on is put back as the program left it. */
struct nx_osk_gl
{
    EGLContext context;
    GLuint texture, framebuffer;
    int width, height;
    unsigned int generation;
    void *pixels;
};
static struct nx_osk_gl nx_osk_gl[8];
static unsigned int nx_osk_gl_next;
static PFN_glGenFramebuffers p_glGenFramebuffers;
static PFN_glBindFramebuffer p_glBindFramebuffer;
static PFN_glFramebufferTexture2D p_glFramebufferTexture2D;
static PFN_glBlitFramebuffer p_glBlitFramebuffer;
static PFN_glIsFramebuffer p_glIsFramebuffer;
static PFN_glBindBuffer p_glBindBuffer;
static PFN_glCheckFramebufferStatus p_glCheckFramebufferStatus;

static void nx_resolve_fbo_funcs( void )
{
    if (!p_glGenFramebuffers)
        p_glGenFramebuffers = funcs->p_glGenFramebuffers ? funcs->p_glGenFramebuffers : (void *)funcs->p_eglGetProcAddress( "glGenFramebuffers" );
    if (!p_glBindFramebuffer)
        p_glBindFramebuffer = funcs->p_glBindFramebuffer ? funcs->p_glBindFramebuffer : (void *)funcs->p_eglGetProcAddress( "glBindFramebuffer" );
    if (!p_glFramebufferTexture2D)
        p_glFramebufferTexture2D = funcs->p_glFramebufferTexture2D ? funcs->p_glFramebufferTexture2D : (void *)funcs->p_eglGetProcAddress( "glFramebufferTexture2D" );
    if (!p_glIsFramebuffer)
        p_glIsFramebuffer = funcs->p_glIsFramebuffer ? funcs->p_glIsFramebuffer : (void *)funcs->p_eglGetProcAddress( "glIsFramebuffer" );
    if (!p_glBindBuffer)
        p_glBindBuffer = funcs->p_glBindBuffer ? funcs->p_glBindBuffer : (void *)funcs->p_eglGetProcAddress( "glBindBuffer" );
    if (!p_glCheckFramebufferStatus)
        p_glCheckFramebufferStatus = funcs->p_glCheckFramebufferStatus ? funcs->p_glCheckFramebufferStatus : (void *)funcs->p_eglGetProcAddress( "glCheckFramebufferStatus" );
    if (!p_glBlitFramebuffer)
    {
        if (funcs->p_glBlitFramebuffer) p_glBlitFramebuffer = funcs->p_glBlitFramebuffer;
        else if (funcs->p_glBlitFramebufferEXT) p_glBlitFramebuffer = funcs->p_glBlitFramebufferEXT;
        else p_glBlitFramebuffer = (void *)funcs->p_eglGetProcAddress( "glBlitFramebuffer" );
        if (!p_glBlitFramebuffer) p_glBlitFramebuffer = (void *)funcs->p_eglGetProcAddress( "glBlitFramebufferEXT" );
    }
}

static void nx_osk_draw( struct opengl_drawable *base )
{
    struct wine_nx_osk_frame frame;
    struct nx_osk_gl *osk = NULL;
    EGLint width = 0, height = 0;
    EGLContext context;
    GLint read_fb, draw_fb, texture, unpack_buffer, row_length, skip_pixels, skip_rows, alignment;
    GLboolean scissor, srgb;
    unsigned int i;

    if (!wine_nx_osk_visible()) return;
    if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) ||
        !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height ) ||
        !wine_nx_osk_frame( width, height, &frame ) || !(context = funcs->p_eglGetCurrentContext()))
        return;
    nx_resolve_fbo_funcs();
    if (!p_glBlitFramebuffer || !p_glGenFramebuffers || !p_glBindFramebuffer ||
        !p_glFramebufferTexture2D || !p_glIsFramebuffer || !p_glBindBuffer)
        return;
    for (i = 0; i < ARRAY_SIZE(nx_osk_gl) && !osk; i++)
        if (nx_osk_gl[i].context == context) osk = &nx_osk_gl[i];
    if (!osk)
    {
        /* A context not seen before: its names are its own, so the slot's
         * old ones are only forgotten, never deleted from here. */
        osk = &nx_osk_gl[nx_osk_gl_next++ % ARRAY_SIZE(nx_osk_gl)];
        free( osk->pixels );
        memset( osk, 0, sizeof(*osk) );
        osk->context = context;
    }

    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fb );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb );
    funcs->p_glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
    funcs->p_glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer );
    funcs->p_glGetIntegerv( GL_UNPACK_ROW_LENGTH, &row_length );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skip_pixels );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skip_rows );
    funcs->p_glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB );

    /* Names the program deleted, or a context that took an old one's place. */
    if (!osk->texture || !funcs->p_glIsTexture( osk->texture ) || !p_glIsFramebuffer( osk->framebuffer ))
    {
        funcs->p_glGenTextures( 1, &osk->texture );
        p_glGenFramebuffers( 1, &osk->framebuffer );
        osk->width = osk->height = 0;
    }
    funcs->p_glBindTexture( GL_TEXTURE_2D, osk->texture );
    p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
    if (osk->width != frame.width || osk->height != frame.height)
    {
        free( osk->pixels );
        osk->pixels = malloc( (size_t)frame.width * frame.height * 4 );
        funcs->p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0, GL_BGRA,
                               GL_UNSIGNED_BYTE, NULL );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        osk->width = frame.width;
        osk->height = frame.height;
        osk->generation = 0;
    }
    if (osk->pixels && osk->generation != frame.generation &&
        (osk->generation = wine_nx_osk_copy( width, height, osk->pixels, frame.width * 4, 0 )))
        funcs->p_glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_BGRA, GL_UNSIGNED_BYTE,
                                  osk->pixels );
    if (osk->generation)
    {
        p_glBindFramebuffer( GL_READ_FRAMEBUFFER, osk->framebuffer );
        p_glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, osk->texture, 0 );
        p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
        if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
        if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );
        /* The picture's first row is its top; the window's first is its bottom. */
        p_glBlitFramebuffer( 0, 0, frame.width, frame.height, frame.x, height - frame.y,
                             frame.x + frame.width, height - frame.y - frame.height, GL_COLOR_BUFFER_BIT, GL_NEAREST );
        if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
        if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );
    }

    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fb );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fb );
    funcs->p_glBindTexture( GL_TEXTURE_2D, texture );
    p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, unpack_buffer );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, row_length );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, skip_pixels );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, skip_rows );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );
}

extern int wine_nx_show_fps __attribute__((weak));

struct nx_fps_gl
{
    EGLContext context;
    GLuint texture;
    GLuint framebuffer;
    int last_fps_val;
    unsigned long long last_tick;
    unsigned int frame_count;
    int current_fps;
};

static struct nx_fps_gl nx_fps_slots[4];
static unsigned int nx_fps_next;

static void nx_fps_draw( struct opengl_drawable *base )
{
    struct nx_fps_gl *fps = NULL;
    EGLint width = 0, height = 0;
    EGLContext context;
    GLint read_fb = 0, draw_fb = 0, texture = 0, unpack_buffer = 0;
    GLint row_length = 0, skip_pixels = 0, skip_rows = 0, alignment = 4;
    GLint prev_read_buf = GL_NONE, prev_draw_buf = GL_NONE, prev_active_tex = GL_TEXTURE0;
    GLboolean prev_colormask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLboolean scissor = GL_FALSE, srgb = GL_FALSE, has_pbo = GL_FALSE;
    unsigned long long now, diff;
    unsigned int i;
    int badge_w, badge_h = 28;

    if (!&wine_nx_show_fps || !wine_nx_show_fps) return;
    if (!funcs || !egl || !egl->display || !base || !base->surface) return;
    if (!funcs->p_eglQuerySurface( egl->display, base->surface, EGL_WIDTH, &width ) ||
        !funcs->p_eglQuerySurface( egl->display, base->surface, EGL_HEIGHT, &height ) ||
        width <= 32 || height <= 32 ||
        !(context = funcs->p_eglGetCurrentContext()))
        return;

    nx_resolve_fbo_funcs();
    if (!p_glBlitFramebuffer || !p_glGenFramebuffers || !p_glBindFramebuffer ||
        !p_glFramebufferTexture2D || !p_glIsFramebuffer)
        return;

    for (i = 0; i < ARRAY_SIZE(nx_fps_slots) && !fps; i++)
        if (nx_fps_slots[i].context == context) fps = &nx_fps_slots[i];
    if (!fps)
    {
        fps = &nx_fps_slots[nx_fps_next++ % ARRAY_SIZE(nx_fps_slots)];
        memset( fps, 0, sizeof(*fps) );
        fps->context = context;
        fps->last_fps_val = -999;
    }

    /* Track real-time OpenGL frame rate */
    now = horizon_interrupt_time();
    if (!fps->last_tick)
    {
        fps->last_tick = now;
        fps->current_fps = 60;
    }
    fps->frame_count++;
    diff = now - fps->last_tick;
    /* 500ms in 100ns units = 5,000,000 */
    if (diff >= 5000000ULL)
    {
        fps->current_fps = (int)((fps->frame_count * 10000000ULL + diff / 2) / diff);
        fps->frame_count = 0;
        fps->last_tick = now;
    }

    /* Save state before mutating */
    if (funcs->p_glActiveTexture)
    {
        funcs->p_glGetIntegerv( GL_ACTIVE_TEXTURE, &prev_active_tex );
        funcs->p_glActiveTexture( GL_TEXTURE0 );
    }

    if (funcs->p_glGetBooleanv && funcs->p_glColorMask)
    {
        funcs->p_glGetBooleanv( GL_COLOR_WRITEMASK, prev_colormask );
        funcs->p_glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
    }

    funcs->p_glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &read_fb );
    funcs->p_glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb );
    funcs->p_glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );

    if (funcs->p_glReadBuffer) funcs->p_glGetIntegerv( GL_READ_BUFFER, &prev_read_buf );
    if (funcs->p_glDrawBuffer) funcs->p_glGetIntegerv( GL_DRAW_BUFFER, &prev_draw_buf );

    if (p_glBindBuffer)
    {
        funcs->p_glGetIntegerv( GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer );
        if (unpack_buffer)
        {
            has_pbo = GL_TRUE;
            p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, 0 );
        }
    }

    funcs->p_glGetIntegerv( GL_UNPACK_ROW_LENGTH, &row_length );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_PIXELS, &skip_pixels );
    funcs->p_glGetIntegerv( GL_UNPACK_SKIP_ROWS, &skip_rows );
    funcs->p_glGetIntegerv( GL_UNPACK_ALIGNMENT, &alignment );
    scissor = funcs->p_glIsEnabled( GL_SCISSOR_TEST );
    srgb = funcs->p_glIsEnabled ? funcs->p_glIsEnabled( GL_FRAMEBUFFER_SRGB ) : GL_FALSE;

    if (!fps->texture || !funcs->p_glIsTexture( fps->texture ) || !p_glIsFramebuffer( fps->framebuffer ))
    {
        funcs->p_glGenTextures( 1, &fps->texture );
        p_glGenFramebuffers( 1, &fps->framebuffer );
        fps->last_fps_val = -999;
    }

    funcs->p_glBindTexture( GL_TEXTURE_2D, fps->texture );
    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, 0 );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );

    badge_w = wine_nx_fps_badge_width( fps->current_fps );

    if (fps->last_fps_val != fps->current_fps)
    {
        uint32_t buf[WINE_NX_FPS_W * WINE_NX_FPS_H];
        wine_nx_fps_render_badge( fps->current_fps, buf, 0 ); /* BGRA */
        funcs->p_glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, WINE_NX_FPS_W, WINE_NX_FPS_H, 0, GL_BGRA,
                               GL_UNSIGNED_BYTE, buf );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
        funcs->p_glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
        fps->last_fps_val = fps->current_fps;
    }

    /* Bind source FBO */
    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, fps->framebuffer );
    p_glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fps->texture, 0 );
    if (funcs->p_glReadBuffer) funcs->p_glReadBuffer( GL_COLOR_ATTACHMENT0 );

    /* Bind destination window framebuffer (0) */
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
    if (funcs->p_glDrawBuffer) funcs->p_glDrawBuffer( GL_BACK );

    if (scissor) funcs->p_glDisable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glDisable( GL_FRAMEBUFFER_SRGB );

    if (!p_glCheckFramebufferStatus ||
        p_glCheckFramebufferStatus( GL_READ_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE)
    {
        /* Destination: (16, height - 16 - badge_h) to (16 + badge_w, height - 16).
         * Source: invert Y (badge_h to 0) to flip texture onto screen. */
        p_glBlitFramebuffer( 0, badge_h, badge_w, 0,
                             16, height - 16 - badge_h,
                             16 + badge_w, height - 16,
                             GL_COLOR_BUFFER_BIT, GL_NEAREST );
    }

    if (scissor) funcs->p_glEnable( GL_SCISSOR_TEST );
    if (srgb) funcs->p_glEnable( GL_FRAMEBUFFER_SRGB );

    if (funcs->p_glReadBuffer && prev_read_buf != GL_NONE) funcs->p_glReadBuffer( prev_read_buf );
    if (funcs->p_glDrawBuffer && prev_draw_buf != GL_NONE) funcs->p_glDrawBuffer( prev_draw_buf );
    if (funcs->p_glColorMask)
        funcs->p_glColorMask( prev_colormask[0], prev_colormask[1], prev_colormask[2], prev_colormask[3] );

    p_glBindFramebuffer( GL_READ_FRAMEBUFFER, read_fb );
    p_glBindFramebuffer( GL_DRAW_FRAMEBUFFER, draw_fb );
    funcs->p_glBindTexture( GL_TEXTURE_2D, texture );

    if (has_pbo) p_glBindBuffer( GL_PIXEL_UNPACK_BUFFER, unpack_buffer );

    funcs->p_glPixelStorei( GL_UNPACK_ROW_LENGTH, row_length );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_PIXELS, skip_pixels );
    funcs->p_glPixelStorei( GL_UNPACK_SKIP_ROWS, skip_rows );
    funcs->p_glPixelStorei( GL_UNPACK_ALIGNMENT, alignment );

    if (funcs->p_glActiveTexture) funcs->p_glActiveTexture( prev_active_tex );
}

static BOOL nx_drawable_swap( struct opengl_drawable *base )
{
    extern unsigned int wine_nx_gl_calls;
    unsigned long long start;
    BOOL ret;

    nx_osk_draw( base );
    nx_fps_draw( base );
    start = horizon_interrupt_time();
    ret = funcs->p_eglSwapBuffers( egl->display, base->surface );
    nx_last_swap_tick = horizon_interrupt_time();
    nx_last_swapped_calls = __atomic_load_n( &wine_nx_gl_calls, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_gl_swap_time, nx_last_swap_tick - start, __ATOMIC_RELAXED );
    __atomic_add_fetch( &wine_nx_gl_swaps, 1, __ATOMIC_RELAXED );
    return ret;
}


void wine_nx_gl_check_present( void )
{
    extern unsigned int wine_nx_gl_calls;
    unsigned int current_calls;
    unsigned long long now;
    struct opengl_drawable *drawable;

    if (!nx_screen_surface) return;

    current_calls = __atomic_load_n( &wine_nx_gl_calls, __ATOMIC_RELAXED );
    if (current_calls == nx_last_swapped_calls) return;

    now = horizon_interrupt_time();
    /* Throttle to ~60 FPS (16 ms = 160000 in 100ns units) */
    if (now - nx_last_swap_tick < 160000) return;

    pthread_mutex_lock( &nx_screen_mutex );
    drawable = nx_screen_drawable;
    if (drawable && nx_screen_surface && funcs && egl && egl->display)
    {
        nx_drawable_swap( drawable );
    }
    pthread_mutex_unlock( &nx_screen_mutex );
}

static const struct opengl_drawable_funcs nx_drawable_funcs =
{
    .destroy = nx_drawable_destroy,
    .flush = nx_drawable_flush,
    .swap = nx_drawable_swap,
};

/* A window surface covers the whole screen: the Switch has one NWindow, and
 * window surfaces and EGL cannot share it. Programs drawing with OpenGL are
 * expected to be full screen; other windows are not shown meanwhile. */
static BOOL nx_surface_create( HWND hwnd, BOOL raw, int format, struct opengl_drawable **drawable )
{
    struct opengl_drawable *previous;
    struct client_surface *client;
    struct nx_gl_drawable *gl;
    void *window;

    TRACE( "hwnd %p, raw %u, format %d\n", hwnd, raw, format );
    (void)raw;  /* win32u wraps the drawable in a framebuffer surface itself */

    if ((previous = *drawable) && previous->format == format) return TRUE;
    /* A previous surface may hold the screen: let it go first. */
    if (previous)
    {
        opengl_drawable_release( previous );
        *drawable = NULL;
    }

    if (!(client = nulldrv_client_surface_create( hwnd ))) return FALSE;
    gl = opengl_drawable_create( sizeof(*gl), &nx_drawable_funcs, format, client );
    client_surface_release( client );
    if (!gl) return FALSE;
    gl->base.buffer_map[0] = GL_BACK_LEFT;
    gl->base.buffer_map[1] = GL_BACK_RIGHT;
    gl->base.buffer_map[GL_FRONT - GL_FRONT_LEFT] = GL_BACK;
    gl->base.buffer_map[GL_FRONT_AND_BACK - GL_FRONT_LEFT] = GL_BACK;

    pthread_mutex_lock( &nx_screen_mutex );
    if (nx_screen_surface && nx_screen_format == format)
    {
        /* Another drawable of this window still has the screen; share it. */
        gl->base.surface = nx_screen_surface;
        gl->screen = TRUE;
        nx_screen_refs++;
        nx_screen_drawable = &gl->base;
        pthread_mutex_unlock( &nx_screen_mutex );
        TRACE( "hwnd %p: sharing the screen surface %p\n", hwnd, nx_screen_surface );
        *drawable = &gl->base;
        return TRUE;
    }
    if (nx_screen_surface)
    {
        pthread_mutex_unlock( &nx_screen_mutex );
        ERR( "hwnd %p: the screen has a format %d surface, cannot serve format %d\n",
             hwnd, nx_screen_format, format );
        goto err;
    }
    pthread_mutex_unlock( &nx_screen_mutex );

    if (!(window = wine_nx_gl_acquire_window()))
    {
        ERR( "hwnd %p: the screen already has an OpenGL surface\n", hwnd );
        goto err;
    }
    gl->screen = TRUE;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface( egl->display, nx_config_for_format( format ),
                                                              (EGLNativeWindowType)window, NULL )))
    {
        ERR( "hwnd %p: eglCreateWindowSurface failed, error %#x\n", hwnd, funcs->p_eglGetError() );
        /* nothing is sharing the screen yet, so give it back here */
        gl->screen = FALSE;
        wine_nx_gl_release_window();
        goto err;
    }

    pthread_mutex_lock( &nx_screen_mutex );
    nx_screen_surface = gl->base.surface;
    nx_screen_format = format;
    nx_screen_refs = 1;
    nx_screen_drawable = &gl->base;
    pthread_mutex_unlock( &nx_screen_mutex );

    TRACE( "created drawable %s with EGL surface %p\n", debugstr_opengl_drawable( &gl->base ), gl->base.surface );
    {
        static BOOL logged;
        const char *vendor = funcs->p_eglQueryString( egl->display, EGL_VENDOR );
        const char *version = funcs->p_eglQueryString( egl->display, EGL_VERSION );

        /* raw 0: win32u draws through its framebuffer surface (DPI scaling or gamma) */
        if (!logged) nx_log( "[NXGL] EGL %s %s, %u configs; window surface for format %d, raw %u",
                             vendor ? vendor : "?", version ? version : "?", egl->config_count, format, raw );
        logged = TRUE;
    }
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release( &gl->base );
    return FALSE;
}

/* win32u's egldrv creates contexts without a config, but switch-mesa 20.1's EGL
 * driver lacks EGL_KHR_no_config_context (such a context fails with
 * EGL_BAD_CONFIG) and EGL_KHR_create_context_no_error. A context gets the config
 * of its pixel format, the one this format's window surfaces and pbuffers use,
 * so EGL lets them be made current together. */
static BOOL nx_context_create( int format, void *share, const int *attribs, void **context )
{
    EGLint egl_attribs[16], *end = egl_attribs, error;

    TRACE( "format %d, share %p, attribs %p\n", format, share, attribs );

    for (; attribs && attribs[0]; attribs += 2)
    {
        EGLint name, *dst;

        switch (attribs[0])
        {
        case WGL_CONTEXT_MAJOR_VERSION_ARB:
            name = EGL_CONTEXT_MAJOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_MINOR_VERSION_ARB:
            name = EGL_CONTEXT_MINOR_VERSION_KHR;
            break;
        case WGL_CONTEXT_FLAGS_ARB:
            name = EGL_CONTEXT_FLAGS_KHR;
            break;
        case WGL_CONTEXT_PROFILE_MASK_ARB:
            if (attribs[1] & WGL_CONTEXT_ES2_PROFILE_BIT_EXT)
            {
                ERR( "OpenGL ES contexts are not supported\n" );
                return FALSE;
            }
            name = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
            break;
        case WGL_CONTEXT_OPENGL_NO_ERROR_ARB:
            FIXME( "no-error contexts are unavailable, ignoring %#x\n", attribs[1] );
            continue;
        default:
            FIXME( "unhandled attribute %#x %#x\n", attribs[0], attribs[1] );
            continue;
        }

        /* A repeated attribute replaces the earlier one. */
        for (dst = egl_attribs; dst != end && *dst != name; dst += 2) continue;
        if (dst == end)
        {
            if (end - egl_attribs >= (int)ARRAY_SIZE(egl_attribs) - 2) continue;
            end += 2;
        }
        dst[0] = name;
        dst[1] = attribs[1];
    }
    *end = EGL_NONE;

    funcs->p_eglBindAPI( EGL_OPENGL_API );
    *context = funcs->p_eglCreateContext( egl->display, nx_config_for_format( format ), share,
                                          end != egl_attribs ? egl_attribs : NULL );
    if ((error = funcs->p_eglGetError()) != EGL_SUCCESS || !*context)
    {
        ERR( "context creation failed for format %d, share %p, error %#x\n", format, share, error );
        return FALSE;
    }
    TRACE( "created context %p\n", *context );
    return TRUE;
}

/* Mesa's Switch platform is the default display; its windows are NWindows. */
static void nx_init_egl_platform( struct egl_platform *platform )
{
    platform->type = 0;
    platform->native_display = 0;
    egl = platform;
}

static struct opengl_driver_funcs nx_driver_funcs =
{
    .p_init_egl_platform = nx_init_egl_platform,
    .p_surface_create = nx_surface_create,
    .p_context_create = nx_context_create,
};

static BOOL (*p_orig_context_destroy)( void *context );

static BOOL nx_context_destroy( void *context )
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(nx_fps_slots); i++)
    {
        if (nx_fps_slots[i].context == (EGLContext)context)
        {
            memset( &nx_fps_slots[i], 0, sizeof(nx_fps_slots[i]) );
            nx_fps_slots[i].last_fps_val = -999;
        }
    }
    if (p_orig_context_destroy) return p_orig_context_destroy( context );
    return TRUE;
}

UINT wine_nx_drv_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs,
                             const struct opengl_driver_funcs **driver_funcs )
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR( "version mismatch, opengl32 wants %u but the driver has %u\n", version, WINE_OPENGL_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }
    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    nx_driver_funcs.p_get_proc_address = (*driver_funcs)->p_get_proc_address;
    nx_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    nx_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    nx_driver_funcs.p_init_wgl_extensions = (*driver_funcs)->p_init_wgl_extensions;
    p_orig_context_destroy = (*driver_funcs)->p_context_destroy;
    nx_driver_funcs.p_context_destroy = nx_context_destroy;
    nx_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;
    nx_driver_funcs.p_pbuffer_create = (*driver_funcs)->p_pbuffer_create;
    nx_driver_funcs.p_pbuffer_updated = (*driver_funcs)->p_pbuffer_updated;
    nx_driver_funcs.p_pbuffer_bind = (*driver_funcs)->p_pbuffer_bind;

    *driver_funcs = &nx_driver_funcs;
    return STATUS_SUCCESS;
}

BOOL wine_nx_gl_has_screen_surface( void )
{
    return nx_screen_surface != NULL;
}

#endif /* __SWITCH__ */

