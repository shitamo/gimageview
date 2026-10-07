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
 * $Id: gimv_thumb.c,v 1.7 2004/04/08 14:11:28 makeinu Exp $
 */

#include <string.h>

#include "gimageview.h"
#include "fileload.h"
#include "fileutil.h"
#include "fr-archive.h"
#include "gfileutil.h"
#include "gimv_icon_stock.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_image_loader.h"
#include "prefs.h"
#include "gimv_thumb.h"
#include "gimv_thumb_cache.h"
#include "gimv_thumb_view.h"


static void gimv_thumb_destroy    (GimvObject *obj);


static GimvObjectClass *parent_class = NULL;
static GHashTable *loader_table = NULL;


/* private functions */
static void       store_thumbnail                (GimvImage      *image,
                                                  gint            thumbsize,
                                                  GdkTexture     **pixmap,
                                                  GdkTexture     **mask,
                                                  gint           *width,
                                                  gint           *height);
static void       create_thumbnail               (GimvThumb      *thumb,
                                                  gint            thumbsize,
                                                  gchar          *cache_type,
                                                  gboolean        save_cache);
static void       load_thumbnail_cache           (GimvThumb      *thumb,
                                                  gint            thumbsize);


static GHashTable *cache_type_labels;
static gchar *config_cache_read_string = NULL;
static GList *cache_read_list = NULL;


G_DEFINE_TYPE (GimvThumb, gimv_thumb, GIMV_TYPE_OBJECT)


static void
gimv_thumb_class_init (GimvThumbClass *klass)
{
   GimvObjectClass *object_class;

   object_class = (GimvObjectClass *) klass;
   parent_class = gimv_thumb_parent_class;

   object_class->destroy  = gimv_thumb_destroy;
}


static void
gimv_thumb_init (GimvThumb *thumb)
{
   thumb->info           = NULL;

   thumb->thumbnail      = NULL;
   thumb->thumbnail_mask = NULL;
   thumb->icon           = NULL;
   thumb->icon_mask      = NULL;
   thumb->thumb_width    = -1;
   thumb->thumb_height   = -1;
   thumb->cache_type     = NULL;

   /* will be removed! */
   thumb->selected   = FALSE;

   /* like gtk_object_ref () + gtk_object_sink () of GTK2 */
   g_object_ref_sink (G_OBJECT (thumb));
}


static void
gimv_thumb_destroy (GimvObject *object)
{
   GimvThumb *thumb;

   g_return_if_fail (GIMV_IS_THUMB(object));

   thumb = GIMV_THUMB(object);

   /* free thumbnail */
   if (thumb->thumbnail) {
      g_object_unref (thumb->thumbnail);
      thumb->thumbnail = NULL;
   }
   if (thumb->thumbnail_mask) {
      g_object_unref (thumb->thumbnail_mask);
      thumb->thumbnail_mask = NULL;
   }
   if (thumb->icon) {
      g_object_unref (thumb->icon);
      thumb->icon = NULL;
   }
   if (thumb->icon_mask) {
      g_object_unref (thumb->icon_mask);
      thumb->icon_mask = NULL;
   }

   if (thumb->info) {
      gimv_image_info_unref (thumb->info);
      thumb->info = NULL;
   }

   if (GIMV_OBJECT_CLASS (parent_class)->destroy)
      (*GIMV_OBJECT_CLASS (parent_class)->destroy) (object);
}


/******************************************************************************
 *
 *   Private Functions.
 *
 ******************************************************************************/
static void
store_thumbnail (GimvImage *image, gint thumbsize,
                 GdkTexture **pixmap, GdkTexture **mask,
                 gint *width_ret, gint *height_ret)
{
   gfloat scale;
   gint width = 0, height = 0;

   if (!image || !thumbsize) return;
   g_return_if_fail (pixmap && mask);

   if (width_ret)  *width_ret = -1;
   if (height_ret) *height_ret = -1;

   if (gimv_image_width (image) >= gimv_image_height (image)) {
      scale = (gfloat) thumbsize / (gfloat) gimv_image_width (image);
      width = thumbsize;
      height = gimv_image_height (image) * scale;
   } else {
      scale = (gfloat) thumbsize / (gfloat) gimv_image_height (image);
      width = gimv_image_width (image) * scale;
      height = thumbsize;
   }

   if (width > 0 && height > 0) {
      gimv_image_scale_get_pixmap (image, width, height, pixmap, mask);
      if (width_ret)  *width_ret = width;
      if (height_ret) *height_ret = height;
   }
}


