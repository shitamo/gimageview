/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: saving the selected images smaller (to send them, to make
 * them fit the screen), as new files in a folder next to them.
 *
 * Each image is read at the size it is wanted at (a JPEG is decoded
 * scaled down by libjpeg), turned as its EXIF says, and written in a
 * thread, one after another; the window shows how far it is and can stop
 * it.  An image not larger than the size is copied as it is.  The
 * originals are never changed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_resize_dialog.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>

#include "gimageview.h"
#include "gimv_comment.h"
#include "gimv_image_info.h"
#include "gimv_save_dialog.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "prefs.h"
#ifdef ENABLE_INDEX
#  include "gimv_index.h"
#endif
#ifdef ENABLE_GEXIV2
#  include <gexiv2/gexiv2.h>
#endif


typedef enum {
   SIZE_HEIGHT,
   SIZE_WIDTH,
   SIZE_LONG_SIDE
} SizeMode;


typedef struct {
   GtkWidget     *window;
   GtkWindow     *parent;
   GPtrArray     *paths;        /* gchar *: the images */

   GtkWidget     *mode_drop;
   GtkWidget     *size_spin;
   GtkWidget     *quality_scale;
   GtkWidget     *dir_entry;
   GtkWidget     *metadata_check;
   GtkWidget     *gps_check;
   GtkWidget     *comment_check;
   GtkWidget     *estimate;
   GtkWidget     *progress;
   GtkWidget     *status;
   GtkWidget     *save_button;
   GtkWidget     *close_button;
   GtkWidget     *open_button;

   guint          estimate_id;  /* the timeout before the estimate */
   GCancellable  *estimate_cancel;

   /* the saving */
   GCancellable  *cancel;
   gboolean       running;
   gboolean       closed;
   guint          next;         /* the index of the next image */
   guint          made, copied, failed;
   gchar         *first_saved;  /* to open its folder */
   GString       *errors;
} Dialog;


/* what to do with one image (copied: the thread has its own) */
typedef struct {
   gchar     *src;
   gchar     *dest;         /* NULL: the estimate (to memory) */
   SizeMode   mode;
   gint       size;
   gint       quality;
   gboolean   metadata;
   gboolean   no_gps;
   gboolean   comment;

   /* the result */
   gboolean   resized;      /* FALSE: copied as it is */
   gint       src_w, src_h, w, h;
   goffset    src_bytes, bytes;
   gchar     *error;
   gboolean   stopped;      /* given up at the stop: nothing written */
} Job;


static void
job_free (gpointer p)
{
   Job *job = p;
   g_free (job->src);
   g_free (job->dest);
   g_free (job->error);
   g_free (job);
}


static void
dialog_free (Dialog *d)
{
   if (d->estimate_id) g_source_remove (d->estimate_id);
   if (d->estimate_cancel) {
      g_cancellable_cancel (d->estimate_cancel);
      g_object_unref (d->estimate_cancel);
   }
   if (d->cancel) g_object_unref (d->cancel);
   g_ptr_array_unref (d->paths);
   g_free (d->first_saved);
   if (d->errors) g_string_free (d->errors, TRUE);
   g_free (d);
}


/******************************************************************************
 *   the work (in a thread)
 ******************************************************************************/

/* the format gdk-pixbuf writes it in: as it was if it can, else JPEG */
static const gchar *
writer_of (const gchar *name, const gchar **ext)
{
   static const struct { const gchar *name, *ext; } keep[] = {
      { "jpeg", NULL }, { "png", NULL }, { "webp", NULL }, { "tiff", NULL },
   };
   GSList *list, *node;
   const gchar *ret = NULL;
   guint i;

   *ext = NULL;
   for (i = 0; name && i < G_N_ELEMENTS (keep) && !ret; i++) {
      if (strcmp (name, keep[i].name)) continue;
      list = gdk_pixbuf_get_formats ();
      for (node = list; node; node = g_slist_next (node)) {
         gchar *n = gdk_pixbuf_format_get_name (node->data);
         if (!strcmp (n, name) && gdk_pixbuf_format_is_writable (node->data))
            ret = keep[i].name;
         g_free (n);
      }
      g_slist_free (list);
   }
   if (!ret) {
      *ext = "jpg";      /* written as JPEG: the extension changes */
      ret = "jpeg";
   }
   return ret;
}


