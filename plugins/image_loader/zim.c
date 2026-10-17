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
 * ZIM loader: the 16 colour image format of Z's STAFF KID98, a paint tool
 * for the NEC PC-9801 (400 and 900 line images).  The format is not
 * documented; this follows the decoder of emk's "HTML5 ZIM Viewer"
 * (https://emk.name/2016/01/zim.html), itself based on XGLOAD.
 *
 *   0x000  "FORMAT-A", then machine, file, tool and user names
 *   0x1fa  u16 n, n * u16
 *          u16 x 2, u16 sx, sy, ex, ey, 12 bytes, u16 compressed,
 *          u16 has_palette
 *          [16 * (B, R, G, 0) palette]
 *          u16 n, n * u16
 *   lines  u16 dots, u16 x, u16 y, data ... ; dots == 0 ends the image
 *
 * The data of a line is 4 planes of (dots + 7) / 8 bytes: intensity, G,
 * R and B.  Compressed lines are "u16 len, u16 size" and three levels of
 * bit masks (8, 64, 512 bits; a set bit means "byte present") marking the
 * non-zero bytes.  After expanding, every byte is XORed with the one
 * before it and then with the one two before it.
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

#define ZIM_MAGIC      "FORMAT-A"
#define ZIM_MAGIC_LEN  8
#define ZIM_TABLE_POS  0x1fa

static GimvImage *zim_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_zim_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "ZIM",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        zim_load,
   }
};

static const gchar *zim_extensions[] = { "zim" };

