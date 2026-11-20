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
 * Pi loader: the 16 (and 256) colour image format of Yanagisawa's Pi
 * tools, common on the NEC PC-9801 and the Sharp X68000.
 *
 *   "Pi", a comment ending with 0x1a, a second one ending with 0x00
 *   u8  mode (bit 7: no palette)
 *   u8  aspect n, m
 *   u8  bits per pixel (4 or 8)
 *   4   the machine it was saved on ("PC98", "X68K", ...)
 *   u16 the length of the extra data (big endian), the extra data
 *   u16 width, height (big endian)
 *   palette: 16 or 256 * (R, G, B) unless mode bit 7
 *   the picture: a stream of bits, the highest first
 *
 * The picture is made of pairs of pixels.  It starts with two colours that
 * fill two lines above the picture; then, again and again, a place to copy
 * from (relative to where the next pair goes) and how many pairs:
 *
 *   00  0: two pixels to the left (four if those two are not alike)
 *   01  1: the line above
 *   10  2: two lines above
 *   110 3: the line above, one pixel to the right
 *   111 4: the line above, one pixel to the left
 *
 *   length: k ones, a zero, k bits v: 1 when k is 0, else 2^k + v
 *
 * The same place twice in a row means new colours instead: pairs of
 * colours, each followed by a bit (1: another pair).  A colour is the
 * position in a list kept for the colour to its left, the most recently
 * used first (1x: 0-1, 00x: 2-3, 010xx: 4-7, 011xxx: 8-15; with 256
 * colours 0110xxx: 8-15, ... 0111111xxxxxxx: 128-255); the colour used
 * is moved to the front.  The lists start as c, c - 1, c - 2, ...
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *pi_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_pi_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "Pi",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        pi_load,
   }
};

static const gchar *pi_extensions[] = { "pi" };