/* the scale for an image shown w x h */
static gdouble
scale_for (SizeMode mode, gint size, gint w, gint h)
{
   gint side;

   switch (mode) {
   case SIZE_WIDTH:  side = w;           break;
   case SIZE_HEIGHT: side = h;           break;
   default:          side = MAX (w, h);  break;
   }
   if (side <= size || side <= 0) return 1.0;   /* never larger */
   return (gdouble) size / side;
}


typedef struct {
   gint    width, height;   /* as stored */
   gdouble scale;
} SizeData;

static void
cb_size_prepared (GdkPixbufLoader *loader, gint width, gint height, gpointer p)
{
   SizeData *s = p;

   s->width = width;
   s->height = height;
   if (s->scale < 1.0)
      gdk_pixbuf_loader_set_size (loader,
                                  MAX (1, (gint) (width * s->scale + 0.5)),
                                  MAX (1, (gint) (height * s->scale + 0.5)));
}


#ifdef ENABLE_GEXIV2
static void
exiv2_init (void)
{
   static gsize init = 0;
   if (g_once_init_enter (&init)) {
      gexiv2_initialize ();
      g_once_init_leave (&init, 1);
   }
}
#endif


/* the orientation of the EXIF turns it by 90 degrees */
static gboolean
turned_sideways (const gchar *path, const gchar *format)
{
   GdkPixbuf *probe;
   const gchar *o;
   gboolean ret = FALSE;

#ifdef ENABLE_GEXIV2
   {
      GExiv2Metadata *m;
      exiv2_init ();
      m = gexiv2_metadata_new ();
      if (gexiv2_metadata_open_path (m, path, NULL)) {
         GExiv2Orientation v;
         G_GNUC_BEGIN_IGNORE_DEPRECATIONS
         v = gexiv2_metadata_try_get_orientation (m, NULL);
         G_GNUC_END_IGNORE_DEPRECATIONS
         ret = (v >= GEXIV2_ORIENTATION_ROT_90_HFLIP && v <= GEXIV2_ORIENTATION_ROT_270);
      }
      g_object_unref (m);
      return ret;
   }
#endif

   /* gdk-pixbuf gives the orientation of a JPEG (or TIFF) in the options
      of the image it reads; the smallest size is enough to see it */
   if (g_strcmp0 (format, "jpeg") && g_strcmp0 (format, "tiff")) return FALSE;

   /* gdk-pixbuf puts the orientation in the options of the image it reads;
      the smallest size is enough to see it */
   probe = gdk_pixbuf_new_from_file_at_size (path, 8, 8, NULL);
   if (!probe) return FALSE;
   o = gdk_pixbuf_get_option (probe, "orientation");
   if (o) {
      gint v = atoi (o);
      ret = (v >= 5 && v <= 8);
   }
   g_object_unref (probe);
   return ret;
}


static gboolean
resize_copy_file (const gchar *src, const gchar *dest, GError **error)
{
   GFile *s = g_file_new_for_path (src), *d = g_file_new_for_path (dest);
   gboolean ok = g_file_copy (s, d, G_FILE_COPY_NOFOLLOW_SYMLINKS
                                    | G_FILE_COPY_TARGET_DEFAULT_PERMS,
                              NULL, NULL, NULL, error);
   g_object_unref (s);
   g_object_unref (d);
   return ok;
}


/* the comment (with the tags given by hand) of src for dest too */
static void
copy_comment (const gchar *src, const gchar *dest)
{
   gchar *from = gimv_comment_find_file (src);
   gchar *to, *dir;
   GError *error = NULL;

   if (!from) return;
   to = gimv_comment_get_path (dest);
   dir = to ? g_path_get_dirname (to) : NULL;
   if (to && g_mkdir_with_parents (dir, 0700) == 0 && resize_copy_file (from, to, &error)) {
#ifdef ENABLE_INDEX
      gimv_index_tags_changed (dest);
#endif
   }
   g_clear_error (&error);
   g_free (dir);
   g_free (to);
   g_free (from);
}