static void
thumbnail_apply_cache_read_prefs (void)
{
   gchar **labels;
   gint i;

   if (cache_read_list) {
      g_list_foreach (cache_read_list, (GFunc)g_free, NULL);
      g_list_free (cache_read_list);
      cache_read_list = NULL;
   }

   config_cache_read_string = conf.cache_read_list;
   if (!conf.cache_read_list) return;

   labels = g_strsplit (conf.cache_read_list, ",", -1);

   for (i = 0; labels[i]; i++) {
      cache_read_list = g_list_append (cache_read_list, g_strdup (labels[i]));
   }

   g_strfreev (labels);
}


static void
cb_loader_progress_update (GimvImageLoader *loader, GimvThumb *thumb)
{
   GimvThumbView *tv;

   gimv_flush_events ();

   /* FIXME!! */
   if (!GIMV_IS_THUMB(thumb)) return;
   tv = gimv_thumb_get_parent_thumbview (thumb);
   if (tv && tv->progress && tv->progress->status >= CANCEL)
   {
      gimv_thumb_load_stop (thumb);
   }
}


static void
create_thumbnail (GimvThumb *thumb, gint thumbsize,
                  gchar *cache_type, gboolean save_cache)
{
   GimvImageInfo *info;
   GimvImage *imdisp = NULL;
   gboolean has_cache = FALSE;
   gchar *filename;

   g_return_if_fail (GIMV_IS_THUMB (thumb));
   g_return_if_fail (thumb->info);

   info = thumb->info;

   /* get filename */
   filename = gimv_image_info_get_path_with_archive (info);

   /* load cache if cache type is specified */
   if (cache_type && !save_cache) {
      GimvImage *imcache = NULL;
      gchar *cache_path;
      struct stat cache_st;

      cache_path = gimv_thumb_cache_get_path (filename, cache_type);
      if (!cache_path) goto ERROR;
      if (stat (cache_path, &cache_st)
          || (info->st.st_mtime > cache_st.st_mtime))
      {
         g_free (cache_path);
         goto ERROR;
      }
      g_free (cache_path);

      imcache = gimv_thumb_cache_load (filename, cache_type, info);
      if (imcache) {
         has_cache = TRUE;
         imdisp = imcache;
      }

   /* load original image */
   } else {
      GimvImageLoader *loader;
      GimvImage *image = NULL, *imcache = NULL;

      if (!loader_table)
         loader_table = g_hash_table_new (g_direct_hash, g_direct_equal);

      /* load image */
      loader = gimv_image_loader_new_with_image_info (info);
      if (loader) {
         g_hash_table_insert (loader_table, thumb, loader);

         g_signal_connect (G_OBJECT (loader), "progress_update",
                             G_CALLBACK (cb_loader_progress_update),
                             thumb);
         gimv_image_loader_set_load_type (loader,
                                          GIMV_IMAGE_LOADER_LOAD_THUMBNAIL);
         gimv_image_loader_set_size_request (loader, thumbsize, thumbsize,
                                             TRUE);
         gimv_image_loader_set_as_animation (loader, FALSE);
         gimv_image_loader_load (loader);
         image = gimv_image_loader_get_image (loader);
         if (image)
            gimv_image_ref (image);
         gimv_image_loader_unref (loader);

         g_hash_table_remove (loader_table, thumb);
      }

      /* write cache to disk */
      if (image && save_cache && cache_type
          && (!gimv_image_info_is_in_archive (info)
              || !strcmp (cache_type, "GImageView")))
      {
         imcache = gimv_thumb_cache_save (filename, cache_type,
                                          image, info);
      }

      if (imcache) {
         has_cache = TRUE;
         imdisp = imcache;
         gimv_image_unref (image);
         image = NULL;
      } else {
         imdisp = image;
      }
   }

   /* get GdkPixbuf from GimvImage */
   if (imdisp) {
      /* resize to display thumbnail size */
      store_thumbnail (imdisp, thumbsize,
                       &thumb->thumbnail,
                       &thumb->thumbnail_mask,
                       &thumb->thumb_width,
                       &thumb->thumb_height);
      store_thumbnail (imdisp, ICON_SIZE,
                       &thumb->icon,
                       &thumb->icon_mask,
                       NULL,
                       NULL);

      gimv_image_unref  (imdisp);
      imdisp = NULL;
   }

   /* store cache type label */
   if (thumb->thumbnail && has_cache && cache_type) {
      gchar *label;

      if (!cache_type_labels)
         cache_type_labels = g_hash_table_new (g_str_hash, g_str_equal);
      label = g_hash_table_lookup (cache_type_labels, cache_type);
      if (!label) {
         label = g_strdup (cache_type);
         g_hash_table_insert (cache_type_labels, label, label);
      }
      thumb->cache_type = label;
   }

ERROR:
   g_free (filename);
}


