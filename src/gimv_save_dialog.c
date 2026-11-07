/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: saving the image shown in another format (JPEG, PNG, WebP,
 * ...), with the image saver plugins.
 *
 * The image is saved as it is shown (turned) in its full size.  It is
 * written in a thread; the information of the file (EXIF, XMP, IPTC: the
 * date, the camera, ...) is copied with Exiv2 when built with gexiv2, the
 * orientation then put to normal.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_save_dialog.h"

#include <string.h>

#include "gimageview.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_image_saver.h"
#include "gimv_thumb_view.h"

#ifdef ENABLE_GEXIV2
#  include <gexiv2/gexiv2.h>
#endif


typedef struct {
   const gchar *label;
   const gchar *format;     /* of the saver plugins */
   const gchar *ext;
   const gchar *writer;     /* gdk-pixbuf must write it, or NULL */
   gboolean     quality;
   gboolean     metadata;   /* Exiv2 can write its EXIF, XMP, IPTC */
} Format;

static const Format formats[] = {
   { "JPEG", "jpeg", "jpg",  "jpeg", TRUE,  TRUE  },
   { "PNG",  "png",  "png",  NULL,   FALSE, TRUE  },
   { "WebP", "webp", "webp", "webp", TRUE,  TRUE  },
   { "AVIF", "avif", "avif", "avif", TRUE,  FALSE },
   { "TIFF", "tiff", "tif",  "tiff", FALSE, TRUE  },
   { "BMP",  "bmp",  "bmp",  "bmp",  FALSE, FALSE },
   { "PNM",  "pnm",  "ppm",  NULL,   FALSE, FALSE },
};

/* what was chosen last (this session) */
static gchar *last_format  = NULL;
static gint   last_quality = 90;
static gboolean last_metadata = TRUE;


typedef struct {
   GtkWidget     *window;
   GimvImage     *image;        /* full size, as shown */
   gchar         *src_path;     /* the file shown, or NULL */

   GPtrArray     *shown;        /* const Format *: those that can be saved */
   GtkWidget     *path_entry;
   GtkWidget     *format_drop;
   GtkWidget     *quality_label;
   GtkWidget     *quality_scale;
   GtkWidget     *metadata_check;
   GtkWidget     *status;
   GtkWidget     *spinner;
   GtkWidget     *save_button;
   GtkWidget     *cancel_button;
   gboolean       saving;
   gboolean       closed;       /* the window is gone, the save still runs */
} Dialog;


static void
dialog_free (Dialog *d)
{
   if (d->image) gimv_image_unref (d->image);
   g_free (d->src_path);
   if (d->shown) g_ptr_array_unref (d->shown);
   g_free (d);
}


static gboolean
pixbuf_writes (const gchar *name)
{
   GSList *list = gdk_pixbuf_get_formats (), *node;
   gboolean ret = FALSE;

   for (node = list; node && !ret; node = g_slist_next (node)) {
      GdkPixbufFormat *f = node->data;
      gchar *n = gdk_pixbuf_format_get_name (f);
      if (!g_strcmp0 (n, name) && gdk_pixbuf_format_is_writable (f)) ret = TRUE;
      g_free (n);
   }
   g_slist_free (list);
   return ret;
}


static const Format *
current_format (Dialog *d)
{
   guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (d->format_drop));
   if (i == GTK_INVALID_LIST_POSITION || i >= d->shown->len) return NULL;
   return g_ptr_array_index (d->shown, i);
}


static void
set_status (Dialog *d, const gchar *text, gboolean error)
{
   gtk_label_set_text (GTK_LABEL (d->status), text ? text : "");
   if (error) gtk_widget_add_css_class (d->status, "error");
   else gtk_widget_remove_css_class (d->status, "error");
}


/* the extension of the path to the format's (when it has one we know) */
static void
fix_extension (Dialog *d, const Format *f)
{
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (d->path_entry));
   const gchar *slash = strrchr (text, '/');
   const gchar *dot = strrchr (text, '.');
   gchar *stem, *path;

   if (!f || !*text) return;
   if (dot && (!slash || dot > slash))
      stem = g_strndup (text, dot - text);
   else
      stem = g_strdup (text);
   path = g_strdup_printf ("%s.%s", stem, f->ext);
   /* not over the file shown by mistake */
   if (d->src_path && !strcmp (path, d->src_path)) {
      g_free (path);
      path = g_strdup_printf ("%s_2.%s", stem, f->ext);
   }
   gtk_editable_set_text (GTK_EDITABLE (d->path_entry), path);
   g_free (stem);
   g_free (path);
}