/* a file copied as it is, without the place it was taken at */
static void
strip_gps (const gchar *path)
{
#ifdef ENABLE_GEXIV2
   GExiv2Metadata *m;

   exiv2_init ();
   m = gexiv2_metadata_new ();
   if (gexiv2_metadata_open_path (m, path, NULL)) {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gdouble lon, lat, alt;
      if (gexiv2_metadata_try_get_gps_info (m, &lon, &lat, &alt, NULL)
          || gexiv2_metadata_try_has_tag (m, "Xmp.exif.GPSLatitude", NULL))
      {
         gexiv2_metadata_try_delete_gps_info (m, NULL);
         gexiv2_metadata_try_clear_tag (m, "Xmp.exif.GPSLatitude", NULL);
         gexiv2_metadata_try_clear_tag (m, "Xmp.exif.GPSLongitude", NULL);
         gexiv2_metadata_try_clear_tag (m, "Xmp.exif.GPSAltitude", NULL);
         gexiv2_metadata_save_file (m, path, NULL);
      }
      G_GNUC_END_IGNORE_DEPRECATIONS
   }
   g_object_unref (m);
#endif
}


static void
resize_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   Job *job = data;
   GdkPixbufFormat *format;
   GdkPixbufLoader *loader = NULL;
   GdkPixbuf *pixbuf = NULL, *turned = NULL;
   SizeData sd = { 0, 0, 1.0 };
   gchar *contents = NULL, *name = NULL;
   gsize length = 0;
   gint w, h;
   const gchar *writer, *ext;
   GError *error = NULL;
   gboolean sideways;

   format = gdk_pixbuf_get_file_info (job->src, &w, &h);
   if (!format) {
      job->error = g_strdup (_("not an image gdk-pixbuf can read"));
      goto out;
   }
   name = gdk_pixbuf_format_get_name (format);
   sideways = turned_sideways (job->src, name);
   job->src_w = sideways ? h : w;
   job->src_h = sideways ? w : h;
   {
      GStatBuf st;
      if (g_stat (job->src, &st) == 0) job->src_bytes = st.st_size;
   }

   sd.scale = scale_for (job->mode, job->size, job->src_w, job->src_h);
   writer = writer_of (name, &ext);

   /* not larger than the size, and in a format kept: copied as it is */
   if (sd.scale >= 1.0 && !ext) {
      job->w = job->src_w;
      job->h = job->src_h;
      job->bytes = job->src_bytes;
      if (job->dest && !resize_copy_file (job->src, job->dest, &error)) {
         job->error = g_strdup (error->message);
         goto out;
      }
      if (job->dest && job->metadata && job->no_gps) {
         GStatBuf st;
         strip_gps (job->dest);
         if (g_stat (job->dest, &st) == 0) job->bytes = st.st_size;
      }
      goto out;
   }

   if (!g_file_get_contents (job->src, &contents, &length, &error)) {
      job->error = g_strdup (error->message);
      goto out;
   }
   if (g_cancellable_is_cancelled (cancel)) { job->stopped = TRUE; goto out; }

   loader = gdk_pixbuf_loader_new ();
   g_signal_connect (loader, "size-prepared", G_CALLBACK (cb_size_prepared), &sd);
   if (!gdk_pixbuf_loader_write (loader, (const guchar *) contents, length, &error)
       || !gdk_pixbuf_loader_close (loader, &error))
   {
      job->error = g_strdup (error ? error->message : _("could not be read"));
      goto out;
   }
   pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
   if (!pixbuf) {
      job->error = g_strdup (_("could not be read"));
      goto out;
   }
   turned = gdk_pixbuf_apply_embedded_orientation (pixbuf);
   job->w = gdk_pixbuf_get_width (turned);
   job->h = gdk_pixbuf_get_height (turned);
   job->resized = TRUE;

   if (g_cancellable_is_cancelled (cancel)) { job->stopped = TRUE; goto out; }

   {
      gchar q[8];
      gchar *buffer = NULL;
      gsize size = 0;
      gboolean ok;

      g_snprintf (q, sizeof q, "%d", job->quality);
      if (!strcmp (writer, "jpeg") || !strcmp (writer, "webp"))
         ok = gdk_pixbuf_save_to_buffer (turned, &buffer, &size, writer, &error,
                                         "quality", q, NULL);
      else
         ok = gdk_pixbuf_save_to_buffer (turned, &buffer, &size, writer, &error,
                                         NULL);
      if (!ok) {
         job->error = g_strdup (error ? error->message : _("could not be written"));
         goto out;
      }
      job->bytes = size;
      if (job->dest && !g_file_set_contents (job->dest, buffer, size, &error)) {
         job->error = g_strdup (error->message);
         g_free (buffer);
         goto out;
      }
      g_free (buffer);
   }