static void
load_thumbnail_cache (GimvThumb *thumb, gint thumbsize)
{
   GList *node;

   g_return_if_fail (GIMV_IS_THUMB (thumb));

   if (!cache_read_list || config_cache_read_string != conf.cache_read_list)
      thumbnail_apply_cache_read_prefs ();

   for (node = cache_read_list; node; node = g_list_next (node)) {
      gchar *cache_type = node->data;
      create_thumbnail (thumb, thumbsize, cache_type, FALSE);
      if (thumb->thumbnail) return;
   }
}


/******************************************************************************
 *
 *   Public Functions.
 *
 ******************************************************************************/
GimvThumb *
gimv_thumb_new (GimvImageInfo *info)
{
   GimvThumb *thumb
      = GIMV_THUMB (g_object_new (GIMV_TYPE_THUMB, NULL));

   thumb->info = gimv_image_info_ref (info);

   return thumb;
}


gboolean
gimv_thumb_load (GimvThumb    *thumb,
                gint          thumb_size,
                ThumbLoadType type)
{
   /* free old thumbnail */
   if (thumb->thumbnail) {
      g_object_unref (thumb->thumbnail);
      thumb->thumbnail = NULL;
   }
   if (thumb->thumbnail_mask) {
      g_object_unref (thumb->thumbnail_mask);
      thumb->thumbnail_mask = NULL;
   }
   if (thumb->icon) {
      g_object_unref (thumb->icon);
      thumb->icon = NULL;
   }
   if (thumb->icon_mask) {
      g_object_unref (thumb->icon_mask);
      thumb->icon_mask = NULL;
   }

   /* if the file is directory */
   if (gimv_image_info_is_dir (thumb->info))
   {
      GimvIcon *icon;

      icon = gimv_icon_stock_get_icon ("folder48");
      thumb->thumbnail      = icon->pixmap ? g_object_ref (icon->pixmap) : NULL;
      thumb->thumbnail_mask = icon->mask ? g_object_ref (icon->mask) : NULL;

      icon = gimv_icon_stock_get_icon ("folder");
      thumb->icon      = icon->pixmap ? g_object_ref (icon->pixmap) : NULL;
      thumb->icon_mask = icon->mask ? g_object_ref (icon->mask) : NULL;
   }

   /* if the file is archive */
   if (!thumb->thumbnail /* && conf.detect_filetype_by_ext */
       &&  gimv_image_info_is_archive (thumb->info))
   {
      GimvIcon *icon;

      icon = gimv_icon_stock_get_icon ("archive");
      thumb->thumbnail      = icon->pixmap ? g_object_ref (icon->pixmap) : NULL;
      thumb->thumbnail_mask = icon->mask ? g_object_ref (icon->mask) : NULL;

      icon = gimv_icon_stock_get_icon ("small_archive");
      thumb->icon      = icon->pixmap ? g_object_ref (icon->pixmap) : NULL;
      thumb->icon_mask = icon->mask ? g_object_ref (icon->mask) : NULL;
   }

   /* load chache  */
   if (!thumb->thumbnail && type != CREATE_THUMB) {
      load_thumbnail_cache (thumb, thumb_size);
   }

#if 0
   if (!thumb->thumbnail  /* && conf.detect_filetype_by_ext */
       && gimv_image_info_is_movie (thumb->info))
   {
      return FALSE;
   }
#endif
   if (gimv_image_info_is_audio (thumb->info))
      return FALSE;

   /* create cache if not exist */
   if (!thumb->thumbnail
       && conf.cache_write_type && conf.cache_write_type[0] != '\0')
   {
      create_thumbnail (thumb, thumb_size,
                        conf.cache_write_type, TRUE);
   }

   /* create thumbnail from original image */
   if (!thumb->thumbnail) {
      create_thumbnail (thumb, thumb_size, NULL, FALSE);
   }

   if (!thumb->thumbnail)
      return FALSE;
   else
      return TRUE;
}


