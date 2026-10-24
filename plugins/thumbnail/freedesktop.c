/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * freedesktop.org thumbnail support plugin for GImageView
 *
 * Reads and writes the thumbnails shared by Thunar, Ristretto, Nautilus,
 * Dolphin, Geeqie, gThumb, ... in ~/.cache/thumbnails, following the
 * Thumbnail Managing Standard:
 *
 *   $XDG_CACHE_HOME/thumbnails/{normal,large,x-large,xx-large}/<md5 of URI>.png
 *   normal = 128px, large = 256px, x-large = 512px, xx-large = 1024px
 *
 * The PNG has the URI and the mtime of the image (Thumb::URI, Thumb::MTime);
 * a thumbnail whose Thumb::MTime is not the mtime of the image is old and
 * not used.  New thumbnails go to the folder for the thumbnail size of gimv
 * (96px: normal), written to a temporary file and renamed, mode 0600.
 * Thumbnails of the images in archives cannot be kept in this format.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"
#include <gmodule.h>

#include "fileutil.h"
#include "gfileutil.h"
#include "intl.h"
#include "gimv_image_loader.h"
#include "gimv_plugin.h"
#include "gimv_thumb_cache.h"
#include "prefs.h"

#define LABEL "freedesktop.org"

static GimvImage *load_thumb (const gchar *filename,
                              const gchar *cache_type,
                              GimvImageInfo *info);
static GimvImage *save_thumb (const gchar *filename,
                              const gchar *cache_type,
                              GimvImage   *image,
                              GimvImageInfo *info);
static gchar     *get_path   (const gchar *filename,
                              const gchar *cache_type);
static gboolean   get_size   (gint         width,
                              gint         height,
                              const gchar *cache_type,
                              gint        *width_ret,
                              gint        *height_ret);


static GimvThumbCacheLoader plugin_impl[] =
{
   {GIMV_THUMB_CACHE_LOADER_IF_VERSION,
    N_(LABEL),
    load_thumb, save_thumb,
    get_path, get_size,
    NULL, NULL, NULL, NULL, NULL},
};

GIMV_PLUGIN_GET_IMPL(plugin_impl, GIMV_PLUGIN_THUMB_CACHE)

GimvPluginInfo gimv_plugin_info =
{
   if_version:    GIMV_PLUGIN_IF_VERSION,
   name:          N_("freedesktop.org thumbnail support (~/.cache/thumbnails)"),
   version:       "0.1.0",
   author:        N_("shitamo"),
   description:   NULL,
   get_implement: gimv_plugin_get_impl,
   get_mime_type: NULL,
   get_prefs_ui:  NULL,
};


gchar *
g_module_check_init (GModule *module)
{
   return NULL;
}


void
g_module_unload (GModule *module)
{
}


/******************************************************************************
 *  sizes and paths
 ******************************************************************************/

static const struct {
   const gchar *dir;
   gint         size;
} sizes[] = {
   {"normal",    128},
   {"large",     256},
   {"x-large",   512},
   {"xx-large", 1024},
};
#define N_SIZES G_N_ELEMENTS (sizes)


/* the smallest size not smaller than the thumbnails of gimv */
static guint
wanted_size (void)
{
   guint i, want = conf.thumbwin_thumb_size > 0 ? conf.thumbwin_thumb_size : 96;

   for (i = 0; i < N_SIZES - 1; i++)
      if ((guint) sizes[i].size >= want) return i;
   return N_SIZES - 1;
}


static gchar *
file_uri (const gchar *filename)
{
   gchar *abspath, *uri;

   /* as file managers do: the absolute path, symbolic links not resolved */
   abspath = relpath2abs (filename);
   if (!abspath) return NULL;
   uri = g_filename_to_uri (abspath, NULL, NULL);
   g_free (abspath);
   return uri;
}


static gchar *
thumb_path (const gchar *uri, guint size)
{
   gchar *md5 = g_compute_checksum_for_string (G_CHECKSUM_MD5, uri, -1);
   gchar *name = g_strconcat (md5, ".png", NULL);
   gchar *path = g_build_filename (g_get_user_cache_dir (), "thumbnails",
                                   sizes[size].dir, name, NULL);
   g_free (md5);
   g_free (name);
   return path;
}