static GimvMimeTypeEntry zim_mime_types[] =
{
   {
      mime_type:      "image/x-zim",
      description:    N_("The ZIM image format (KID98)"),
      extensions:     zim_extensions,
      extensions_len: G_N_ELEMENTS (zim_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_zim_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(zim_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("ZIM Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("ZIM images of Z's STAFF KID98 (PC-9801)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


static inline guint
zim_le16 (const guchar *p)
{
   return p[0] | (p[1] << 8);
}


/* skip "u16 n, n * u16" */
static gboolean
zim_skip_table (gsize *pos, gsize len, const guchar *buf)
{
   if (*pos + 2 > len) return FALSE;
   *pos += 2 + (gsize) zim_le16 (buf + *pos) * 2;
   return *pos <= len;
}


/*
 * Expand one compressed line (p .. end) into out (size bytes, zeroed).
 * Bytes are taken in the order of the set bits of the mask levels.
 */
static void
zim_expand (const guchar *p, const guchar *end, guchar *out, guint size)
{
   guchar h2[8], h3[64];
   guint h1, i, b, n = 0;

   memset (h2, 0, sizeof (h2));
   memset (h3, 0, sizeof (h3));

   if (p >= end) return;
   h1 = *p++;
   for (i = 0; i < 8; i++)
      if ((h1 & (0x80 >> i)) && p < end) h2[i] = *p++;

   for (i = 0; i < 64; i++)
      if ((h2[i / 8] & (0x80 >> (i % 8))) && p < end) h3[i] = *p++;

   for (i = 0; i < 64 && n < size; i++) {
      for (b = 0; b < 8 && n < size; b++, n++) {
         if (h3[i] & (0x80 >> b)) {
            if (p >= end) return;
            out[n] = *p++;
         }
      }
   }
}


static GimvImage *
zim_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[ZIM_MAGIC_LEN];
   GByteArray *file;
   const guchar *buf;
   gsize len, pos;
   guint sx, sy, ex, ey, width, height, i;
   gboolean compressed, has_palette;
   guchar pal[16][3];
   guchar *dest = NULL, *line = NULL;
   gsize line_alloc = 0;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, ZIM_MAGIC_LEN) < ZIM_MAGIC_LEN)
      return NULL;
   if (memcmp (head, ZIM_MAGIC, ZIM_MAGIC_LEN)) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   buf = file->data;
   len = file->len;

   /* header */
   pos = ZIM_TABLE_POS;
   if (!zim_skip_table (&pos, len, buf)) goto ERROR;
   if (pos + 28 > len) goto ERROR;
   sx = zim_le16 (buf + pos + 4);
   sy = zim_le16 (buf + pos + 6);
   ex = zim_le16 (buf + pos + 8);
   ey = zim_le16 (buf + pos + 10);
   compressed  = zim_le16 (buf + pos + 24) != 0;
   has_palette = zim_le16 (buf + pos + 26) != 0;
   pos += 28;

   if (ex < sx || ey < sy) goto ERROR;
   width  = ex - sx + 1;
   height = ey - sy + 1;
   if (!gimv_loader_size_ok (width, height)) goto ERROR;

   /* palette */
   if (has_palette) {
      if (pos + 16 * 4 > len) goto ERROR;
      for (i = 0; i < 16; i++, pos += 4) {
         pal[i][0] = buf[pos + 1];   /* R */
         pal[i][1] = buf[pos + 2];   /* G */
         pal[i][2] = buf[pos];       /* B */
      }
   } else {
      /* PC-9801 digital 8 colours */
      for (i = 0; i < 16; i++) {
         pal[i][0] = (i & 2) ? 255 : 0;
         pal[i][1] = (i & 4) ? 255 : 0;
         pal[i][2] = (i & 1) ? 255 : 0;
      }
   }
   if (!zim_skip_table (&pos, len, buf)) goto ERROR;

   dest = g_try_malloc ((gsize) width * height * 3);
   if (!dest) goto ERROR;
   for (i = 0; i < width * height; i++) {
      dest[i * 3]     = pal[0][0];
      dest[i * 3 + 1] = pal[0][1];
      dest[i * 3 + 2] = pal[0][2];
   }

   /* lines */
   while (pos + 2 <= len) {
      guint dots, x, y, plane, n, px;
      const guchar *src;
      guchar *d;

      dots = zim_le16 (buf + pos);
      pos += 2;
      if (dots == 0) break;
      if (pos + 4 > len) break;
      x = zim_le16 (buf + pos);
      y = zim_le16 (buf + pos + 2);
      pos += 4;

      plane = (dots + 7) / 8;
      n = plane * 4;
      if (n > line_alloc) {
         g_free (line);
         line = g_try_malloc (n);
         if (!line) goto ERROR;
         line_alloc = n;
      }

      if (!compressed) {
         if (pos + n > len) break;
         memcpy (line, buf + pos, n);
         pos += n;
      } else {
         guint clen, size;
         if (pos + 2 > len) break;
         clen = zim_le16 (buf + pos);
         pos += 2;
         if (clen < 2 || pos + clen > len) break;
         size = zim_le16 (buf + pos);
         if (size > n) size = n;
         memset (line, 0, n);
         zim_expand (buf + pos + 2, buf + pos + clen, line, size);
         pos += clen;
         for (i = 1; i < size; i++) line[i] ^= line[i - 1];
         for (i = 2; i < size; i++) line[i] ^= line[i - 2];
         if (size / 4 != plane) {
            /* planes are laid out by the stored size */
            plane = size / 4;
            if (plane * 8 < dots) dots = plane * 8;
         }
      }

      if (y < sy || y > ey || x < sx || x > ex) continue;
      y -= sy;
      x -= sx;
      if (dots > width - x) dots = width - x;

      src = line;
      d = dest + ((gsize) y * width + x) * 3;
      for (px = 0; px < dots; px++) {
         guint byte = px >> 3, shift = 7 - (px & 7), c;
         c = (((src[byte]             >> shift) & 1) << 3)   /* intensity */
           | (((src[byte + plane]     >> shift) & 1) << 2)   /* G */
           | (((src[byte + plane * 2] >> shift) & 1) << 1)   /* R */
           |  ((src[byte + plane * 3] >> shift) & 1);        /* B */
         *d++ = pal[c][0];
         *d++ = pal[c][1];
         *d++ = pal[c][2];
      }

      if ((y & 63) == 0 && !gimv_image_loader_progress_update (loader))
         goto ERROR;
   }

   g_free (line);
   g_byte_array_free (file, TRUE);

   return gimv_image_create_from_data (dest, width, height, FALSE);

ERROR:
   g_free (line);
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