static void
update_options (Dialog *d)
{
   const Format *f = current_format (d);

   gtk_widget_set_visible (d->quality_label, f && f->quality);
   gtk_widget_set_visible (d->quality_scale, f && f->quality);
#ifdef ENABLE_GEXIV2
   gtk_widget_set_sensitive (d->metadata_check, f && f->metadata && d->src_path);
#endif
}


static void
cb_format_changed (GObject *drop, GParamSpec *pspec, Dialog *d)
{
   const Format *f = current_format (d);

   fix_extension (d, f);
   update_options (d);
   set_status (d, NULL, FALSE);
   if (f) {
      g_free (last_format);
      last_format = g_strdup (f->format);
   }
}


/******************************************************************************
 *   choosing the file
 ******************************************************************************/

static void
cb_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), res, NULL);

   if (file && !d->closed) {
      gchar *path = g_file_get_path (file);
      if (path) {
         gtk_editable_set_text (GTK_EDITABLE (d->path_entry), path);
         fix_extension (d, current_format (d));
      }
      g_free (path);
   }
   if (file) g_object_unref (file);
}


static void
cb_browse (GtkButton *button, Dialog *d)
{
   GtkFileDialog *fd = gtk_file_dialog_new ();
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (d->path_entry));

   gtk_file_dialog_set_title (fd, _("Save Image As"));
   if (text && *text) {
      gchar *dir = g_path_get_dirname (text), *name = g_path_get_basename (text);
      GFile *folder = g_file_new_for_path (dir);
      gtk_file_dialog_set_initial_folder (fd, folder);
      gtk_file_dialog_set_initial_name (fd, name);
      g_object_unref (folder);
      g_free (dir);
      g_free (name);
   }
   gtk_file_dialog_save (fd, GTK_WINDOW (d->window), NULL, cb_chosen, d);
   g_object_unref (fd);
}


/******************************************************************************
 *   saving
 ******************************************************************************/

typedef struct {
   Dialog         *d;
   GimvImageSaver *saver;
   gchar          *path;
   gchar          *src_path;     /* the information from, or NULL */
   gboolean        metadata;
   gchar          *error;
   gchar          *warning;      /* saved, but the information not copied */
} SaveJob;


static void
save_job_free (gpointer p)
{
   SaveJob *job = p;
   gimv_image_saver_unref (job->saver);
   g_free (job->path);
   g_free (job->src_path);
   g_free (job->error);
   g_free (job->warning);
   g_free (job);
}


#ifdef ENABLE_GEXIV2
/* the EXIF, XMP and IPTC of src into dest (saved as it is shown: the
 * orientation is normal, the thumbnail of the EXIF is gone) */
static gboolean
copy_metadata (const gchar *src, const gchar *dest, gint width, gint height,
               gchar **error_text)
{
   static gsize init = 0;
   GExiv2Metadata *m;
   GError *error = NULL;
   gboolean ok = FALSE;

   if (g_once_init_enter (&init)) {
      gexiv2_initialize ();
      g_once_init_leave (&init, 1);
   }

   m = gexiv2_metadata_new ();
   if (!gexiv2_metadata_open_path (m, src, &error)) goto out;
   if (!gexiv2_metadata_has_exif (m) && !gexiv2_metadata_has_xmp (m)
       && !gexiv2_metadata_has_iptc (m))
   {
      ok = TRUE;   /* nothing to copy */
      goto out;
   }

   G_GNUC_BEGIN_IGNORE_DEPRECATIONS
   gexiv2_metadata_try_set_orientation (m, GEXIV2_ORIENTATION_NORMAL, NULL);
   gexiv2_metadata_try_erase_exif_thumbnail (m, NULL);
   {
      gchar w[16], h[16];
      g_snprintf (w, sizeof w, "%d", width);
      g_snprintf (h, sizeof h, "%d", height);
      if (gexiv2_metadata_try_has_tag (m, "Exif.Photo.PixelXDimension", NULL))
         gexiv2_metadata_try_set_tag_string (m, "Exif.Photo.PixelXDimension", w, NULL);
      if (gexiv2_metadata_try_has_tag (m, "Exif.Photo.PixelYDimension", NULL))
         gexiv2_metadata_try_set_tag_string (m, "Exif.Photo.PixelYDimension", h, NULL);
      if (gexiv2_metadata_try_has_tag (m, "Xmp.tiff.Orientation", NULL))
         gexiv2_metadata_try_set_tag_string (m, "Xmp.tiff.Orientation", "1", NULL);
   }
   G_GNUC_END_IGNORE_DEPRECATIONS

   ok = gexiv2_metadata_save_file (m, dest, &error);

out:
   if (!ok && error_text)
      *error_text = g_strdup (error ? error->message : "");
   g_clear_error (&error);
   g_object_unref (m);
   return ok;
}
#endif