/* the thumbnail to read: the wanted size, or a larger one */
static gchar *
existing_path (const gchar *uri, guint *size_ret)
{
   guint i;

   for (i = wanted_size (); i < N_SIZES; i++) {
      gchar *path = thumb_path (uri, i);
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
         if (size_ret) *size_ret = i;
         return path;
      }
      g_free (path);
   }
   return NULL;
}


static gchar *
get_path (const gchar *filename, const gchar *cache_type)
{
   gchar *uri, *path;

   g_return_val_if_fail (filename, NULL);
   g_return_val_if_fail (cache_type, NULL);
   g_return_val_if_fail (!strcmp (cache_type, plugin_impl[0].label), NULL);

   uri = file_uri (filename);
   if (!uri) return NULL;
   path = existing_path (uri, NULL);
   if (!path) path = thumb_path (uri, wanted_size ());   /* where it will be */
   g_free (uri);
   return path;
}


static gboolean
get_size (gint width, gint height, const gchar *cache_type,
          gint *width_ret, gint *height_ret)
{
   gint max_size = sizes[wanted_size ()].size;

   g_return_val_if_fail (width_ret && height_ret, FALSE);
   *width_ret = *height_ret = -1;
   if (width < 1 || height < 1) return FALSE;

   /* not enlarged */
   if (width <= max_size && height <= max_size) {
      *width_ret = width;
      *height_ret = height;
   } else if (width > height) {
      *width_ret = max_size;
      *height_ret = MAX (1, (gint) ((gdouble) height * max_size / width + 0.5));
   } else {
      *width_ret = MAX (1, (gint) ((gdouble) width * max_size / height + 0.5));
      *height_ret = max_size;
   }
   return TRUE;
}


/******************************************************************************
 *  load
 ******************************************************************************/

static GimvImage *
load_thumb (const gchar *filename, const gchar *cache_type, GimvImageInfo *info)
{
   GimvImage *image;
   GdkPixbuf *pixbuf;
   gchar *uri, *thumb_file;
   const gchar *mtime;
   struct stat st;

   g_return_val_if_fail (filename, NULL);
   g_return_val_if_fail (cache_type, NULL);

   uri = file_uri (filename);
   if (!uri) return NULL;
   thumb_file = existing_path (uri, NULL);
   g_free (uri);
   if (!thumb_file) return NULL;

   /* gdk-pixbuf keeps the tEXt chunks (the image loaders of gimv do not) */
   pixbuf = gdk_pixbuf_new_from_file (thumb_file, NULL);
   g_free (thumb_file);
   if (!pixbuf) return NULL;

   /* old when Thumb::MTime is not the mtime of the image */
   mtime = gdk_pixbuf_get_option (pixbuf, "tEXt::Thumb::MTime");
   if (info && info->st.st_mtime) {
      st.st_mtime = info->st.st_mtime;
   } else if (stat (filename, &st) != 0) {
      st.st_mtime = 0;
   }
   if (mtime && st.st_mtime && g_ascii_strtoll (mtime, NULL, 10) != (gint64) st.st_mtime) {
      g_object_unref (pixbuf);
      return NULL;
   }

   image = gimv_image_new ();
   image->image = pixbuf;
   return image;
}


/******************************************************************************
 *  save
 ******************************************************************************/

/* the image loaded for the thumbnail of gimv can be smaller than the size
   to keep: load it again for that size */
