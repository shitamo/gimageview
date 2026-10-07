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
 * WBMP (Wireless bitmap, type 0) loader.  gdk-pixbuf no longer has a WBMP
 * loader.  WBMP has no real magic (it starts with two zero bytes), so this
 * loader only takes files named *.wbmp whose size matches the header
 * exactly, and runs after the other loaders.
 */

#include "config.h"

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"

static GimvImage *wbmp_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_wbmp_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "WBMP",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_LOW,
      check_type:    NULL,
      get_info:      NULL,
      loader:        wbmp_load,
   }
};

static const gchar *wbmp_extensions[] = { "wbmp" };

static GimvMimeTypeEntry wbmp_mime_types[] =
{
   {
      mime_type:      "image/vnd.wap.wbmp",
      description:    N_("The WBMP image format"),
      extensions:     wbmp_extensions,
      extensions_len: G_N_ELEMENTS (wbmp_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_wbmp_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(wbmp_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("WBMP Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Wireless bitmap (WBMP type 0) images"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


/* multi-byte integer: 7 bits per byte, high bit set on all but the last */
static gboolean
wbmp_read_mbi (const guchar **p, const guchar *end, guint *value)
{
   guint v = 0, n;

   for (n = 0; n < 4; n++) {
      guchar c;
      if (*p >= end) return FALSE;
      c = *(*p)++;
      v = (v << 7) | (c & 0x7f);
      if (!(c & 0x80)) {
         *value = v;
         return TRUE;
      }
   }

   return FALSE;
}


static GimvImage *
wbmp_load (GimvImageLoader *loader, gpointer data)
{
   const gchar *path;
   guchar head[2];
   GByteArray *file;
   const guchar *p, *end;
   guint type, width, height, x, y;
   gsize row_bytes;
   guchar *dest, *d;

   g_return_val_if_fail (loader, NULL);

   path = gimv_image_loader_get_path (loader);
   if (!path) return NULL;
   {
      gchar *lower = g_ascii_strdown (path, -1);
      gboolean is_wbmp = g_str_has_suffix (lower, ".wbmp");
      g_free (lower);
      if (!is_wbmp) return NULL;
   }

   /* type 0, fix header field 0 */
   if (gimv_loader_peek (loader, head, 2) < 2) return NULL;
   if (head[0] != 0 || head[1] != 0) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;

   p   = file->data;
   end = file->data + file->len;
   if (!wbmp_read_mbi (&p, end, &type) || type != 0) goto ERROR;
   p++;                                             /* fix header field */
   if (!wbmp_read_mbi (&p, end, &width) || !wbmp_read_mbi (&p, end, &height))
      goto ERROR;
   if (!gimv_loader_size_ok (width, height)) goto ERROR;

   row_bytes = (width + 7) / 8;
   if ((gsize) (end - p) != row_bytes * height) goto ERROR;

   dest = g_try_malloc ((gsize) width * height * 3);
   if (!dest) goto ERROR;
   d = dest;

   for (y = 0; y < height; y++) {
      const guchar *row = p + row_bytes * y;
      for (x = 0; x < width; x++) {
         guchar v = (row[x / 8] & (0x80 >> (x % 8))) ? 255 : 0;   /* 1 is white */
         *d++ = v; *d++ = v; *d++ = v;
      }
   }

   g_byte_array_free (file, TRUE);

   return gimv_image_create_from_data (dest, width, height, FALSE);

ERROR:
   g_byte_array_free (file, TRUE);
   return NULL;
}