static void
save_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   SaveJob *job = data;

   if (!gimv_image_saver_save (job->saver)) {
      const gchar *e = gimv_image_saver_get_option (job->saver, "error");
      job->error = g_strdup (e && *e ? e : _("The image could not be written."));
   }
#ifdef ENABLE_GEXIV2
   else if (job->metadata && job->src_path) {
      gchar *e = NULL;
      GimvImage *image = gimv_image_saver_get_image (job->saver);
      if (!copy_metadata (job->src_path, job->path,
                          gimv_image_width (image), gimv_image_height (image), &e))
         job->warning = e;
   }
#endif
   g_task_return_boolean (task, TRUE);
}


/* a thumbnail view of the folder shows the file */
static void
refresh_folder (const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *dir_slash = g_strconcat (dir, "/", NULL);
   GimvThumbView *tv = gimv_thumb_view_find_opened_dir (dir_slash);

   if (!tv) tv = gimv_thumb_view_find_opened_dir (dir);
   if (tv) gimv_thumb_view_refresh_list (tv);
   g_free (dir);
   g_free (dir_slash);
}


static void
save_done (GObject *source, GAsyncResult *res, gpointer data)
{
   SaveJob *job = g_task_get_task_data (G_TASK (res));
   Dialog *d = job->d;

   g_task_propagate_boolean (G_TASK (res), NULL);
   d->saving = FALSE;

   if (!job->error) refresh_folder (job->path);

   if (d->closed) {
      dialog_free (d);
      return;
   }

   gtk_spinner_stop (GTK_SPINNER (d->spinner));
   gtk_widget_set_visible (d->spinner, FALSE);
   gtk_widget_set_sensitive (d->save_button, TRUE);

   if (job->error) {
      gchar *text = g_strdup_printf (_("Could not save: %s"), job->error);
      set_status (d, text, TRUE);
      g_free (text);
      return;
   }
   if (job->warning) {
      gchar *text = g_strdup_printf (_("Saved, but the information of the file "
                                       "(EXIF, ...) could not be copied: %s"),
                                     job->warning);
      set_status (d, text, TRUE);
      g_free (text);
      gtk_button_set_label (GTK_BUTTON (d->cancel_button), _("_Close"));
      return;
   }
   gtk_window_destroy (GTK_WINDOW (d->window));
}


static void
start_save (Dialog *d, const gchar *path)
{
   const Format *f = current_format (d);
   SaveJob *job;
   GTask *task;

   job = g_new0 (SaveJob, 1);
   job->d = d;
   job->path = g_strdup (path);
   job->src_path = g_strdup (d->src_path);
   job->saver = gimv_image_saver_new_with_attr (d->image, path, f->format);
   if (f->quality) {
      gchar q[8];
      last_quality = (gint) gtk_range_get_value (GTK_RANGE (d->quality_scale));
      g_snprintf (q, sizeof q, "%d", last_quality);
      gimv_image_saver_set_option (job->saver, "quality", q);
   }
#ifdef ENABLE_GEXIV2
   last_metadata = gtk_check_button_get_active (GTK_CHECK_BUTTON (d->metadata_check));
   job->metadata = f->metadata && last_metadata;
#endif

   d->saving = TRUE;
   gtk_widget_set_sensitive (d->save_button, FALSE);
   gtk_widget_set_visible (d->spinner, TRUE);
   gtk_spinner_start (GTK_SPINNER (d->spinner));
   set_status (d, _("Saving..."), FALSE);

   task = g_task_new (NULL, NULL, save_done, NULL);
   g_task_set_task_data (task, job, save_job_free);
   g_task_run_in_thread (task, save_thread);
   g_object_unref (task);
}


static void
cb_overwrite_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   Dialog *d = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);

   if (button == 1 && !d->closed && !d->saving)
      start_save (d, gtk_editable_get_text (GTK_EDITABLE (d->path_entry)));
}


