/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * Thumbnails made in worker threads (see gimv_thumb_async.h).
 *
 * Before, every thumbnail was made in the main thread, one after the
 * other, between the events: an image read in full (a big photo from a
 * slow disk, a format read by glycin in a sandbox process) held gimv for
 * that time, and one core did all.  Now the reading and scaling is done
 * by a few threads; the main thread only puts the result in place and
 * writes the cache, as before.
 *
 * The threads use only what is safe there: libjpeg and libexif on their
 * own files, GdkPixbuf.  The plugins of gimv (GimvImageLoader) stay in the
 * main thread.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gimv_thumb_async.h"

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef ENABLE_JPEG
#  include <jpeglib.h>
#endif
#ifdef ENABLE_EXIF
#  include <libexif/exif-data.h>
#endif

#include "gimv_image.h"
#include "gimv_image_info.h"
#include "prefs.h"

#define PENDING_KEY  "gimv-thumb-async"
#define MAX_THREADS  4

typedef struct {
   GimvThumb          *thumb;      /* held */
   gchar              *path;
   gchar              *ext;        /* lower case */
   gint                size;
   ThumbLoadType       type;
   gboolean            use_exif;
   GimvThumbAsyncFunc  func;
   GWeakRef            view;
   gint                cancel;     /* atomic */

   /* made in the thread */
   GdkPixbuf          *pixbuf;
   gint                orig_w, orig_h;
} Job;

static GThreadPool *pool = NULL;
static GHashTable  *pixbuf_exts = NULL;   /* the extensions GdkPixbuf reads */
static guint        n_pending = 0;


static void
job_free (Job *job)
{
   g_object_unref (job->thumb);
   g_weak_ref_clear (&job->view);
   g_clear_object (&job->pixbuf);
   g_free (job->path);
   g_free (job->ext);
   g_free (job);
}


/******************************************************************************
 *  JPEG (libjpeg): at 1/2 .. 1/8, or the thumbnail embedded (EXIF)
 ******************************************************************************/
#ifdef ENABLE_JPEG

typedef struct {
   struct jpeg_error_mgr pub;
   jmp_buf               jump;
} JpegError;


static void
jpeg_error_exit (j_common_ptr cinfo)
{
   JpegError *err = (JpegError *) cinfo->err;
   longjmp (err->jump, 1);
}


static void
jpeg_quiet (j_common_ptr cinfo, int level)
{
}


/* as the JPEG loader of gimv (jpeg_loader.c) */
static gint
jpeg_divisor (gint w, gint h, gint want)
{
   if (w / 8 > want && h / 8 > want) return 8;
   if (w / 4 > want && h / 4 > want) return 4;
   if (w / 2 > want && h / 2 > want) return 2;
   return 1;
}


/* from a file (data NULL) or from memory; want > 0: scaled down by the
 * libjpeg to no smaller than it; header_only: the size alone */
static GdkPixbuf *
jpeg_read (FILE *file, const guchar *data, gsize len, gint want, gboolean header_only,
           gint *w_ret, gint *h_ret)
{
   struct jpeg_decompress_struct cinfo;
   JpegError err;
   GdkPixbuf *volatile pixbuf = NULL;
   guchar *pixels;
   gint stride;

   cinfo.err = jpeg_std_error (&err.pub);
   err.pub.error_exit = jpeg_error_exit;
   err.pub.emit_message = jpeg_quiet;
   if (setjmp (err.jump)) {
      jpeg_destroy_decompress (&cinfo);
      if (pixbuf) g_object_unref (pixbuf);
      return NULL;
   }
   jpeg_create_decompress (&cinfo);
   if (file)
      jpeg_stdio_src (&cinfo, file);
   else
      jpeg_mem_src (&cinfo, (unsigned char *) data, len);
   jpeg_read_header (&cinfo, TRUE);
   if (w_ret) *w_ret = cinfo.image_width;
   if (h_ret) *h_ret = cinfo.image_height;
   /* CMYK (Adobe) and the like: the usual way */
   if (header_only || (cinfo.jpeg_color_space != JCS_YCbCr
                       && cinfo.jpeg_color_space != JCS_GRAYSCALE
                       && cinfo.jpeg_color_space != JCS_RGB))
   {
      jpeg_destroy_decompress (&cinfo);
      return NULL;
   }
   cinfo.out_color_space = JCS_RGB;
   cinfo.scale_num = 1;
   cinfo.scale_denom = want > 0 ? jpeg_divisor (cinfo.image_width, cinfo.image_height, want) : 1;
   cinfo.dct_method = JDCT_IFAST;
   cinfo.do_fancy_upsampling = FALSE;
   jpeg_start_decompress (&cinfo);
   if (cinfo.output_components != 3) {
      jpeg_destroy_decompress (&cinfo);
      return NULL;
   }
   pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8,
                            cinfo.output_width, cinfo.output_height);
   if (!pixbuf) {
      jpeg_destroy_decompress (&cinfo);
      return NULL;
   }
   pixels = gdk_pixbuf_get_pixels (pixbuf);
   stride = gdk_pixbuf_get_rowstride (pixbuf);
   while (cinfo.output_scanline < cinfo.output_height) {
      JSAMPROW row = pixels + (gsize) cinfo.output_scanline * stride;
      jpeg_read_scanlines (&cinfo, &row, 1);
   }
   jpeg_finish_decompress (&cinfo);
   jpeg_destroy_decompress (&cinfo);
   return pixbuf;
}


