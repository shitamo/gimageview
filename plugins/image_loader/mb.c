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
 * MB loader: an image format of the NEC PC-8801 (640 x 200, 8 colours),
 * passed around on PC-VAN about 1990.  No description of it was found:
 * this follows what MB2LOAD.COM 1.22 (a viewer of MB and MB2 for the
 * PC-9801) does, as it was seen run in an emulator.  MB2 (its header,
 * its comment) is not read.
 *
 * The file has no header: it is a stream of bytes packed by runs,
 *
 *   c < 0x80: the byte after it, c times (0: 256 times)
 *   c >= 0x80: the c & 0x7f bytes after it as they are
 *
 * and the stream gives the three planes of the screen, blue, red, green
 * (80 bytes a line, 200 lines), one after the other:
 *
 *   boxes, until a 0:
 *     u8  width in bytes (the low 7 bits)
 *     u8  height in lines (0: 256)
 *     u16 the place, big endian: the top 2 bits + 1 the number of bytes
 *         of the pattern, the low 14 bits the line * 80 + the byte
 *     the pattern: its bytes for the lines one after the other, again
 *     and again
 *   250 bytes: a bit for each of 2000 bytes (the highest bit first): 1,
 *     a byte of the stream follows; 0, 0
 *   these 2000 bytes, xored with those 10 further (from the end), then
 *     each bit xored with the bit before it (the lowest with the highest
 *     of the next byte), from the end, are a bit for each byte of the
 *     plane (10 a line, the highest bit first): 0, the byte is the next
 *     of the stream; 1, it is that of the boxes
 */

#include "config.h"

#include <string.h>

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *mb_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_mb_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "MB",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        mb_load,
   }
};

static const gchar *mb_extensions[] = { "mb" };

