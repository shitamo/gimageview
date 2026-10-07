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
 * XPM (XPM3, C source) loader.  gdk-pixbuf builds its XPM loader only as a
 * "weakly maintained" optional loader.  The decoder is gimv_xpm.c.
 */

#include "config.h"

#include <string.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_image.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "loader_utils.h"
#include "gimv_xpm.h"

#define XPM_MAGIC "/* XPM */"

static GimvImage *xpm_load (GimvImageLoader *loader, gpointer data);

static GimvImageLoaderPlugin gimv_xpm_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "XPM",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CAN_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        xpm_load,
   }
};

static const gchar *xpm_extensions[] = { "xpm" };

static GimvMimeTypeEntry xpm_mime_types[] =
{
   {
      mime_type:      "image/x-xpixmap",
      description:    N_("The XPM image format"),
      extensions:     xpm_extensions,
      extensions_len: G_N_ELEMENTS (xpm_extensions),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_xpm_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(xpm_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("XPM Image Loader"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("X PixMap (XPM3) images"),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};


static GimvImage *
xpm_load (GimvImageLoader *loader, gpointer data)
{
   guchar head[64];
   guint len;
   const gchar *start;
   GByteArray *file;
   GPtrArray *strings;
   guchar *pixels;
   gint width, height;
   gboolean alpha;

   g_return_val_if_fail (loader, NULL);

   len = gimv_loader_peek (loader, head, sizeof (head) - 1);
   head[len] = '\0';
   start = (const gchar *) head;
   while (*start && g_ascii_isspace (*start)) start++;
   if (strncmp (start, XPM_MAGIC, strlen (XPM_MAGIC))) return NULL;

   file = gimv_loader_read_all (loader);
   if (!file) return NULL;

   /* the decoder lives in gimv (gimv_xpm.c): the built-in icons use it too */
   strings = gimv_xpm_get_strings ((const gchar *) file->data, file->len);
   pixels = gimv_xpm_decode ((const gchar * const *) strings->pdata, strings->len,
                             &width, &height, &alpha);
   g_ptr_array_free (strings, TRUE);
   g_byte_array_free (file, TRUE);

   if (!pixels) return NULL;

   return gimv_image_create_from_data (pixels, width, height, alpha);
}
