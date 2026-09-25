/* Copyright 2026 Wine-NX contributors. LGPL-2.1-or-later.
 * Shared FPS overlay rendering and font definitions. */
#ifndef WINE_NX_FPS_OVERLAY_H
#define WINE_NX_FPS_OVERLAY_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define WINE_NX_FPS_W 128
#define WINE_NX_FPS_H 32

static const uint8_t wine_nx_font5x7[14][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, /* 0 */
    {0x00, 0x42, 0x7f, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4b, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7f, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1e}, /* 9 */
    {0x7f, 0x09, 0x09, 0x01, 0x01}, /* F */
    {0x7f, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x26, 0x49, 0x49, 0x49, 0x32}, /* S */
    {0x00, 0x00, 0x00, 0x00, 0x00}  /* Space */
};

static inline int wine_nx_fps_badge_width( int fps_val )
{
    char text[16];
    int len, box_w;
    if (fps_val < 0) fps_val = 0;
    if (fps_val > 999) fps_val = 999;
    snprintf( text, sizeof(text), "%d FPS", fps_val );
    len = (int)strlen( text );
    box_w = len * 12 + 16;
    if (box_w > WINE_NX_FPS_W) box_w = WINE_NX_FPS_W;
    return box_w;
}

/* Renders a 128x32 FPS badge. rgba: 1 for RGBA, 0 for BGRA. */
static inline void wine_nx_fps_render_badge( int fps_val, uint32_t *buf, int rgba )
{
    char text[16];
    int len, cidx, col, r, px, py;
    int box_w, cur_x;
    uint32_t color_bg, color_border, color_shadow, color_text;

    if (fps_val < 0) fps_val = 0;
    if (fps_val > 999) fps_val = 999;
    snprintf( text, sizeof(text), "%d FPS", fps_val );
    len = (int)strlen( text );
    box_w = wine_nx_fps_badge_width( fps_val );

    if (rgba)
    {
        /* RGBA (Little-endian: R byte 0, G byte 1, B byte 2, A byte 3) */
        color_bg     = 0xb0141414u;
        color_border = 0xd0303030u;
        color_shadow = 0xff000000u;
        color_text   = 0xff00ff40u;
    }
    else
    {
        /* BGRA (Little-endian: B byte 0, G byte 1, R byte 2, A byte 3) */
        color_bg     = 0xb0141414u;
        color_border = 0xd0303030u;
        color_shadow = 0xff000000u;
        color_text   = 0xff40ff00u;
    }

    /* Dark translucent rounded background */
    for (py = 0; py < WINE_NX_FPS_H; py++)
    {
        for (px = 0; px < WINE_NX_FPS_W; px++)
        {
            if (px < box_w && py < 28)
            {
                int border = (px == 0 || px == box_w - 1 || py == 0 || py == 27);
                buf[py * WINE_NX_FPS_W + px] = border ? color_border : color_bg;
            }
            else
            {
                buf[py * WINE_NX_FPS_W + px] = 0;
            }
        }
    }

    /* Draw text with drop shadow */
    cur_x = 8;
    for (cidx = 0; cidx < len; cidx++)
    {
        char ch = text[cidx];
        int g = 13;
        if (ch >= '0' && ch <= '9') g = ch - '0';
        else if (ch == 'F') g = 10;
        else if (ch == 'P') g = 11;
        else if (ch == 'S') g = 12;

        /* Drop shadow */
        for (col = 0; col < 5; col++)
        {
            uint8_t bits = wine_nx_font5x7[g][col];
            for (r = 0; r < 7; r++)
            {
                if (bits & (1 << r))
                {
                    int gx = cur_x + col * 2;
                    int gy = 7 + r * 2;
                    for (int dy = 0; dy < 2; dy++)
                    {
                        for (int dx = 0; dx < 2; dx++)
                        {
                            if (gx + dx + 1 < WINE_NX_FPS_W && gy + dy + 1 < WINE_NX_FPS_H)
                                buf[(gy + dy + 1) * WINE_NX_FPS_W + (gx + dx + 1)] = color_shadow;
                        }
                    }
                }
            }
        }

        /* Bright lime green text */
        for (col = 0; col < 5; col++)
        {
            uint8_t bits = wine_nx_font5x7[g][col];
            for (r = 0; r < 7; r++)
            {
                if (bits & (1 << r))
                {
                    int gx = cur_x + col * 2;
                    int gy = 7 + r * 2;
                    for (int dy = 0; dy < 2; dy++)
                    {
                        for (int dx = 0; dx < 2; dx++)
                        {
                            if (gx + dx < WINE_NX_FPS_W && gy + dy < WINE_NX_FPS_H)
                                buf[(gy + dy) * WINE_NX_FPS_W + (gx + dx)] = color_text;
                        }
                    }
                }
            }
        }
        cur_x += 12;
    }
}

#endif /* WINE_NX_FPS_OVERLAY_H */