static GimvMimeTypeEntry pi_mime_types[] =
{
   {
      mime_type:      "image/x-pi",
      description:    N_("The Pi image format"),
      extensions:     pi_extensions,
      extensions_len: G_N_ELEMENTS (pi_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_pi_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(pi_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("Pi Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Pi images (PC-9801, X68000)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


typedef struct {
   const guchar *p, *end;
   guint         n;          /* the bit of *p next (0: the highest) */
   gboolean      over;       /* read past the end */
} Bits;


static inline guint
bit (Bits *b)
{
   guint v;

   if (b->p >= b->end) {
      b->over = TRUE;
      return 0;
   }
   v = (*b->p >> (7 - b->n)) & 1;
   if (++b->n == 8) {
      b->n = 0;
      b->p++;
   }
   return v;
}


static inline guint
bits (Bits *b, guint n)
{
   guint v = 0;
   while (n--) v = (v << 1) | bit (b);
   return v;
}


/* a colour after the colour c (to its left) */
static inline guint
read_color (Bits *b, guchar *table, guint ncolors, guint planes, guint c)
{
   guchar *list = table + (gsize) c * ncolors;
   guint idx, col, k;

   if (bit (b)) {
      idx = bit (b);
   } else if (!bit (b)) {
      idx = 2 + bit (b);
   } else {
      guint max = planes - 3;
      k = 0;
      while (k < max && bit (b)) k++;
      idx = (4u << k) + bits (b, k + 2);
   }
   if (idx >= ncolors) idx = ncolors - 1;
   col = list[idx];
   memmove (list + 1, list, idx);
   list[0] = col;
   return col;
}


static inline guint
read_length (Bits *b)
{
   guint k = 0;

   while (bit (b)) {
      if (++k > 30 || b->over) return 0;
   }
   return k == 0 ? 1 : (1u << k) + bits (b, k);
}


GimvImage *
pi_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[2];
   GByteArray *file;
   const guchar *buf;
   gsize len, pos;
   guint mode, planes, ncolors, width, height, i, j;
   guchar machine[4] = { 0, 0, 0, 0 };
   guchar pal[256][3];
   guchar *table = NULL, *pix = NULL, *dest = NULL;
   gsize total, at, end;
   gint prev_place = -1;
   guint steps = 0;
   Bits b;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, 2) < 2) return NULL;
   if (head[0] != 'P' || head[1] != 'i') return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   buf = file->data;
   len = file->len;

   /* the comments */
   pos = 2;
   while (pos < len && buf[pos] != 0x1a) pos++;
   pos++;
   while (pos < len && buf[pos] != 0x00) pos++;
   pos++;

   if (pos + 10 > len) goto ERROR;
   mode   = buf[pos];
   planes = buf[pos + 3];
   memcpy (machine, buf + pos + 4, 4);
   pos += 4 + 4;                                  /* aspect, planes, machine */
   pos += 2 + ((buf[pos] << 8) | buf[pos + 1]);   /* the extra data */
   if (pos + 4 > len) goto ERROR;
   width  = (buf[pos] << 8) | buf[pos + 1];
   height = (buf[pos + 2] << 8) | buf[pos + 3];
   pos += 4;

   if (planes != 4 && planes != 8) goto ERROR;
   ncolors = 1u << planes;
   if (width == 0 || height == 0 || !gimv_loader_size_ok (width, height)) goto ERROR;

   if (!(mode & 0x80)) {
      gboolean low = FALSE;
      if (pos + ncolors * 3 > len) goto ERROR;
      for (i = 0; i < ncolors; i++)
         for (j = 0; j < 3; j++) {
            pal[i][j] = buf[pos++];
            if (pal[i][j] & 0x0f) low = TRUE;
         }
      /* 16 levels in the high bits (the PC-9801 palette): to 0 .. 255 */
      if (!low)
         for (i = 0; i < ncolors; i++)
            for (j = 0; j < 3; j++) pal[i][j] |= pal[i][j] >> 4;
   } else if (ncolors == 16) {
      /* the PC-9801 colours: G R B, the high bit brighter */
      for (i = 0; i < 16; i++) {
         guchar on = (i & 8) ? 255 : 153;
         pal[i][0] = (i & 2) ? on : 0;
         pal[i][1] = (i & 4) ? on : 0;
         pal[i][2] = (i & 1) ? on : 0;
      }
   } else {
      for (i = 0; i < 256; i++) pal[i][0] = pal[i][1] = pal[i][2] = i;
   }

   /* the lists of colours */
   table = g_try_malloc ((gsize) ncolors * ncolors);
   if (!table) goto ERROR;
   for (i = 0; i < ncolors; i++)
      for (j = 0; j < ncolors; j++)
         table[(gsize) i * ncolors + j] = (i - j) & (ncolors - 1);

   /* the picture, with two lines above it */
   total = (gsize) width * (height + 2);
   pix = g_try_malloc0 (total);
   if (!pix) goto ERROR;

   b.p = buf + pos;
   b.end = buf + len;
   b.n = 0;
   b.over = FALSE;

   {
      guint c0 = read_color (&b, table, ncolors, planes, 0);
      guint c1 = read_color (&b, table, ncolors, planes, c0);
      for (at = 0; at < (gsize) width * 2; at++)
         pix[at] = (at & 1) ? c1 : c0;
   }

   at = (gsize) width * 2;
   end = total;
   while (at < end && !b.over) {
      gint place = bits (&b, 2);
      if (place == 3) place += bit (&b);

      if (place == prev_place) {
         /* new colours */
         do {
            guint c = read_color (&b, table, ncolors, planes, pix[at - 1]);
            pix[at++] = c;
            if (at >= end) break;
            pix[at] = read_color (&b, table, ncolors, planes, c);
            at++;
         } while (at < end && bit (&b) && !b.over);
         prev_place = -1;
      } else {
         gsize n = (gsize) read_length (&b) * 2, from;
         if (n == 0) break;
         switch (place) {
         case 0:  from = (pix[at - 1] == pix[at - 2]) ? 2 : 4; break;
         case 1:  from = width;     break;
         case 2:  from = width * 2; break;
         case 3:  from = width - 1; break;
         default: from = width + 1; break;
         }
         if (from > at || from == 0) from = 1;
         if (n > end - at) n = end - at;
         for (; n > 0; n--, at++) pix[at] = pix[at - from];
         prev_place = place;
      }

      if (++steps % 4096 == 0 && !gimv_image_loader_progress_update (loader))
         goto ERROR;
   }

   dest = g_try_malloc ((gsize) width * height * 3);
   if (!dest) goto ERROR;
   {
      const guchar *s = pix + (gsize) width * 2;
      guchar *d = dest;
      gsize k, n = (gsize) width * height;
      for (k = 0; k < n; k++, d += 3)
         memcpy (d, pal[s[k]], 3);
   }

   g_free (table);
   g_free (pix);
   g_byte_array_free (file, TRUE);
   {
      /* the aspect of the header is 0:0 in the files seen: by the machine */
      gint wn, wd, hn, hd;
      gimv_loader_dot_aspect_of (machine, width, &wn, &wd, &hn, &hd);
      return gimv_loader_image_with_aspect (dest, width, height, wn, wd, hn, hd);
   }

ERROR:
   g_free (table);
   g_free (pix);
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