#ifdef ENABLE_EXIF
/* the thumbnail of the EXIF data, if no smaller than "want" (its black
 * bands, a 4:3 thumbnail of a 3:2 photo, cut off) */
static GdkPixbuf *
exif_thumbnail (const gchar *path, gint want, gint orig_w, gint orig_h)
{
   ExifData *ed = exif_data_new_from_file (path);
   GdkPixbuf *pixbuf = NULL;
   gint w = 0, h = 0;

   if (!ed) return NULL;
   if (ed->data && ed->size > 0)
      pixbuf = jpeg_read (NULL, ed->data, ed->size, 0, FALSE, &w, &h);
   exif_data_unref (ed);
   if (!pixbuf) return NULL;
   w = gdk_pixbuf_get_width (pixbuf);
   h = gdk_pixbuf_get_height (pixbuf);
   if (MAX (w, h) < want || orig_w <= 0 || orig_h <= 0
       /* turned another way than the image: not trusted */
       || (w > h) != (orig_w > orig_h))
   {
      g_object_unref (pixbuf);
      return NULL;
   }
   {
      gdouble a = (gdouble) orig_w / orig_h, b = (gdouble) w / h;
      if (b > a * 1.02 || b < a / 1.02) {
         gint cw = w, ch = h;
         GdkPixbuf *cut;
         if (b > a) cw = (gint) (h * a + 0.5);
         else       ch = (gint) (w / a + 0.5);
         if (MAX (cw, ch) < want || cw < 1 || ch < 1) {
            g_object_unref (pixbuf);
            return NULL;
         }
         cut = gdk_pixbuf_new_subpixbuf (pixbuf, (w - cw) / 2, (h - ch) / 2, cw, ch);
         g_object_unref (pixbuf);
         pixbuf = gdk_pixbuf_copy (cut);
         g_object_unref (cut);
      }
   }
   return pixbuf;
}
#endif /* ENABLE_EXIF */


static void
make_jpeg (Job *job)
{
   FILE *file = fopen (job->path, "rb");

   if (!file) return;
   /* the size first (the header only), for the EXIF thumbnail */
   jpeg_read (file, NULL, 0, 0, TRUE, &job->orig_w, &job->orig_h);
#ifdef ENABLE_EXIF
   if (job->use_exif && !g_atomic_int_get (&job->cancel))
      job->pixbuf = exif_thumbnail (job->path, job->size, job->orig_w, job->orig_h);
#endif
   if (!job->pixbuf && !g_atomic_int_get (&job->cancel)) {
      rewind (file);
      job->pixbuf = jpeg_read (file, NULL, 0, job->size, FALSE, NULL, NULL);
   }
   fclose (file);
}

#endif /* ENABLE_JPEG */


/******************************************************************************
 *  the threads
 ******************************************************************************/

static gboolean
is_jpeg (const gchar *ext)
{
   return ext && (!strcmp (ext, "jpg") || !strcmp (ext, "jpeg") || !strcmp (ext, "jpe")
                  || !strcmp (ext, "jfif"));
}


static gboolean
idle_done (gpointer data)
{
   Job *job = data;
   GObject *view = g_weak_ref_get (&job->view);
   GimvThumb *thumb = job->thumb;
   gboolean made = FALSE;

   n_pending--;
   g_object_set_data (G_OBJECT (thumb), PENDING_KEY, NULL);

   if (!g_atomic_int_get (&job->cancel) && thumb->info
       && !GIMV_OBJECT (thumb)->destroyed && job->pixbuf)
   {
      GimvImage *image = gimv_image_new ();
      image->image = g_object_ref (job->pixbuf);
      made = gimv_thumb_set_image (thumb, job->size, image, job->orig_w, job->orig_h);
      gimv_image_unref (image);
   }
   if (view) {
      job->func (thumb, made, g_atomic_int_get (&job->cancel) != 0, view);
      g_object_unref (view);
   }
   job_free (job);
   return G_SOURCE_REMOVE;
}