static void
cb_save (GtkWidget *widget, Dialog *d)
{
   const gchar *path = gtk_editable_get_text (GTK_EDITABLE (d->path_entry));
   const Format *f = current_format (d);

   if (d->saving || !f) return;
   if (!path || !*path || !g_path_is_absolute (path)) {
      set_status (d, _("Give the file to save to (its full path)."), TRUE);
      return;
   }
   {
      gchar *dir = g_path_get_dirname (path);
      gboolean ok = g_file_test (dir, G_FILE_TEST_IS_DIR);
      g_free (dir);
      if (!ok) {
         set_status (d, _("The folder is not there."), TRUE);
         return;
      }
   }

   if (g_file_test (path, G_FILE_TEST_EXISTS)) {
      gboolean same = d->src_path && !strcmp (path, d->src_path);
      GtkAlertDialog *ask = gtk_alert_dialog_new (same
         ? _("Overwrite the image shown?")
         : _("The file is there already.  Overwrite it?"));
      const gchar *buttons[] = { _("_Cancel"), _("_Overwrite"), NULL };
      gchar *name = g_filename_display_name (path);

      gtk_alert_dialog_set_detail (ask, name);
      gtk_alert_dialog_set_buttons (ask, buttons);
      gtk_alert_dialog_set_cancel_button (ask, 0);
      gtk_alert_dialog_set_default_button (ask, 0);
      gtk_alert_dialog_choose (ask, GTK_WINDOW (d->window), NULL,
                               cb_overwrite_answer, d);
      g_object_unref (ask);
      g_free (name);
      return;
   }
   start_save (d, path);
}


static void
cb_cancel (GtkButton *button, Dialog *d)
{
   gtk_window_destroy (GTK_WINDOW (d->window));
}


static void
cb_destroy (GtkWidget *window, Dialog *d)
{
   d->closed = TRUE;
   /* a save still running finishes and frees it */
   if (!d->saving) dialog_free (d);
}


/******************************************************************************
 *   the dialog
 ******************************************************************************/

static GtkWidget *
label_new (const gchar *text)
{
   GtkWidget *label = gtk_label_new_with_mnemonic (text);
   gtk_label_set_xalign (GTK_LABEL (label), 1.0);
   return label;
}


