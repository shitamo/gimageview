/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * gdk-pixbuf image saver plugin for GImageView: JPEG, WebP, AVIF, TIFF,
 * BMP and ICO, as far as the gdk-pixbuf of the system can write them.
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
 */

#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#include "gimv_image.h"

#include "gimv_plugin.h"
#include "gimv_image_saver.h"

static gboolean save_pixbuf (GimvImageSaver *saver,
                             GimvImage      *image,
                             const gchar    *filename,
                             const gchar    *format);

static GimvImageSaverPlugin plugin_impl[] =
{
   {GIMV_IMAGE_SAVER_IF_VERSION, "jpeg", save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "jpg",  save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "webp", save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "avif", save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "tiff", save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "tif",  save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "bmp",  save_pixbuf, NULL},
   {GIMV_IMAGE_SAVER_IF_VERSION, "ico",  save_pixbuf, NULL},
};

GIMV_PLUGIN_GET_IMPL(plugin_impl, GIMV_PLUGIN_IMAGE_SAVER)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("GdkPixbuf Image Saver"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   N_("Saves images as JPEG, WebP, AVIF, TIFF, BMP and ICO "
                     "with the writers of gdk-pixbuf."),
   get_implement: gimv_plugin_get_impl,
   get_mime_type: NULL,
   get_prefs_ui:  NULL,
};


/* the gdk-pixbuf name of a format */
static const gchar *
pixbuf_type (const gchar *format)
{
   if (!g_ascii_strcasecmp (format, "jpg"))  return "jpeg";
   if (!g_ascii_strcasecmp (format, "tif"))  return "tiff";
   return format;
}


/* a copy of the pixels of image; without alpha (on white) when the format
 * has none */
static GdkPixbuf *
pixbuf_of (GimvImage *image, gboolean keep_alpha)
{
   gint w = gimv_image_width (image), h = gimv_image_height (image);
   gint rs = gimv_image_rowstride (image);
   gboolean alpha = gimv_image_has_alpha (image);
   guchar *pixels = gimv_image_get_pixels (image);
   GdkPixbuf *src, *copy;

   if (!pixels || w <= 0 || h <= 0) return NULL;
   src = gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, alpha, 8,
                                   w, h, rs, NULL, NULL);
   if (alpha && !keep_alpha) {
      copy = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
      gdk_pixbuf_fill (copy, 0xffffffff);
      gdk_pixbuf_composite (src, copy, 0, 0, w, h, 0, 0, 1, 1,
                            GDK_INTERP_NEAREST, 255);
   } else {
      copy = gdk_pixbuf_copy (src);
   }
   g_object_unref (src);
   return copy;
}


static gboolean
save_pixbuf (GimvImageSaver *saver,
             GimvImage      *image,
             const gchar    *filename,
             const gchar    *format)
{
   const gchar *type = pixbuf_type (format);
   const gchar *quality = gimv_image_saver_get_option (saver, "quality");
   gchar *keys[4] = { NULL }, *values[4] = { NULL };
   gint n = 0;
   gboolean alpha_ok, ok;
   GdkPixbuf *pixbuf;
   gchar *buf = NULL;
   gsize len = 0;
   GError *error = NULL;

   g_return_val_if_fail (image && filename && *filename, FALSE);

   alpha_ok = strcmp (type, "jpeg") && strcmp (type, "bmp");
   pixbuf = pixbuf_of (image, alpha_ok);
   if (!pixbuf) return FALSE;

   if (quality && (!strcmp (type, "jpeg") || !strcmp (type, "webp")
                   || !strcmp (type, "avif")))
   {
      keys[n] = "quality"; values[n] = (gchar *) quality; n++;
   }
   if (!strcmp (type, "tiff")) {
      /* LZW: no loss, smaller */
      keys[n] = "compression"; values[n] = "5"; n++;
   }

   /* into memory, then written whole (a file is not left half written) */
   ok = gdk_pixbuf_save_to_bufferv (pixbuf, &buf, &len, type, keys, values, &error);
   if (ok)
      ok = g_file_set_contents (filename, buf, len, &error);
   if (!ok) {
      gimv_image_saver_set_option (saver, "error",
                                   error ? error->message : "");
      g_clear_error (&error);
   }

   g_free (buf);
   g_object_unref (pixbuf);
   return ok;
}