#ifdef ENABLE_GEXIV2
   if (job->dest && job->metadata) {
      gchar *e = NULL;
      if (!gimv_save_copy_metadata (job->src, job->dest, job->w, job->h,
                                    job->no_gps, &e))
         g_free (e);   /* saved without it: not an error of the image */
      else {
         GStatBuf st;
         if (g_stat (job->dest, &st) == 0) job->bytes = st.st_size;
      }
   }
#endif

out:
   g_clear_error (&error);
   if (turned) g_object_unref (turned);
   if (loader) g_object_unref (loader);
   g_free (contents);
   g_free (name);
   g_task_return_boolean (task, TRUE);
}


/******************************************************************************
 *   the window
 ******************************************************************************/

static void
set_status (Dialog *d, const gchar *text, gboolean error)
{
   gtk_label_set_text (GTK_LABEL (d->status), text ? text : "");
   if (error) gtk_widget_add_css_class (d->status, "error");
   else gtk_widget_remove_css_class (d->status, "error");
}


static Job *
job_from_dialog (Dialog *d, const gchar *src)
{
   Job *job = g_new0 (Job, 1);

   job->src     = g_strdup (src);
   job->mode    = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->mode_drop));
   job->size    = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->size_spin));
   job->quality = (gint) gtk_range_get_value (GTK_RANGE (d->quality_scale));
   job->metadata = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->metadata_check));
   job->no_gps   = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->gps_check));
   job->comment  = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->comment_check));
   return job;
}


static void
remember (Dialog *d)
{
   const gchar *dir = gtk_editable_get_text (GTK_EDITABLE (d->dir_entry));

   conf.resize_mode     = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->mode_drop));
   conf.resize_size     = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (d->size_spin));
   conf.resize_quality  = (gint) gtk_range_get_value (GTK_RANGE (d->quality_scale));
   conf.resize_metadata = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->metadata_check));
   conf.resize_no_gps   = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->gps_check));
   conf.resize_comment  = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->comment_check));
   if (dir && *dir) {
      g_free (conf.resize_dir);
      conf.resize_dir = g_strdup (dir);
   }
}


/* "1.2 MB" */
static gchar *
size_text (goffset bytes)
{
   return g_format_size_full (bytes, G_FORMAT_SIZE_DEFAULT);
}


/*** the estimate: the first image written to memory ***/

typedef struct {
   Dialog       *d;
   GCancellable *cancel;
} EstimateData;

static void
estimate_done (GObject *source, GAsyncResult *res, gpointer data)
{
   EstimateData *e = data;
   Job *job = g_task_get_task_data (G_TASK (res));
   Dialog *d = e->d;

   if (!g_cancellable_is_cancelled (e->cancel)) {
      gchar *text;
      if (job->error) {
         text = g_strdup_printf (_("The first image: %s"), job->error);
      } else {
         gchar *a = size_text (job->src_bytes), *b = size_text (job->bytes);
         if (job->resized)
            text = g_strdup_printf (_("The first image: %d × %d, %s → %d × %d, about %s"),
                                    job->src_w, job->src_h, a, job->w, job->h, b);
         else
            text = g_strdup_printf (_("The first image: %d × %d, %s (not larger: copied as it is)"),
                                    job->src_w, job->src_h, a);
         g_free (a);
         g_free (b);
      }
      gtk_label_set_text (GTK_LABEL (d->estimate), text);
      g_free (text);
   }
   g_object_unref (e->cancel);
   g_free (e);
}


static gboolean
timeout_estimate (gpointer data)
{
   Dialog *d = data;
   EstimateData *e;
   GTask *task;

   d->estimate_id = 0;
   if (d->estimate_cancel) {
      g_cancellable_cancel (d->estimate_cancel);
      g_object_unref (d->estimate_cancel);
   }
   d->estimate_cancel = g_cancellable_new ();

   e = g_new0 (EstimateData, 1);
   e->d = d;
   e->cancel = g_object_ref (d->estimate_cancel);
   gtk_label_set_text (GTK_LABEL (d->estimate), _("The first image: ..."));

   task = g_task_new (NULL, d->estimate_cancel, estimate_done, e);
   g_task_set_task_data (task, job_from_dialog (d, g_ptr_array_index (d->paths, 0)),
                         job_free);
   g_task_run_in_thread (task, resize_thread);
   g_object_unref (task);
   return G_SOURCE_REMOVE;
}


