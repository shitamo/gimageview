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
 * Sun raster loader: depth 1, 8 (colour map or gray), 24 and 32, plain or
 * byte-encoded (RLE).  gdk-pixbuf no longer has a Sun raster loader.
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

#define RAS_MAGIC        0x59a66a95
#define RAS_HEADER_SIZE  32

enum {
   RT_OLD          = 0,
   RT_STANDARD     = 1,
   RT_BYTE_ENCODED = 2,
   RT_FORMAT_RGB   = 3,
};

enum {
   RMT_NONE      = 0,
   RMT_EQUAL_RGB = 1,
   RMT_RAW       = 2,
};

static GimvImage *ras_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_ras_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "SunRaster",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        ras_load,
   }
};

static const gchar *ras_extensions[] = { "ras", "sun", "rs", "im1", "im8", "im24", "im32" };

static GimvMimeTypeEntry ras_mime_types[] =
{
   {
      mime_type:      "image/x-sun-raster",
      description:    N_("The Sun raster image format"),
      extensions:     ras_extensions,
      extensions_len: G_N_ELEMENTS (ras_extensions),
      icon:           NULL,
   },
   {
      mime_type:      "image/x-cmu-raster",
      description:    N_("The Sun raster image format"),
      extensions:     ras_extensions,
      extensions_len: 1,
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_ras_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(ras_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("Sun Raster Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Sun raster images (1, 8, 24 and 32 bits, RLE)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


static inline guint32
ras_be32 (const guchar *p)
{
   return ((guint32) p[0] << 24) | ((guint32) p[1] << 16)
      | ((guint32) p[2] << 8) | (guint32) p[3];
}


/* expand the byte-encoded (RLE) data: 0x80 0x00 is a literal 0x80,
   0x80 n v is n + 1 times v */
static guchar *
ras_decode_rle (const guchar *src, gsize src_len, gsize out_len)
{
   guchar *out = g_try_malloc0 (out_len);
   gsize i = 0, o = 0;

   if (!out) return NULL;

   while (o < out_len && i < src_len) {
      guchar c = src[i++];
      if (c != 0x80) {
         out[o++] = c;
      } else {
         guint count;
         if (i >= src_len) break;
         count = src[i++];
         if (count == 0) {
            out[o++] = 0x80;
         } else {
            guchar v;
            if (i >= src_len) break;
            v = src[i++];
            count++;
            while (count-- && o < out_len) out[o++] = v;
         }
      }
   }

   return out;
}


static GimvImage *
ras_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[RAS_HEADER_SIZE];
   GByteArray *file;
   guint32 width, height, depth, type, maptype, maplength;
   const guchar *map, *pixels;
   guchar *decoded = NULL, *dest = NULL, *d;
   gsize row_bytes, pixels_len, map_colors = 0;
   guint x, y;

   g_return_val_if_fail (loader, NULL);

   if (gimv_loader_peek (loader, head, RAS_HEADER_SIZE) < RAS_HEADER_SIZE)
      return NULL;
   if (ras_be32 (head) != RAS_MAGIC) return NULL;

   width     = ras_be32 (head + 4);
   height    = ras_be32 (head + 8);
   depth     = ras_be32 (head + 12);
   type      = ras_be32 (head + 20);
   maptype   = ras_be32 (head + 24);
   maplength = ras_be32 (head + 28);

   if (!gimv_loader_size_ok (width, height)) return NULL;
   if (depth != 1 && depth != 8 && depth != 24 && depth != 32) return NULL;
   if (type > RT_FORMAT_RGB) return NULL;
   if (maptype > RMT_RAW) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   if (file->len < RAS_HEADER_SIZE || maplength > file->len - RAS_HEADER_SIZE)
      goto ERROR;

   map = file->data + RAS_HEADER_SIZE;
   if (maptype == RMT_EQUAL_RGB) {
      map_colors = maplength / 3;
      if (map_colors > 256) map_colors = 256;
   }

   /* rows are padded to 16 bits */
   row_bytes  = (((gsize) width * depth + 15) / 16) * 2;
   pixels_len = row_bytes * height;
   pixels     = map + maplength;

   if (type == RT_BYTE_ENCODED) {
      decoded = ras_decode_rle (pixels, file->data + file->len - pixels, pixels_len);
      if (!decoded) goto ERROR;
      pixels = decoded;
   } else if ((gsize) (file->data + file->len - pixels) < pixels_len) {
      goto ERROR;
   }

   dest = g_try_malloc ((gsize) width * height * 3);
   if (!dest) goto ERROR;
   d = dest;

   for (y = 0; y < height; y++) {
      const guchar *row = pixels + row_bytes * y;

      for (x = 0; x < width; x++) {
         guint idx;

         switch (depth) {
         case 1:
            idx = (row[x / 8] & (0x80 >> (x % 8))) ? 1 : 0;
            if (map_colors >= 2) {
               *d++ = map[idx];
               *d++ = map[map_colors + idx];
               *d++ = map[map_colors * 2 + idx];
            } else {
               guchar v = idx ? 0 : 255;   /* set bits are black */
               *d++ = v; *d++ = v; *d++ = v;
            }
            break;
         case 8:
            idx = row[x];
            if (map_colors > 0) {
               if (idx >= map_colors) idx = map_colors - 1;
               *d++ = map[idx];
               *d++ = map[map_colors + idx];
               *d++ = map[map_colors * 2 + idx];
            } else {
               *d++ = idx; *d++ = idx; *d++ = idx;
            }
            break;
         case 24:
         case 32: {
            const guchar *p = row + x * (depth / 8) + (depth == 32 ? 1 : 0);
            if (type == RT_FORMAT_RGB) {
               *d++ = p[0]; *d++ = p[1]; *d++ = p[2];
            } else {                        /* BGR */
               *d++ = p[2]; *d++ = p[1]; *d++ = p[0];
            }
            break;
         }
         }
      }
   }

   g_free (decoded);
   g_byte_array_free (file, TRUE);

   return gimv_image_create_from_data (dest, width, height, FALSE);

ERROR:
   g_free (dest);
   g_free (decoded);
   g_byte_array_free (file, TRUE);
   return NULL;
}