static void
thread_make (gpointer data, gpointer user_data)
{
   Job *job = data;

   if (!g_atomic_int_get (&job->cancel)) {
#ifdef ENABLE_JPEG
      if (is_jpeg (job->ext))
         make_jpeg (job);
#endif
      /* the other formats of GdkPixbuf (and a JPEG libjpeg did not read) */
      if (!job->pixbuf && !g_atomic_int_get (&job->cancel)) {
         if (job->orig_w <= 0)
            gdk_pixbuf_get_file_info (job->path, &job->orig_w, &job->orig_h);
         job->pixbuf = gdk_pixbuf_new_from_file_at_scale (job->path, job->size, job->size,
                                                          TRUE, NULL);
      }
      /* turned as it is meant to be seen (EXIF): that of gdk-pixbuf, or
       * read here for the JPEGs read by libjpeg */
      if (job->pixbuf && !g_atomic_int_get (&job->cancel)) {
         GdkPixbuf *turned = gimv_thumb_pixbuf_orient (
            job->pixbuf, gdk_pixbuf_get_option (job->pixbuf, "orientation")
                         ? 1 : gimv_thumb_file_orientation (job->path));
         g_object_unref (job->pixbuf);
         job->pixbuf = turned;
      }
   }
   g_idle_add (idle_done, job);
}


static void
init (void)
{
   GSList *formats, *node;
   gint threads;

   if (pool) return;
   threads = CLAMP ((gint) g_get_num_processors () - 1, 2, MAX_THREADS);
   pool = g_thread_pool_new (thread_make, NULL, threads, FALSE, NULL);

   pixbuf_exts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   formats = gdk_pixbuf_get_formats ();
   for (node = formats; node; node = g_slist_next (node)) {
      gchar **exts = gdk_pixbuf_format_get_extensions (node->data);
      gint i;
      for (i = 0; exts && exts[i]; i++)
         g_hash_table_add (pixbuf_exts, g_ascii_strdown (exts[i], -1));
      g_strfreev (exts);
   }
   g_slist_free (formats);
}


static gchar *
lower_ext (const gchar *path)
{
   const gchar *dot = strrchr (path, '.'), *slash = strrchr (path, '/');
   if (!dot || (slash && dot < slash)) return NULL;
   return g_ascii_strdown (dot + 1, -1);
}


/******************************************************************************
 *  public
 ******************************************************************************/

gboolean
gimv_thumb_async_can (GimvThumb *thumb)
{
   GimvImageInfo *info;
   const gchar *path;
   gchar *ext;
   gboolean ok;

   g_return_val_if_fail (GIMV_IS_THUMB (thumb), FALSE);
   info = thumb->info;
   if (!info || gimv_image_info_is_dir (info) || gimv_image_info_is_archive (info)
       || gimv_image_info_is_in_archive (info) || gimv_image_info_is_movie (info)
       || gimv_image_info_is_audio (info))
      return FALSE;
   path = gimv_image_info_get_path (info);
   if (!path || path[0] != '/') return FALSE;

   init ();
   ext = lower_ext (path);
   ok = ext && (is_jpeg (ext) || g_hash_table_contains (pixbuf_exts, ext));
   g_free (ext);
   return ok;
}


gboolean
gimv_thumb_async_load (GimvThumb *thumb, gint thumb_size, ThumbLoadType type,
                       GimvThumbAsyncFunc func, gpointer view)
{
   Job *job;

   g_return_val_if_fail (GIMV_IS_THUMB (thumb), FALSE);
   job = g_object_get_data (G_OBJECT (thumb), PENDING_KEY);
   if (job) {
      /* waiting, perhaps cancelled: wanted again */
      g_atomic_int_set (&job->cancel, 0);
      return TRUE;
   }
   if (!gimv_thumb_async_can (thumb)) return FALSE;

   job = g_new0 (Job, 1);
   job->thumb = g_object_ref (thumb);
   job->path = g_strdup (gimv_image_info_get_path (thumb->info));
   job->ext = lower_ext (job->path);
   job->size = thumb_size;
   job->type = type;
   job->use_exif = conf.thumbnail_use_exif;
   job->func = func;
   g_weak_ref_init (&job->view, view);
   g_object_set_data (G_OBJECT (thumb), PENDING_KEY, job);
   n_pending++;
   g_thread_pool_push (pool, job, NULL);
   return TRUE;
}


gboolean
gimv_thumb_async_pending (GimvThumb *thumb)
{
   return thumb && g_object_get_data (G_OBJECT (thumb), PENDING_KEY) != NULL;
}


void
gimv_thumb_async_cancel (GimvThumb *thumb)
{
   Job *job = thumb ? g_object_get_data (G_OBJECT (thumb), PENDING_KEY) : NULL;
   if (job) g_atomic_int_set (&job->cancel, 1);
}


guint
gimv_thumb_async_n_pending (void)
{
   return n_pending;
}