static void
estimate_later (Dialog *d)
{
   if (d->closed) return;
   if (d->estimate_id) g_source_remove (d->estimate_id);
   d->estimate_id = g_timeout_add (300, timeout_estimate, d);
}


static void
cb_option_changed (GObject *object, GParamSpec *pspec, Dialog *d)
{
   estimate_later (d);
}


static void
cb_metadata_toggled (GtkCheckButton *check, Dialog *d)
{
   gtk_widget_set_sensitive (d->gps_check, gtk_check_button_get_active (check));
}


/*** the saving ***/

/* dest in the folder dirname of the image; a name not taken (never over
   another file) */
static gchar *
dest_for (const gchar *src, const gchar *dirname)
{
   gchar *dir = g_path_get_dirname (src);
   gchar *base = g_path_get_basename (src);
   gchar *outdir = g_build_filename (dir, dirname, NULL);
   gchar *dot, *stem, *path, *ext_new = NULL;
   const gchar *ext;
   GdkPixbufFormat *format;
   gint i;

   /* written as JPEG when its format can not be written */
   format = gdk_pixbuf_get_file_info (src, NULL, NULL);
   if (format) {
      gchar *name = gdk_pixbuf_format_get_name (format);
      writer_of (name, &ext);
      if (ext) ext_new = g_strdup (ext);
      g_free (name);
   }

   dot = strrchr (base, '.');
   stem = dot ? g_strndup (base, dot - base) : g_strdup (base);
   ext = ext_new ? ext_new : (dot ? dot + 1 : "jpg");

   path = g_strdup_printf ("%s/%s.%s", outdir, stem, ext);
   for (i = 1; g_file_test (path, G_FILE_TEST_EXISTS); i++) {
      g_free (path);
      path = g_strdup_printf ("%s/%s-%d.%s", outdir, stem, i, ext);
   }

   g_free (ext_new);
   g_free (stem);
   g_free (outdir);
   g_free (base);
   g_free (dir);
   return path;
}


static void run_next (Dialog *d);

static void
finish (Dialog *d)
{
   gchar *text;
   GString *msg = g_string_new (NULL);

   d->running = FALSE;
   g_string_append_printf (msg, _("%u saved smaller"), d->made);
   if (d->copied)
      g_string_append_printf (msg, _(", %u copied as they are (not larger)"), d->copied);
   if (d->failed)
      g_string_append_printf (msg, _(", %u could not be saved"), d->failed);
   if (g_cancellable_is_cancelled (d->cancel))
      g_string_append (msg, _(" (stopped)"));
   g_string_append (msg, ".");
   if (d->errors && d->errors->len) {
      g_string_append (msg, "\n");
      g_string_append (msg, d->errors->str);
   }
   text = g_string_free (msg, FALSE);
   set_status (d, text, d->failed > 0);
   g_free (text);

   /* a thumbnail view of the folder shows the new files */
   if (d->first_saved) {
      gchar *dir = g_path_get_dirname (d->first_saved);
      gchar *dir_slash = g_strconcat (dir, "/", NULL);
      GimvThumbView *tv = gimv_thumb_view_find_opened_dir (dir_slash);
      if (!tv) tv = gimv_thumb_view_find_opened_dir (dir);
      if (tv) gimv_thumb_view_refresh_list (tv);
      g_free (dir_slash);
      g_free (dir);
   }

   {
      guint done = d->made + d->copied + d->failed;
      gchar *t = g_strdup_printf (_("%u / %u"), done, d->paths->len);
      gtk_progress_bar_set_text (GTK_PROGRESS_BAR (d->progress), t);
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (d->progress),
                                     (gdouble) done / d->paths->len);
      g_free (t);
   }
   gtk_button_set_label (GTK_BUTTON (d->close_button), _("_Close"));
   gtk_widget_set_visible (d->save_button, FALSE);
   gtk_widget_set_visible (d->open_button, d->first_saved != NULL);
}