gboolean
gimv_thumb_is_loading (GimvThumb *thumb)
{
   if (!loader_table) return FALSE;

   if (g_hash_table_lookup (loader_table, thumb))
      return TRUE;

   return FALSE;
}


void
gimv_thumb_load_stop (GimvThumb *thumb)
{
   GimvImageLoader *loader;

   if (!loader_table) return;

   loader = g_hash_table_lookup (loader_table, thumb);

   if (loader)
      gimv_image_loader_load_stop (loader);
}


void
gimv_thumb_get_thumb (GimvThumb *thumb, GdkTexture **pixmap, GdkTexture **mask)
{
   g_return_if_fail (GIMV_IS_THUMB (thumb));
   g_return_if_fail (pixmap && mask);

   *pixmap = thumb->thumbnail;
   *mask   = thumb->thumbnail_mask;
}


GtkWidget *
gimv_thumb_get_thumb_by_widget (GimvThumb *thumb)
{
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), NULL);
   if (!thumb->thumbnail) return NULL;

   return gtk_image_new_from_paintable (GDK_PAINTABLE (thumb->thumbnail));
}


void
gimv_thumb_get_icon (GimvThumb *thumb, GdkTexture **pixmap, GdkTexture **mask)
{
   g_return_if_fail (GIMV_IS_THUMB (thumb));
   g_return_if_fail (pixmap && mask);

   *pixmap = thumb->icon;
   *mask   = thumb->icon_mask;
}


GtkWidget *
gimv_thumb_get_icon_by_widget (GimvThumb *thumb)
{
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), NULL);
   if (!thumb->icon) return NULL;

   return gtk_image_new_from_paintable (GDK_PAINTABLE (thumb->icon));
}


const gchar *
gimv_thumb_get_cache_type (GimvThumb *thumb)
{
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), NULL);

   return thumb->cache_type;
}


gboolean
gimv_thumb_has_thumbnail (GimvThumb *thumb)
{
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), FALSE);

   if (isdir (gimv_image_info_get_path (thumb->info)))
      return FALSE;

   if (fr_archive_utils_get_file_name_ext (gimv_image_info_get_path (thumb->info)))
      return FALSE;

   if (thumb->thumbnail)
      return TRUE;
   else
      return FALSE;
}


gchar *
gimv_thumb_find_thumbcache (const gchar *filename, gchar **type)
{
   gchar *thumb_file = NULL;
   gchar *cache_type;
   GList *node;

   if (!cache_read_list || config_cache_read_string != conf.cache_read_list)
      thumbnail_apply_cache_read_prefs ();

   for (node = cache_read_list; node; node = g_list_next (node)) {
      cache_type = node->data;
      thumb_file = gimv_thumb_cache_get_path (filename, cache_type);
      if (file_exists (thumb_file)) {
         if (type)
            *type = cache_type;
         return thumb_file;
      }
      g_free (thumb_file);
      thumb_file = NULL;
   }

   if (type)
      *type = NULL;

   return NULL;
}