static GimvMimeTypeEntry mb_mime_types[] =
{
   {
      mime_type:      "image/x-pc88-mb",
      description:    N_("The MB image format (PC-8801)"),
      extensions:     mb_extensions,
      extensions_len: G_N_ELEMENTS (mb_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_mb_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(mb_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("MB Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("MB images (PC-8801)"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


#define MB_WIDTH    640
#define MB_HEIGHT   200
#define MB_LINE     80                        /* bytes */
#define MB_PLANE    (MB_LINE * MB_HEIGHT)
#define MB_MASK     (MB_PLANE / 8)            /* 2000 */


typedef struct {
   const guchar *p, *end;
   guint         repeat, literal;
   guchar        last;
   gboolean      over;
} Runs;


static guchar
next_byte (Runs *r)
{
   guchar c;

   if (r->repeat) {
      r->repeat--;
      return r->last;
   }
   if (r->literal) {
      r->literal--;
   } else {
      if (r->p >= r->end) {
         r->over = TRUE;
         return 0;
      }
      c = *r->p++;
      if (c & 0x80)
         r->literal = ((c & 0x7f) - 1) & 0xff;
      else
         r->repeat = (c - 1) & 0xff;
   }
   if (r->p >= r->end) {
      r->over = TRUE;
      return 0;
   }
   r->last = *r->p++;
   return r->last;
}


static gboolean
read_plane (Runs *r, guchar *plane)
{
   guchar mask[MB_MASK];
   guint i, boxes = 0;

   /* the boxes */
   for (;;) {
      guint w, h, place, n, k = 0, j;
      guchar first, pattern[4];

      first = next_byte (r);
      if (r->over) return FALSE;
      if (first == 0) break;
      if (++boxes > 65536) return FALSE;
      w = first & 0x7f;
      h = next_byte (r);
      place = next_byte (r) << 8;
      place |= next_byte (r);
      n = (place >> 14) + 1;
      place &= 0x3fff;
      for (j = 0; j < n; j++) pattern[j] = next_byte (r);
      if (r->over) return FALSE;

      do {
         gsize at = place;
         for (j = 0; j < w && at < MB_PLANE; j++, at++)
            plane[at] = pattern[k % n];
         place += MB_LINE;
         k++;
         h = (h - 1) & 0xff;
      } while (h != 0 && place < MB_PLANE);
   }

   /* where the bytes of the stream go */
   {
      guchar bits[MB_MASK / 8];
      guint b = 0;

      for (i = 0; i < G_N_ELEMENTS (bits); i++) bits[i] = next_byte (r);
      for (i = 0; i < MB_MASK; i++, b++)
         mask[i] = (bits[b / 8] >> (7 - b % 8)) & 1 ? next_byte (r) : 0;
      if (r->over) return FALSE;
   }
   for (i = MB_MASK - 10; i-- > 0; )
      mask[i] ^= mask[i + 10];
   {
      guchar after = mask[MB_MASK - 1];
      for (i = MB_MASK - 1; i-- > 0; ) {
         guint bit;
         guchar v = mask[i], x;

         x = (guchar) ((after >> 7) ^ v);       /* bit 0 */
         for (bit = 1; bit < 8; bit++)
            x ^= (guchar) ((x >> (bit - 1)) & 1) << bit;
         mask[i] = x;
         after = x;
      }
   }

   /* the bytes of the stream */
   for (i = 0; i < MB_PLANE; i++)
      if (!((mask[i / 8] >> (7 - i % 8)) & 1)) {
         plane[i] = next_byte (r);
         if (r->over) return FALSE;
      }
   return TRUE;
}


GimvImage *
mb_load (GimvImageLoader *loader, gpointer data)
{
   GByteArray *file;
   guchar *planes = NULL, *dest = NULL, head[4];
   Runs r;
   guint i, x, y;

   g_return_val_if_fail (loader, NULL);

   /* no header to tell it by: named .mb only (other loaders try their
      files on every loader), and not a Maya scene (.mb too) */
   {
      const gchar *path = gimv_image_loader_get_path (loader);
      const gchar *dot = path ? strrchr (path, '.') : NULL;
      if (!dot || g_ascii_strcasecmp (dot, ".mb")) return NULL;
   }
   if (gimv_loader_peek (loader, head, 4) < 4) return NULL;
   if (!memcmp (head, "FOR4", 4) || !memcmp (head, "FOR8", 4)) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;
   /* about 50 KB unpacked */
   if (file->len > 256 * 1024) goto ERROR;

   memset (&r, 0, sizeof (r));
   r.p = file->data;
   r.end = file->data + file->len;

   planes = g_malloc0 (MB_PLANE * 3);
   for (i = 0; i < 3; i++) {
      /* nothing but a whole picture: the format has nothing else to tell
         it from other files */
      if (!read_plane (&r, planes + i * MB_PLANE)) goto ERROR;
      if (!gimv_image_loader_progress_update (loader)) goto ERROR;
   }

   dest = g_malloc ((gsize) MB_WIDTH * MB_HEIGHT * 3);
   for (y = 0; y < MB_HEIGHT; y++)
      for (x = 0; x < MB_WIDTH; x++) {
         guint at = y * MB_LINE + x / 8, shift = 7 - x % 8;
         guchar *d = dest + ((gsize) y * MB_WIDTH + x) * 3;
         d[0] = (planes[MB_PLANE + at] >> shift) & 1 ? 255 : 0;       /* red */
         d[1] = (planes[MB_PLANE * 2 + at] >> shift) & 1 ? 255 : 0;   /* green */
         d[2] = (planes[at] >> shift) & 1 ? 255 : 0;                  /* blue */
      }

   g_free (planes);
   g_byte_array_free (file, TRUE);
   /* 200 lines on a screen of 400 at 4:3: the pixels 2.4 high */
   return gimv_loader_image_with_aspect (dest, MB_WIDTH, MB_HEIGHT, 1, 1, 12, 5);

ERROR:
   g_free (planes);
   g_free (dest);
   g_byte_array_free (file, TRUE);
   return NULL;
}