static void
resize_done (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   Job *job = g_task_get_task_data (G_TASK (res));

   if (job->stopped) {
      /* not saved: not counted */
   } else if (job->error) {
      gchar *name = g_filename_display_basename (job->src);
      d->failed++;
      if (!d->errors) d->errors = g_string_new (NULL);
      if (d->failed <= 5)
         g_string_append_printf (d->errors, "%s%s: %s", d->errors->len ? "\n" : "",
                                 name, job->error);
      g_free (name);
      /* a file begun and not finished is not left */
      if (job->dest && g_file_test (job->dest, G_FILE_TEST_EXISTS) && !job->bytes)
         g_unlink (job->dest);
   } else {
      if (job->resized) d->made++; else d->copied++;
      if (!d->first_saved) d->first_saved = g_strdup (job->dest);
      if (job->comment) copy_comment (job->src, job->dest);
   }

   if (d->closed && !d->running) {
      dialog_free (d);
      return;
   }
   run_next (d);
}


static void
run_next (Dialog *d)
{
   const gchar *dirname;
   gchar *dest, *outdir;
   Job *job;
   GTask *task;
   gchar *text;

   if (d->next >= d->paths->len || g_cancellable_is_cancelled (d->cancel)) {
      if (d->closed) { dialog_free (d); return; }
      finish (d);
      return;
   }

   text = g_strdup_printf (_("%u / %u"), d->next + 1, d->paths->len);
   gtk_progress_bar_set_text (GTK_PROGRESS_BAR (d->progress), text);
   g_free (text);
   gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (d->progress),
                                  (gdouble) d->next / d->paths->len);

   dirname = gtk_editable_get_text (GTK_EDITABLE (d->dir_entry));
   job = job_from_dialog (d, g_ptr_array_index (d->paths, d->next++));
   dest = dest_for (job->src, dirname);
   outdir = g_path_get_dirname (dest);
   if (g_mkdir_with_parents (outdir, 0755) != 0) {
      job->error = g_strdup_printf (_("the folder %s could not be made"), outdir);
   }
   g_free (outdir);
   job->dest = dest;

   task = g_task_new (NULL, d->cancel, resize_done, d);
   g_task_set_task_data (task, job, job_free);
   if (job->error) {
      g_task_return_boolean (task, FALSE);
   } else {
      g_task_run_in_thread (task, resize_thread);
   }
   g_object_unref (task);
}


static void
cb_save (GtkButton *button, Dialog *d)
{
   const gchar *dirname = gtk_editable_get_text (GTK_EDITABLE (d->dir_entry));

   if (d->running) return;
   if (!dirname || !*dirname || strchr (dirname, '/') || !strcmp (dirname, ".")
       || !strcmp (dirname, ".."))
   {
      set_status (d, _("Give the name of a folder (made next to the images)."), TRUE);
      return;
   }

   remember (d);
   if (d->estimate_id) {
      g_source_remove (d->estimate_id);
      d->estimate_id = 0;
   }

   d->running = TRUE;
   d->cancel = g_cancellable_new ();
   gtk_widget_set_sensitive (d->save_button, FALSE);
   gtk_widget_set_visible (d->progress, TRUE);
   gtk_button_set_label (GTK_BUTTON (d->close_button), _("_Stop"));
   set_status (d, NULL, FALSE);
   run_next (d);
}


static void
cb_close (GtkButton *button, Dialog *d)
{
   if (d->running) {
      g_cancellable_cancel (d->cancel);   /* stops after the image being saved */
      return;
   }
   gtk_window_destroy (GTK_WINDOW (d->window));
}


/* the folder of the first image saved, in the thumbnail window, with it
   chosen */
static void
cb_open (GtkButton *button, Dialog *d)
{
   GimvThumbWin *tw = NULL;
   GList *list;

   if (!d->first_saved) return;
   if (d->parent && GIMV_IS_THUMB_WIN (d->parent))
      tw = GIMV_THUMB_WIN (d->parent);
   if (!tw && (list = gimv_thumb_win_get_list ()))
      tw = list->data;
   if (tw) gimv_thumb_view_open_folder_of (tw, d->first_saved);
   gtk_window_destroy (GTK_WINDOW (d->window));
}


static void
cb_destroy (GtkWidget *window, Dialog *d)
{
   d->closed = TRUE;
   if (d->estimate_id) {
      g_source_remove (d->estimate_id);
      d->estimate_id = 0;
   }
   if (d->estimate_cancel) g_cancellable_cancel (d->estimate_cancel);
   if (d->running) {
      g_cancellable_cancel (d->cancel);  /* freed when the image ends */
      return;
   }
   /* a refresh of the folders open there shows the new files */
   dialog_free (d);
}