void
gimv_save_dialog_open (GimvImageView *iv, GtkWindow *parent)
{
   Dialog *d;
   GtkWidget *window, *vbox, *grid, *hbox, *label, *button;
   GtkStringList *names;
   const gchar *src = NULL;
   gchar *text;
   guint i, select = 0;
   gint row = 0;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   d = g_new0 (Dialog, 1);
   d->image = gimv_image_view_get_full_image (iv);
   if (!d->image) {
      GtkAlertDialog *alert = gtk_alert_dialog_new ("%s", _("There is no image to save."));
      gtk_alert_dialog_show (alert, parent);
      g_object_unref (alert);
      g_free (d);
      return;
   }
   if (iv->info && !gimv_image_info_is_in_archive (iv->info))
      src = gimv_image_info_get_path (iv->info);
   d->src_path = src ? g_strdup (src) : NULL;

   /* the formats there are savers for */
   d->shown = g_ptr_array_new ();
   names = gtk_string_list_new (NULL);
   for (i = 0; i < G_N_ELEMENTS (formats); i++) {
      const Format *f = &formats[i];
      if (!gimv_image_saver_has_format (f->format)) continue;
      if (f->writer && !pixbuf_writes (f->writer)) continue;
      if (last_format && !strcmp (last_format, f->format)) select = d->shown->len;
      g_ptr_array_add (d->shown, (gpointer) f);
      gtk_string_list_append (names, f->label);
   }

   window = d->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (window), _("Save Image"));
   gtk_window_set_modal (GTK_WINDOW (window), TRUE);
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

   /* the image */
   gtk_grid_attach (GTK_GRID (grid), label_new (_("Image:")), 0, row, 1, 1);
   {
      gchar *name = d->src_path ? g_filename_display_basename (d->src_path)
                                : g_strdup (_("(not a file)"));
      text = g_strdup_printf ("%s   %d × %d", name,
                              gimv_image_width (d->image), gimv_image_height (d->image));
      g_free (name);
   }
   label = gtk_label_new (text);
   g_free (text);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   gtk_grid_attach (GTK_GRID (grid), label, 1, row++, 1, 1);

   /* where */
   label = label_new (_("Save _to:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   d->path_entry = gtk_entry_new ();
   gtk_widget_set_hexpand (d->path_entry, TRUE);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->path_entry);
   g_signal_connect (d->path_entry, "activate", G_CALLBACK (cb_save), d);
   gtk_box_append (GTK_BOX (hbox), d->path_entry);
   button = gtk_button_new_with_mnemonic (_("_Browse..."));
   g_signal_connect (button, "clicked", G_CALLBACK (cb_browse), d);
   gtk_box_append (GTK_BOX (hbox), button);
   gtk_grid_attach (GTK_GRID (grid), hbox, 1, row++, 1, 1);

   /* the format */
   label = label_new (_("_Format:"));
   gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
   d->format_drop = gtk_drop_down_new (G_LIST_MODEL (names), NULL);
   gtk_widget_set_halign (d->format_drop, GTK_ALIGN_START);
   gtk_label_set_mnemonic_widget (GTK_LABEL (label), d->format_drop);
   gtk_grid_attach (GTK_GRID (grid), d->format_drop, 1, row++, 1, 1);

   /* the quality */
   d->quality_label = label_new (_("_Quality:"));
   gtk_grid_attach (GTK_GRID (grid), d->quality_label, 0, row, 1, 1);
   d->quality_scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
   gtk_scale_set_draw_value (GTK_SCALE (d->quality_scale), TRUE);
   gtk_scale_set_value_pos (GTK_SCALE (d->quality_scale), GTK_POS_RIGHT);
   gtk_scale_set_digits (GTK_SCALE (d->quality_scale), 0);
   gtk_range_set_value (GTK_RANGE (d->quality_scale), last_quality);
   gtk_widget_set_hexpand (d->quality_scale, TRUE);
   gtk_label_set_mnemonic_widget (GTK_LABEL (d->quality_label), d->quality_scale);
   gtk_grid_attach (GTK_GRID (grid), d->quality_scale, 1, row++, 1, 1);

   /* the information of the file */
   d->metadata_check = gtk_check_button_new_with_mnemonic
      (_("_Keep the information of the file (EXIF, XMP, IPTC: the date, the camera, ...)"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (d->metadata_check), last_metadata);
   gtk_grid_attach (GTK_GRID (grid), d->metadata_check, 1, row++, 1, 1);
#ifndef ENABLE_GEXIV2
   gtk_widget_set_visible (d->metadata_check, FALSE);
#endif

   label = gtk_label_new (_("The image is saved in its full size, turned as it is shown."));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_widget_add_css_class (label, "dim-label");
   gtk_grid_attach (GTK_GRID (grid), label, 1, row++, 1, 1);

   /* the status, the buttons */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
   gtk_widget_set_margin_top (hbox, 6);
   d->spinner = gtk_spinner_new ();
   gtk_widget_set_visible (d->spinner, FALSE);
   gtk_box_append (GTK_BOX (hbox), d->spinner);
   d->status = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (d->status), 0.0);
   gtk_label_set_wrap (GTK_LABEL (d->status), TRUE);
   gtk_widget_set_hexpand (d->status, TRUE);
   gtk_box_append (GTK_BOX (hbox), d->status);
   d->cancel_button = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (d->cancel_button, "clicked", G_CALLBACK (cb_cancel), d);
   gtk_box_append (GTK_BOX (hbox), d->cancel_button);
   d->save_button = gtk_button_new_with_mnemonic (_("_Save"));
   gtk_widget_add_css_class (d->save_button, "suggested-action");
   g_signal_connect (d->save_button, "clicked", G_CALLBACK (cb_save), d);
   gtk_box_append (GTK_BOX (hbox), d->save_button);
   gtk_box_append (GTK_BOX (vbox), hbox);
   gtk_window_set_default_widget (GTK_WINDOW (window), d->save_button);

   /* the path: next to the file shown, with the extension of the format */
   if (d->src_path) {
      gtk_editable_set_text (GTK_EDITABLE (d->path_entry), d->src_path);
   } else {
      gchar *p = g_build_filename (g_get_home_dir (), "image", NULL);
      gtk_editable_set_text (GTK_EDITABLE (d->path_entry), p);
      g_free (p);
   }

   if (d->shown->len == 0) {
      set_status (d, _("No image saver plugin is there."), TRUE);
      gtk_widget_set_sensitive (d->save_button, FALSE);
   } else {
      gtk_drop_down_set_selected (GTK_DROP_DOWN (d->format_drop), select);
      fix_extension (d, current_format (d));
   }
   g_signal_connect (d->format_drop, "notify::selected", G_CALLBACK (cb_format_changed), d);
   update_options (d);

   gtk_window_present (GTK_WINDOW (window));
   gtk_widget_grab_focus (d->path_entry);
}
