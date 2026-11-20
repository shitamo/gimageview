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
 * Helpers shared by the small loaders added in the GTK4 port (PNM, XPM,
 * Sun raster, WBMP).  Every loader is tried on every file, so a loader first
 * peeks at the magic bytes and gives up at once on other files.
 */

#ifndef __GIMV_LOADER_UTILS_H__
#define __GIMV_LOADER_UTILS_H__

#include <string.h>
#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "gimv_io.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"

#define GIMV_LOADER_MAX_FILE_SIZE (512 * 1024 * 1024)
#define GIMV_LOADER_MAX_PIXELS    (256 * 1024 * 1024)

/* read up to len bytes from the start of the stream; returns the count */
static inline guint
gimv_loader_peek (GimvImageLoader *loader, guchar *buf, guint len)
{
   GimvIO *gio = gimv_image_loader_get_gio (loader);
   guint total = 0, bytes = 0;

   if (!gio) return 0;
   while (total < len) {
      GimvIOStatus status = gimv_io_read (gio, (gchar *) buf + total,
                                          len - total, &bytes);
      if (status != GIMV_IO_STATUS_NORMAL || bytes == 0) break;
      total += bytes;
   }
   gimv_io_seek (gio, 0, SEEK_SET);

   return total;
}


/* the whole stream (NULL on error, cancel or a too large file) */
static inline GByteArray *
gimv_loader_read_all (GimvImageLoader *loader)
{
   GimvIO *gio = gimv_image_loader_get_gio (loader);
   GByteArray *data;
   guchar buf[65536];
   guint bytes;

   if (!gio) return NULL;

   data = g_byte_array_new ();
   for (;;) {
      GimvIOStatus status = gimv_io_read (gio, (gchar *) buf, sizeof (buf), &bytes);
      if (status == GIMV_IO_STATUS_ERROR) goto ERROR;
      if (bytes == 0) break;
      g_byte_array_append (data, buf, bytes);
      if (data->len > GIMV_LOADER_MAX_FILE_SIZE) goto ERROR;
      if (!gimv_image_loader_progress_update (loader)) goto ERROR;
      if (status != GIMV_IO_STATUS_NORMAL) break;
   }

   return data;

ERROR:
   g_byte_array_free (data, TRUE);
   return NULL;
}


/* width x height fits and is sane */
static inline gboolean
gimv_loader_size_ok (gsize width, gsize height)
{
   return width > 0 && height > 0
      && width <= 65535 * 4 && height <= 65535 * 4
      && width * height <= GIMV_LOADER_MAX_PIXELS;
}

/*
 * The image of RGB data (taken, g_free ()d) shown as on the machine it
 * was made for: its pixels w_num / w_den wide and h_num / h_den high
 * (the 640 x 400 of the PC-9801 at 4:3: 1 x 1.2; the 512 x 512 of the
 * X68000: 4/3 x 1).  Not with GIMV_LOADER_DOT_ASPECT=0 in the
 * environment (the tests of the decoders).
 */
static inline GimvImage *
gimv_loader_image_with_aspect (guchar *rgb, gint width, gint height,
                               gint w_num, gint w_den, gint h_num, gint h_den)
{
   const gchar *env = g_getenv ("GIMV_LOADER_DOT_ASPECT");
   gint out_w = (gint) (((gint64) width * w_num + w_den / 2) / w_den);
   gint out_h = (gint) (((gint64) height * h_num + h_den / 2) / h_den);
   GdkPixbuf *src, *scaled;
   guchar *dest;
   gint y;

   if ((env && !strcmp (env, "0")) || (out_w == width && out_h == height)
       || out_w <= 0 || out_h <= 0 || !gimv_loader_size_ok (out_w, out_h))
      return gimv_image_create_from_data (rgb, width, height, FALSE);

   src = gdk_pixbuf_new_from_data (rgb, GDK_COLORSPACE_RGB, FALSE, 8,
                                   width, height, width * 3, NULL, NULL);
   scaled = src ? gdk_pixbuf_scale_simple (src, out_w, out_h, GDK_INTERP_BILINEAR) : NULL;
   if (src) g_object_unref (src);
   if (!scaled)
      return gimv_image_create_from_data (rgb, width, height, FALSE);
   g_free (rgb);

   dest = g_malloc ((gsize) out_w * out_h * 3);
   for (y = 0; y < out_h; y++)
      memcpy (dest + (gsize) y * out_w * 3,
              gdk_pixbuf_read_pixels (scaled) + (gsize) y * gdk_pixbuf_get_rowstride (scaled),
              (gsize) out_w * 3);
   g_object_unref (scaled);
   return gimv_image_create_from_data (dest, out_w, out_h, FALSE);
}

/*
 * The shape of the pixels by the machine an image was saved on (the 4
 * bytes of MAG and Pi: "PC98", "X68K", "88VA", often the name of the
 * program): the X68000 (its 512 dots wide modes) 4/3 wide, the MSX and
 * the FM TOWNS as they are, the others (the PC-9801 and the like, 640 x
 * 400 at 4:3) 1.2 high.
 */
static inline void
gimv_loader_dot_aspect_of (const guchar *machine, gint width,
                           gint *w_num, gint *w_den, gint *h_num, gint *h_den)
{
   gchar m[5];
   guint i;

   for (i = 0; i < 4; i++) m[i] = g_ascii_toupper (machine[i]);
   m[4] = '\0';
   *w_num = *w_den = *h_num = *h_den = 1;
   if (strstr (m, "68")) {
      if (width <= 512) {
         *w_num = 4;
         *w_den = 3;
      }
   } else if (strstr (m, "MSX") || strstr (m, "TOWN") || strstr (m, "FM")) {
      /* as they are */
   } else {
      *h_num = 6;
      *h_den = 5;
   }
}

#endif /* __GIMV_LOADER_UTILS_H__ */
