/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001 Takuro Ashie
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
 *
 * $Id: svg.c,v 1.8 2003/06/18 09:26:53 makeinu Exp $
 */

#include "svg.h"

#ifdef ENABLE_SVG


#include <stdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <librsvg/rsvg.h>
#include "fileutil.h"
#include "gimv_plugin.h"


static GimvImageLoaderPlugin gimv_rsvg_loader[] =
{
   {
      if_version:    GIMV_IMAGE_LOADER_IF_VERSION,
      id:            "SVG",
      priority_hint: GIMV_IMAGE_LOADER_PRIORITY_CANNOT_CANCEL,
      check_type:    NULL,
      get_info:      NULL,
      loader:        svg_load_image,
   }
};

static const gchar *svg_extensions[] =
{
   "svg", "svgz",
};

static GimvMimeTypeEntry svg_mime_types[] =
{
   {
      mime_type:      "image/svg",
      description:    "Scalable Vector Graphics",
      extensions:     svg_extensions,
      extensions_len: sizeof (svg_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
   {
      mime_type:      "image/svg+xml",
      description:    "Scalable Vector Graphics",
      extensions:     svg_extensions,
      extensions_len: sizeof (svg_extensions) / sizeof (gchar *),
      icon:           NULL,
   },
};

GIMV_PLUGIN_GET_IMPL(gimv_rsvg_loader, GIMV_PLUGIN_IMAGE_LOADER)
GIMV_PLUGIN_GET_MIME_TYPE(svg_mime_types)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("SVG Image Loader"),
   version:       "0.1.1",
   author:        N_("Takuro Ashie"),
   description:   NULL,
   get_implement: gimv_plugin_get_impl,
   get_mime_type: gimv_plugin_get_mime_type,
   get_prefs_ui:  NULL,
};

/* Convert a CAIRO_FORMAT_ARGB32 surface (native endian, premultiplied alpha)
   into a GdkPixbuf (RGBA, non-premultiplied). */
static GdkPixbuf *
pixbuf_from_argb32_surface (cairo_surface_t *surface)
{
   GdkPixbuf *pixbuf;
   gint width, height, src_stride, dest_stride, x, y;
   const guchar *src;
   guchar *dest;

   width      = cairo_image_surface_get_width (surface);
   height     = cairo_image_surface_get_height (surface);
   src_stride = cairo_image_surface_get_stride (surface);
   src        = cairo_image_surface_get_data (surface);

   pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
   if (!pixbuf) return NULL;
   dest_stride = gdk_pixbuf_get_rowstride (pixbuf);
   dest        = gdk_pixbuf_get_pixels (pixbuf);

   for (y = 0; y < height; y++) {
      const guint32 *s = (const guint32 *) (src + y * src_stride);
      guchar *d = dest + y * dest_stride;

      for (x = 0; x < width; x++) {
         guint32 pixel = s[x];
         guint a = pixel >> 24;

         if (a == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
         } else {
            d[0] = (((pixel >> 16) & 0xff) * 255 + a / 2) / a;
            d[1] = (((pixel >>  8) & 0xff) * 255 + a / 2) / a;
            d[2] = (((pixel      ) & 0xff) * 255 + a / 2) / a;
            d[3] = a;
         }
         d += 4;
      }
   }

   return pixbuf;
}


/* replacement of the deprecated rsvg_pixbuf_from_file_at_zoom () */
static GdkPixbuf *
svg_render_at_zoom (const gchar *filename, gdouble x_zoom, gdouble y_zoom)
{
   RsvgHandle *handle;
   GdkPixbuf *pixbuf = NULL;
   gdouble width, height;
   gint dest_width, dest_height;
   cairo_surface_t *surface;
   cairo_t *cr;
   RsvgRectangle viewport;
   GError *error = NULL;

   handle = rsvg_handle_new_from_file (filename, &error);
   if (!handle) {
      g_clear_error (&error);
      return NULL;
   }

   if (!rsvg_handle_get_intrinsic_size_in_pixels (handle, &width, &height)) {
      /* no absolute width/height: fall back to the viewBox, then to the
         extents of the drawing (like rsvg_handle_get_dimensions () did) */
      gboolean has_viewbox;
      RsvgRectangle viewbox, ink_rect, logical_rect;

      rsvg_handle_get_intrinsic_dimensions (handle, NULL, NULL, NULL, NULL,
                                            &has_viewbox, &viewbox);
      if (has_viewbox) {
         width  = viewbox.width;
         height = viewbox.height;
      } else {
         RsvgRectangle full = { 0.0, 0.0, 100000.0, 100000.0 };

         if (rsvg_handle_get_geometry_for_layer (handle, NULL, &full,
                                                 &ink_rect, &logical_rect,
                                                 NULL))
         {
            width  = logical_rect.x + logical_rect.width;
            height = logical_rect.y + logical_rect.height;
         } else {
            width = height = 0.0;
         }
      }
   }

   dest_width  = (gint) (width  * x_zoom + 0.5);
   dest_height = (gint) (height * y_zoom + 0.5);
   if (dest_width <= 0 || dest_height <= 0) {
      g_object_unref (handle);
      return NULL;
   }

   surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                         dest_width, dest_height);
   if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy (surface);
      g_object_unref (handle);
      return NULL;
   }

   cr = cairo_create (surface);
   cairo_scale (cr, x_zoom, y_zoom);
   viewport.x      = 0.0;
   viewport.y      = 0.0;
   viewport.width  = width;
   viewport.height = height;

   if (rsvg_handle_render_document (handle, cr, &viewport, &error)) {
      cairo_surface_flush (surface);
      pixbuf = pixbuf_from_argb32_surface (surface);
   } else {
      g_clear_error (&error);
   }

   cairo_destroy (cr);
   cairo_surface_destroy (surface);
   g_object_unref (handle);

   return pixbuf;
}


GimvImage *
svg_load_image (GimvImageLoader *loader, gpointer data)
{
   const gchar *filename;
   gfloat w_scale, h_scale;
   GimvImage *image = NULL;
   GdkPixbuf *pixbuf;

   g_return_val_if_fail (loader, NULL);

#warning FIXME!!
   if (loader->info) {
      if (gimv_image_info_is_movie (loader->info) ||
          gimv_image_info_is_audio (loader->info))
      {
         return NULL;
      }
   }

   filename = gimv_image_loader_get_path (loader);
   if (!filename || !*filename) return NULL;

   if (!gimv_image_loader_get_scale (loader, &w_scale, &h_scale)) {
      w_scale = 1.0;
      h_scale = 1.0;
   }

   pixbuf = svg_render_at_zoom (filename, w_scale, h_scale);

   if (pixbuf) {
      image = gimv_image_new ();
      image->image = pixbuf;
      image->flags |= GIMV_IMAGE_VECTOR_FLAGS;
      return image;
   } else {
      return NULL;
   }
}

#endif /* ENABLE_SVG */
