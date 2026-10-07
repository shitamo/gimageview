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

#endif /* __GIMV_LOADER_UTILS_H__ */
