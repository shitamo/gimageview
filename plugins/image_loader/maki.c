/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * MAKI01 loader (.mki): the PC-9801 format before MAG (MAKI02), always
 * 640 x 400 in 16 colours.  As RECOIL (recoil.sourceforge.net) reads it:
 *
 *   0    "MAKI01A " or "MAKI01B "
 *   8    the machine and the user (24 bytes)
 *   32   u16 the size of flag B, u16, u16 the sizes of the pixels A and B
 *        (big endian; the pixels A and B are read as one)
 *   38   u16 extension flag
 *   40   u16 x0, y0, x1, y1: 0, 0, 640, 400
 *   48   the palette: 16 * (G, R, B), 4 bits in the high nibbles
 *   96   flag A: 8000 bits, one for each block of 4 lines x 4 bytes
 *        (8 pixels), the highest bit first
 *   1096 flag B: 16 bits for each block whose flag A is 1, one for each
 *        byte of the block (row by row)
 *        the pixels: a byte for each flag B bit that is 1
 *
 * A byte of the picture (2 pixels) is the same byte 2 lines above
 * (MAKI01A; 4 lines above for MAKI01B) XORed with its pixel byte if it has
 * one.
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

#define WIDTH   640
#define HEIGHT  400
#define BPL     (WIDTH / 2)            /* bytes per line */
#define BLOCKS  (HEIGHT / 4 * BPL / 4)  /* 8000 */
#define FLAG_A  96
#define FLAG_B  (FLAG_A + BLOCKS / 8)  /* 1096 */

static GimvImage *maki_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_maki_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "MAKI",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        maki_load,
   }
};

static const gchar *maki_extensions[] = { "mki" };

static GimvMimeTypeEntry maki_mime_types[] =
{
   {
      mime_type:      "image/x-maki",
      description:    N_("The MAKI image format"),
      extensions:     maki_extensions,
      extensions_len: G_N_ELEMENTS (maki_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_maki_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(maki_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("MAKI Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("MAKI01 images (PC-9801, before MAG)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


static inline guint
be16 (const guchar *p)
{
   return (p[0] << 8) | p[1];
}


GimvImage *
maki_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[8];
   GByteArray *file;
   const guchar *buf;
   gsize len, pos;
   guchar pal[16][3], lines[4][BPL];
   guint16 *have = NULL;
   guchar *dest = NULL;
   guint i, j, x, y, xor_row;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, 8) < 8) return NULL;
   if (memcmp (head, "MAKI01A ", 8) && memcmp (head, "MAKI01B ", 8)) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   buf = file->data;
   len = file->len;

   if (len < FLAG_B
       || be16 (buf + 40) != 0 || be16 (buf + 42) != 0
       || be16 (buf + 44) != WIDTH || be16 (buf + 46) != HEIGHT)
      goto ERROR;

   /* G, R, B; 16 levels in the high bits: to 0 .. 255 */
   for (i = 0; i < 16; i++) {
      const guchar *p = buf + 48 + i * 3;
      guchar grb[3] = { p[1], p[0], p[2] };
      for (j = 0; j < 3; j++) pal[i][j] = (grb[j] & 0xf0) | (grb[j] >> 4);
   }

   /* which bytes of each block have a pixel byte */
   have = g_new0 (guint16, BLOCKS);
   pos = FLAG_B;
   for (i = 0; i < BLOCKS; i++) {
      if (!((buf[FLAG_A + i / 8] >> (7 - i % 8)) & 1)) continue;
      if (pos + 2 > len) break;
      have[i] = be16 (buf + pos);
      pos += 2;
   }

   dest = g_malloc0 ((gsize) WIDTH * HEIGHT * 3);
   memset (lines, 0, sizeof (lines));
   xor_row = buf[6] == 'A' ? 2 : 0;

   /* a file cut short: the part read (the rest black) */
   for (y = 0; y < HEIGHT && pos <= len; y++) {
      guint row = y & 3;
      const guint16 *hv = have + (y / 4) * (BPL / 4);
      guchar *d = dest + (gsize) y * WIDTH * 3;

      for (x = 0; x < BPL; x++, d += 6) {
         guint v = lines[row ^ xor_row][x];
         if ((hv[x / 4] >> (15 - row * 4 - (x & 3))) & 1) {
            if (pos >= len) { pos = len + 1; break; }
            v ^= buf[pos++];
         }
         lines[row][x] = v;
         memcpy (d, pal[v >> 4], 3);
         memcpy (d + 3, pal[v & 0x0f], 3);
      }

      if (y % 50 == 49 && !gimv_image_loader_progress_update (loader))
         goto ERROR;
   }

   g_free (have);
   g_byte_array_free (file, TRUE);
   return gimv_image_create_from_data (dest, WIDTH, HEIGHT, FALSE);

ERROR:
   g_free (have);
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