static GtkWidget *
label_new (const gchar *text)
{
   GtkWidget *label = gtk_label_new_with_mnemonic (text);
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   return label;
}


void
gimv_resize_dialog_open (GList *paths, GtkWindow *parent)
{
   Dialog *d;
   GtkWidget *window, *vbox, *grid, *hbox, *label;
   GList *node;
   gchar *text;
   gint row = 0;
   const gchar *modes[] = { N_("Height"), N_("Width"), N_("Longer side"), NULL };
   const gchar *shown[4];
   guint i;

   d = g_new0 (Dialog, 1);
   d->parent = parent;
   d->paths = g_ptr_array_new_with_free_func (g_free);
   for (node = paths; node; node = g_list_next (node))
      if (node->data) g_ptr_array_add (d->paths, g_strdup (node->data));
   if (d->paths->len == 0) {
      dialog_free (d);
      return;
   }

   window = d->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (window), _("Save Smaller"));
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   gtk_window_set_default_size (GTK_WINDOW (window), 560, -1);
   g_signal_connect (window, "destroy", G_CALLBACK (cb_destroy), d);
   {
      GtkEventController *keys = gtk_shortcut_controller_new ();
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
         gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                           gtk_named_action_new ("window.close")));
      gtk_widget_add_controller (window, keys);
   }

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
   gtk_widget_set_margin_top (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);
   gtk_widget_set_margin_start (vbox, 12);
   gtk_widget_set_margin_end (vbox, 12);
   gtk_window_set_child (GTK_WINDOW (window), vbox);

   grid = gtk_grid_new ();
   gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
   gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
   gtk_box_append (GTK_BOX (vbox), grid);

   /* the images */
   gtk_grid_attach (GTK_GRID (grid), label_new (_("Images:")), 0, row, 1, 1);
   if (d->paths->len == 1) {
      text = g_filename_display_basename (g_ptr_array_index (d->paths, 0));
   } else {
      gchar *first = g_filename_display_basename (g_ptr_array_index (d->paths, 0));
      text = g_strdup_printf (_("%u images (%s, ...)"), d->paths->len, first);
      g_free (first);
   }
   label = gtk_label_new (text);
   g_free (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_grid_attach (GTK_GRID (grid), label, 1, row++, 1, 1);

   /* the size */
   label = label_new (_("Si_ze:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   for (i = 0; modes[i]; i++) shown[i] = _(modes[i]);
   shown[i] = NULL;
   d->mode_drop = gtk_drop_down_new_from_strings (shown);
   gtk_drop_down_set_selected (GTK_DROP_DOWN (d->mode_drop),
                               CLAMP (conf.resize_mode, SIZE_HEIGHT, SIZE_LONG_SIDE));
   gtk_box_append (GTK_BOX (hbox), d->mode_drop);
   d->size_spin = gtk_spin_button_new_with_range (16, 20000, 10);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (d->size_spin),
                              conf.resize_size > 0 ? conf.resize_size : 800);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->size_spin);
   gtk_box_append (GTK_BOX (hbox), d->size_spin);
   gtk_box_append (GTK_BOX (hbox), gtk_label_new (_("px or less")));
   gtk_grid_attach (GTK_GRID (grid), hbox, 1, row++, 1, 1);

   /* the quality */
   label = label_new (_("_Quality:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   d->quality_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
   gtk_scale_set_draw_value (GTK_SCALE (d->quality_scale), TRUE);
   gtk_scale_set_value_pos (GTK_SCALE (d->quality_scale), GTK_POS_RIGHT);
   gtk_scale_set_digits (GTK_SCALE (d->quality_scale), 0);
   gtk_range_set_value (GTK_RANGE (d->quality_scale),
                        conf.resize_quality > 0 ? conf.resize_quality : 85);
   gtk_widget_set_hexpand (d->quality_scale, TRUE);
   gtk_widget_set_tooltip_text (d->quality_scale, _("Of JPEG and WebP"));
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->quality_scale);
   gtk_grid_attach (GTK_GRID (grid), d->quality_scale, 1, row++, 1, 1);

   /* where */
   label = label_new (_("Save _in the folder:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   d->dir_entry = gtk_entry_new ();
   gtk_editable_set_text (GTK_EDITABLE (d->dir_entry),
                          conf.resize_dir && *conf.resize_dir ? conf.resize_dir : "resized");
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->dir_entry);
   g_signal_connect_swapped (d->dir_entry, "activate", G_CALLBACK (cb_save), d);
   gtk_box_append (GTK_BOX (hbox), d->dir_entry);
   label = gtk_label_new (_("(next to each image; the images are not changed)"));
   gtk_widget_add_css_class (label, "dim-label");
   gtk_box_append (GTK_BOX (hbox), label);
   gtk_grid_attach (GTK_GRID (grid), hbox, 1, row++, 1, 1);

   /* the information */
   d->metadata_check = gtk_check_button_new_with_mnemonic
      (_("_Keep the information of the file (EXIF: the date, the camera, ...)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->metadata_check), conf.resize_metadata);
   g_signal_connect (d->metadata_check, "toggled", G_CALLBACK (cb_metadata_toggled), d);
   gtk_grid_attach (GTK_GRID (grid), d->metadata_check, 1, row++, 1, 1);
   d->gps_check = gtk_check_button_new_with_mnemonic
      (_("Without the _place it was taken at (GPS)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->gps_check), conf.resize_no_gps);
   gtk_widget_set_margin_start (d->gps_check, 24);
   gtk_widget_set_sensitive (d->gps_check, conf.resize_metadata);
   gtk_grid_attach (GTK_GRID (grid), d->gps_check, 1, row++, 1, 1);
#ifndef ENABLE_GEXIV2
   gtk_widget_set_visible (d->metadata_check, FALSE);
   gtk_widget_set_visible (d->gps_check, FALSE);
#endif
   d->comment_check = gtk_check_button_new_with_mnemonic
      (_("Copy the _comment and the tags given by hand"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->comment_check), conf.resize_comment);
   gtk_grid_attach (GTK_GRID (grid), d->comment_check, 1, row++, 1, 1);

   /* the estimate */
   d->estimate = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (d->estimate), 0.0);
   gtk_label_set_wrap (GTK_LABEL (d->estimate), TRUE);
   gtk_widget_add_css_class (d->estimate, "dim-label");
   gtk_grid_attach (GTK_GRID (grid), d->estimate, 1, row++, 1, 1);

   /* the progress, the status, the buttons */
   d->progress = gtk_progress_bar_new ();
   gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (d->progress), TRUE);
   gtk_widget_set_visible (d->progress, FALSE);
   gtk_box_append (GTK_BOX (vbox), d->progress);
   d->status = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (d->status), 0.0);
   gtk_label_set_wrap (GTK_LABEL (d->status), TRUE);
   gtk_label_set_selectable (GTK_LABEL (d->status), TRUE);
   gtk_box_append (GTK_BOX (vbox), d->status);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_halign (hbox, GTK_ALIGN_END);
   d->open_button = gtk_button_new_with_mnemonic (_("_Open the Folder"));
   gtk_widget_set_visible (d->open_button, FALSE);
   g_signal_connect (d->open_button, "clicked", G_CALLBACK (cb_open), d);
   gtk_box_append (GTK_BOX (hbox), d->open_button);
   d->close_button = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (d->close_button, "clicked", G_CALLBACK (cb_close), d);
   gtk_box_append (GTK_BOX (hbox), d->close_button);
   d->save_button = gtk_button_new_with_mnemonic (_("_Save"));
   gtk_widget_add_css_class (d->save_button, "suggested-action");
   g_signal_connect (d->save_button, "clicked", G_CALLBACK (cb_save), d);
   gtk_box_append (GTK_BOX (hbox), d->save_button);
   gtk_box_append (GTK_BOX (vbox), hbox);
   gtk_window_set_default_widget (GTK_WINDOW (window), d->save_button);

   g_signal_connect (d->mode_drop, "notify::selected", G_CALLBACK (cb_option_changed), d);
   g_signal_connect (d->size_spin, "notify::value", G_CALLBACK (cb_option_changed), d);
   g_signal_connect (gtk_range_get_adjustment (GTK_RANGE (d->quality_scale)),
                     "notify::value", G_CALLBACK (cb_option_changed), d);
   estimate_later (d);

   gtk_window_present (GTK_WINDOW (window));
   gtk_widget_grab_focus (d->save_button);
}