static GimvImage *
image_for_size (const gchar *filename, GimvImage *image, gint max_size)
{
   GimvImageLoader *loader;
   GimvImage *bigger = NULL;
   gint w = -1, h = -1;

   gimv_image_get_size (image, &w, &h);
   if (w >= max_size || h >= max_size) return gimv_image_ref (image);

   loader = gimv_image_loader_new_with_file_name (filename);
   if (loader) {
      gimv_image_loader_set_load_type (loader, GIMV_IMAGE_LOADER_LOAD_THUMBNAIL);
      gimv_image_loader_set_size_request (loader, max_size, max_size, TRUE);
      gimv_image_loader_set_as_animation (loader, FALSE);
      gimv_image_loader_load (loader);
      bigger = gimv_image_loader_get_image (loader);
      if (bigger) gimv_image_ref (bigger);
      gimv_image_loader_unref (loader);
   }
   if (bigger) {
      gint bw = -1, bh = -1;
      gimv_image_get_size (bigger, &bw, &bh);
      if (bw * bh > w * h) return bigger;
      gimv_image_unref (bigger);
   }
   return gimv_image_ref (image);
}


static GimvImage *
save_thumb (const gchar *filename, const gchar *cache_type,
            GimvImage *image, GimvImageInfo *info)
{
   GimvImage *src, *imcache = NULL;
   gchar *uri, *thumb_file = NULL, *dir = NULL, *tmp = NULL;
   gchar mtime[32], size[32], width[16], height[16];
   gint im_width = -1, im_height = -1, w = -1, h = -1, fd;
   guint s = wanted_size ();
   struct stat st;
   GError *error = NULL;

   g_return_val_if_fail (filename, NULL);
   g_return_val_if_fail (image, NULL);
   g_return_val_if_fail (cache_type, NULL);

   if (info && info->st.st_mtime) st = info->st;
   else if (stat (filename, &st) != 0) return NULL;

   uri = file_uri (filename);
   if (!uri) return NULL;

   src = image_for_size (filename, image, sizes[s].size);
   gimv_image_get_size (src, &im_width, &im_height);
   if (im_width < 1 || im_height < 1
       || !get_size (im_width, im_height, cache_type, &w, &h))
      goto END;

   imcache = (w == im_width && h == im_height) ? gimv_image_ref (src)
                                                : gimv_image_scale (src, w, h);
   if (!imcache || !GDK_IS_PIXBUF (imcache->image)) goto END;

   thumb_file = thumb_path (uri, s);
   dir = g_path_get_dirname (thumb_file);
   if (g_mkdir_with_parents (dir, 0700) != 0) goto END;

   /* write to a temporary file and rename, so that others never read a
      half written one */
   tmp = g_strconcat (thumb_file, ".gimv-XXXXXX", NULL);
   fd = g_mkstemp_full (tmp, O_WRONLY, 0600);
   if (fd < 0) goto END;
   close (fd);

   g_snprintf (mtime, sizeof mtime, "%" G_GINT64_FORMAT, (gint64) st.st_mtime);
   g_snprintf (size, sizeof size, "%" G_GINT64_FORMAT, (gint64) st.st_size);
   if (info && info->width > 0 && info->height > 0) {
      g_snprintf (width, sizeof width, "%d", info->width);
      g_snprintf (height, sizeof height, "%d", info->height);
   } else {
      width[0] = height[0] = '\0';
   }

   if (width[0]
       ? gdk_pixbuf_save (imcache->image, tmp, "png", &error,
                          "tEXt::Thumb::URI", uri,
                          "tEXt::Thumb::MTime", mtime,
                          "tEXt::Thumb::Size", size,
                          "tEXt::Thumb::Image::Width", width,
                          "tEXt::Thumb::Image::Height", height,
                          "tEXt::Software", "GImageView",
                          NULL)
       : gdk_pixbuf_save (imcache->image, tmp, "png", &error,
                          "tEXt::Thumb::URI", uri,
                          "tEXt::Thumb::MTime", mtime,
                          "tEXt::Thumb::Size", size,
                          "tEXt::Software", "GImageView",
                          NULL))
   {
      if (g_rename (tmp, thumb_file) != 0)
         g_unlink (tmp);
   } else {
      g_message ("GImageView: cannot save %s: %s", thumb_file,
                 error ? error->message : "");
      g_clear_error (&error);
      g_unlink (tmp);
   }

END:
   if (src) gimv_image_unref (src);
   g_free (uri);
   g_free (thumb_file);
   g_free (dir);
   g_free (tmp);
   return imcache;
}
