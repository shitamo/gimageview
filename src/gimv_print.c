/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
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
 *  Printing (GTK 4 port; the TODO had "printing support using libgimp").
 *
 *  GtkPrintOperation, one image per page.  The "Image" tab of the print
 *  dialog chooses the size (fit to the page, or a resolution in dpi, never
 *  larger than the page), whether the image is turned to the orientation
 *  of the page and whether the file name is printed under it.  The print
 *  settings, the page setup and these options are kept in
 *  ~/.gimv/print.ini.
 */

#include <string.h>
#include <math.h>
#include "gimageview.h"
#include "intl.h"
#include "gimv_image.h"
#include "gimv_image_info.h"
#include "gimv_image_loader.h"
#include "gimv_print.h"
#include "gtkutils.h"
#include "charset.h"
#include "prefs.h"

#define PRINT_RC_FILE     "print.ini"
#define OPTIONS_GROUP     "GImageView"
#define DEFAULT_DPI       300
#define CAPTION_FONT      "Sans 9"
#define CAPTION_GAP       6.0   /* points between image and file name */


typedef struct PrintOptions_Tag
{
   gboolean fit;          /* fit to the page, else use dpi */
   gint     dpi;
   gboolean auto_rotate;  /* turn landscape images on portrait pages */
   gboolean print_name;
} PrintOptions;


typedef struct PrintItem_Tag
{
   GimvImageInfo            *info;
   GimvImageViewOrientation  rotate;   /* rotation of the image view */
} PrintItem;


typedef struct PrintJob_Tag
{
   GList        *items;     /* PrintItem */
   GtkWindow    *parent;
   PrintOptions  options;

   /* custom tab */
   GtkWidget    *fit_radio;
   GtkWidget    *dpi_radio;
   GtkWidget    *dpi_spin;
   GtkWidget    *rotate_check;
   GtkWidget    *name_check;
} PrintJob;


static GtkPrintSettings *print_settings = NULL;
static GtkPageSetup     *page_setup     = NULL;
static PrintOptions      print_options  = { TRUE, DEFAULT_DPI, TRUE, FALSE };
static gboolean          settings_loaded = FALSE;


/******************************************************************************
 *
 *   settings
 *
 ******************************************************************************/
static gchar *
print_rc_path (void)
{
   return g_build_filename (g_get_home_dir (), GIMV_RC_DIR, PRINT_RC_FILE, NULL);
}


static void
load_settings (void)
{
   GKeyFile *keyfile;
   gchar *path;

   if (settings_loaded) return;
   settings_loaded = TRUE;

   keyfile = g_key_file_new ();
   path = print_rc_path ();

   if (g_key_file_load_from_file (keyfile, path, G_KEY_FILE_NONE, NULL)) {
      GError *error = NULL;
      gint dpi;

      print_settings = gtk_print_settings_new_from_key_file (keyfile, NULL, NULL);
      page_setup     = gtk_page_setup_new_from_key_file (keyfile, NULL, NULL);

      if (g_key_file_has_key (keyfile, OPTIONS_GROUP, "fit", NULL))
         print_options.fit
            = g_key_file_get_boolean (keyfile, OPTIONS_GROUP, "fit", NULL);
      dpi = g_key_file_get_integer (keyfile, OPTIONS_GROUP, "dpi", &error);
      if (!error && dpi >= 10 && dpi <= 2400)
         print_options.dpi = dpi;
      g_clear_error (&error);
      if (g_key_file_has_key (keyfile, OPTIONS_GROUP, "auto_rotate", NULL))
         print_options.auto_rotate
            = g_key_file_get_boolean (keyfile, OPTIONS_GROUP, "auto_rotate", NULL);
      if (g_key_file_has_key (keyfile, OPTIONS_GROUP, "print_name", NULL))
         print_options.print_name
            = g_key_file_get_boolean (keyfile, OPTIONS_GROUP, "print_name", NULL);
   }

   g_free (path);
   g_key_file_free (keyfile);
}


static void
save_settings (void)
{
   GKeyFile *keyfile;
   GError *error = NULL;
   gchar *path, *dir;

   keyfile = g_key_file_new ();

   if (print_settings)
      gtk_print_settings_to_key_file (print_settings, keyfile, NULL);
   if (page_setup)
      gtk_page_setup_to_key_file (page_setup, keyfile, NULL);

   g_key_file_set_boolean (keyfile, OPTIONS_GROUP, "fit", print_options.fit);
   g_key_file_set_integer (keyfile, OPTIONS_GROUP, "dpi", print_options.dpi);
   g_key_file_set_boolean (keyfile, OPTIONS_GROUP, "auto_rotate",
                           print_options.auto_rotate);
   g_key_file_set_boolean (keyfile, OPTIONS_GROUP, "print_name",
                           print_options.print_name);

   dir = g_build_filename (g_get_home_dir (), GIMV_RC_DIR, NULL);
   g_mkdir_with_parents (dir, 0700);
   g_free (dir);

   path = print_rc_path ();
   if (!g_key_file_save_to_file (keyfile, path, &error)) {
      g_warning ("cannot save %s: %s", path, error->message);
      g_error_free (error);
   }
   g_free (path);
   g_key_file_free (keyfile);
}


/******************************************************************************
 *
 *   job
 *
 ******************************************************************************/
static void
print_job_free (PrintJob *job)
{
   GList *node;

   for (node = job->items; node; node = g_list_next (node)) {
      PrintItem *item = node->data;
      gimv_image_info_unref (item->info);
      g_free (item);
   }
   g_list_free (job->items);

   if (job->parent)
      g_object_remove_weak_pointer (G_OBJECT (job->parent),
                                    (gpointer *) &job->parent);
   g_free (job);
}


static gboolean
info_is_printable (GimvImageInfo *info)
{
   if (!info) return FALSE;
   if (gimv_image_info_is_dir (info)
       || gimv_image_info_is_archive (info)
       || gimv_image_info_is_movie (info)
       || gimv_image_info_is_audio (info))
   {
      return FALSE;
   }
   return TRUE;
}


static void
print_job_add (PrintJob *job, GimvImageInfo *info,
               GimvImageViewOrientation rotate)
{
   PrintItem *item;

   if (!info_is_printable (info)) return;

   /* files in archives must be on the disk before printing starts:
      extracting runs a main loop, which must not happen while a page is
      drawn */
   if (gimv_image_info_is_in_archive (info)
       && !gimv_image_info_extract_archive (info))
   {
      return;
   }

   item = g_new0 (PrintItem, 1);
   item->info   = gimv_image_info_ref (info);
   item->rotate = rotate;
   job->items = g_list_append (job->items, item);
}


/******************************************************************************
 *
 *   drawing
 *
 ******************************************************************************/
/* RGB(A) of a GimvImage -> premultiplied ARGB32 cairo surface */
static cairo_surface_t *
surface_from_image (GimvImage *image)
{
   cairo_surface_t *surface;
   const guchar *src, *s;
   guchar *dest;
   gint width, height, rowstride, dstride, x, y;
   gboolean alpha;

   src       = gimv_image_get_pixels (image);
   width     = gimv_image_width (image);
   height    = gimv_image_height (image);
   rowstride = gimv_image_rowstride (image);
   alpha     = gimv_image_has_alpha (image);

   if (!src || width < 1 || height < 1) return NULL;

   surface = cairo_image_surface_create (alpha ? CAIRO_FORMAT_ARGB32
                                               : CAIRO_FORMAT_RGB24,
                                         width, height);
   if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy (surface);
      return NULL;
   }

   cairo_surface_flush (surface);
   dest    = cairo_image_surface_get_data (surface);
   dstride = cairo_image_surface_get_stride (surface);

   for (y = 0; y < height; y++) {
      guint32 *d = (guint32 *) (dest + y * dstride);
      s = src + y * rowstride;
      for (x = 0; x < width; x++) {
         guint r = s[0], g = s[1], b = s[2], a = 255;
         if (alpha) {
            a = s[3];
            r = (r * a + 127) / 255;
            g = (g * a + 127) / 255;
            b = (b * a + 127) / 255;
            s += 4;
         } else {
            s += 3;
         }
         d[x] = (a << 24) | (r << 16) | (g << 8) | b;
      }
   }

   cairo_surface_mark_dirty (surface);

   return surface;
}


static GimvImage *
load_image (GimvImageInfo *info)
{
   GimvImageLoader *loader;
   GimvImage *image;

   loader = gimv_image_loader_new_with_image_info (info);
   if (!loader) return NULL;

   gimv_image_loader_set_as_animation (loader, FALSE);
   gimv_image_loader_load (loader);
   image = gimv_image_loader_get_image (loader);
   if (image) gimv_image_ref (image);
   gimv_image_loader_unref (loader);

   return image;
}


static void
draw_caption (GtkPrintContext *context, cairo_t *cr, PangoLayout *layout,
              gdouble page_width, gdouble y)
{
   gint lw, lh;

   pango_layout_get_size (layout, &lw, &lh);
   cairo_set_source_rgb (cr, 0, 0, 0);
   cairo_move_to (cr, (page_width - (gdouble) lw / PANGO_SCALE) / 2.0, y);
   pango_cairo_show_layout (cr, layout);
}


static void
cb_draw_page (GtkPrintOperation *op, GtkPrintContext *context,
              gint page_nr, PrintJob *job)
{
   PrintItem *item = g_list_nth_data (job->items, page_nr);
   GimvImage *image;
   cairo_surface_t *surface;
   cairo_t *cr;
   PangoLayout *layout = NULL;
   gdouble pw, ph, area_h, caption_h = 0.0;
   gdouble iw, ih, w, h, scale;
   gint quarter;   /* clockwise quarter turns */

   if (!item) return;

   cr = gtk_print_context_get_cairo_context (context);
   pw = gtk_print_context_get_width (context);
   ph = gtk_print_context_get_height (context);

   /* file name under the image */
   if (job->options.print_name) {
      PangoFontDescription *font;
      gchar *base, *name;
      gint lw, lh;

      base = g_path_get_basename (gimv_image_info_get_path (item->info));
      name = charset_to_internal (base,
                                  conf.charset_filename,
                                  conf.charset_auto_detect_fn,
                                  conf.charset_filename_mode);
      layout = gtk_print_context_create_pango_layout (context);
      font = pango_font_description_from_string (CAPTION_FONT);
      pango_layout_set_font_description (layout, font);
      pango_font_description_free (font);
      pango_layout_set_width (layout, (gint) (pw * PANGO_SCALE));
      pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_MIDDLE);
      pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
      pango_layout_set_text (layout, name ? name : base, -1);
      pango_layout_get_size (layout, &lw, &lh);
      caption_h = (gdouble) lh / PANGO_SCALE + CAPTION_GAP;
      pango_layout_set_width (layout, -1);
      if ((gdouble) lw / PANGO_SCALE > pw)
         pango_layout_set_width (layout, (gint) (pw * PANGO_SCALE));
      g_free (name);
      g_free (base);
   }
   area_h = ph - caption_h;
   if (area_h < 1.0) area_h = ph;

   image = load_image (item->info);
   surface = image ? surface_from_image (image) : NULL;
   if (image) gimv_image_unref (image);

   if (!surface) {
      /* say so on the page instead of printing an empty sheet silently */
      PangoLayout *msg = gtk_print_context_create_pango_layout (context);
      gchar *text = g_strdup_printf (_("Cannot load %s"),
                                     gimv_image_info_get_path (item->info));
      pango_layout_set_width (msg, (gint) (pw * PANGO_SCALE));
      pango_layout_set_text (msg, text, -1);
      cairo_set_source_rgb (cr, 0, 0, 0);
      cairo_move_to (cr, 0, 0);
      pango_cairo_show_layout (cr, msg);
      g_object_unref (msg);
      g_free (text);
      if (layout) g_object_unref (layout);
      return;
   }

   iw = cairo_image_surface_get_width (surface);
   ih = cairo_image_surface_get_height (surface);

   /* rotation of the image view (ROTATE_90 is counterclockwise) */
   switch (item->rotate) {
   case GIMV_IMAGE_VIEW_ROTATE_90:  quarter = 3; break;
   case GIMV_IMAGE_VIEW_ROTATE_180: quarter = 2; break;
   case GIMV_IMAGE_VIEW_ROTATE_270: quarter = 1; break;
   default:                         quarter = 0; break;
   }

   w = (quarter % 2) ? ih : iw;
   h = (quarter % 2) ? iw : ih;

   /* turn the image to the orientation of the page */
   if (job->options.auto_rotate
       && ((w > h && pw < area_h) || (w < h && pw > area_h)))
   {
      gdouble tmp;
      quarter = (quarter + 1) % 4;
      tmp = w; w = h; h = tmp;
   }

   /* size in points */
   if (job->options.fit) {
      scale = MIN (pw / w, area_h / h);
   } else {
      scale = 72.0 / (job->options.dpi > 0 ? job->options.dpi : DEFAULT_DPI);
      scale = MIN (scale, MIN (pw / w, area_h / h));   /* not off the page */
   }

   cairo_save (cr);
   cairo_translate (cr, pw / 2.0, (area_h - h * scale) / 2.0 + h * scale / 2.0);
   cairo_rotate (cr, quarter * G_PI / 2.0);
   cairo_scale (cr, scale, scale);
   cairo_set_source_surface (cr, surface, -iw / 2.0, -ih / 2.0);
   cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_GOOD);
   cairo_rectangle (cr, -iw / 2.0, -ih / 2.0, iw, ih);
   cairo_fill (cr);
   cairo_restore (cr);

   if (layout) {
      gdouble img_bottom = (area_h - h * scale) / 2.0 + h * scale;
      draw_caption (context, cr, layout, pw, img_bottom + CAPTION_GAP);
      g_object_unref (layout);
   }

   cairo_surface_destroy (surface);
}


/******************************************************************************
 *
 *   "Image" tab of the print dialog
 *
 ******************************************************************************/
static void
cb_size_toggled (GtkCheckButton *button, PrintJob *job)
{
   gtk_widget_set_sensitive (job->dpi_spin,
                             gtk_check_button_get_active (GTK_CHECK_BUTTON (job->dpi_radio)));
}


static GObject *
cb_create_custom_widget (GtkPrintOperation *op, PrintJob *job)
{
   GtkWidget *vbox, *frame, *box, *hbox, *label;

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
   gtk_widget_set_margin_start  (vbox, 12);
   gtk_widget_set_margin_end    (vbox, 12);
   gtk_widget_set_margin_top    (vbox, 12);
   gtk_widget_set_margin_bottom (vbox, 12);

   /* size */
   frame = gtk_frame_new (_("Size"));
   gtk_box_append (GTK_BOX (vbox), frame);
   box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start  (box, 8);
   gtk_widget_set_margin_end    (box, 8);
   gtk_widget_set_margin_top    (box, 6);
   gtk_widget_set_margin_bottom (box, 6);
   gtk_frame_set_child (GTK_FRAME (frame), box);

   job->fit_radio = gtk_check_button_new_with_mnemonic (_("_Fit to the page"));
   gtk_box_append (GTK_BOX (box), job->fit_radio);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append (GTK_BOX (box), hbox);
   job->dpi_radio = gtk_check_button_new_with_mnemonic (_("_Resolution:"));
   gtk_check_button_set_group (GTK_CHECK_BUTTON (job->dpi_radio),
                               GTK_CHECK_BUTTON (job->fit_radio));
   gtk_box_append (GTK_BOX (hbox), job->dpi_radio);
   job->dpi_spin = gtk_spin_button_new_with_range (10, 2400, 10);
   gtk_spin_button_set_value (GTK_SPIN_BUTTON (job->dpi_spin), job->options.dpi);
   gtk_box_append (GTK_BOX (hbox), job->dpi_spin);
   label = gtk_label_new (_("dpi (at most the page size)"));
   gtk_box_append (GTK_BOX (hbox), label);

   if (job->options.fit)
      gtk_check_button_set_active (GTK_CHECK_BUTTON (job->fit_radio), TRUE);
   else
      gtk_check_button_set_active (GTK_CHECK_BUTTON (job->dpi_radio), TRUE);
   gtk_widget_set_sensitive (job->dpi_spin, !job->options.fit);
   g_signal_connect (job->dpi_radio, "toggled",
                     G_CALLBACK (cb_size_toggled), job);

   /* others */
   job->rotate_check
      = gtk_check_button_new_with_mnemonic (_("Turn the image to the _orientation of the page"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (job->rotate_check),
                                job->options.auto_rotate);
   gtk_box_append (GTK_BOX (vbox), job->rotate_check);

   job->name_check
      = gtk_check_button_new_with_mnemonic (_("Print the file _name"));
   gtk_check_button_set_active (GTK_CHECK_BUTTON (job->name_check),
                                job->options.print_name);
   gtk_box_append (GTK_BOX (vbox), job->name_check);

   return G_OBJECT (vbox);
}


static void
cb_custom_widget_apply (GtkPrintOperation *op, GtkWidget *widget, PrintJob *job)
{
   job->options.fit
      = gtk_check_button_get_active (GTK_CHECK_BUTTON (job->fit_radio));
   job->options.dpi
      = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (job->dpi_spin));
   job->options.auto_rotate
      = gtk_check_button_get_active (GTK_CHECK_BUTTON (job->rotate_check));
   job->options.print_name
      = gtk_check_button_get_active (GTK_CHECK_BUTTON (job->name_check));
}


/******************************************************************************
 *
 *   run
 *
 ******************************************************************************/
static void
cb_done (GtkPrintOperation *op, GtkPrintOperationResult result, PrintJob *job)
{
   if (result == GTK_PRINT_OPERATION_RESULT_ERROR) {
      GError *error = NULL;
      gtk_print_operation_get_error (op, &error);
      gtkutil_message_dialog (_("Print"),
                              error ? error->message : _("Printing failed."),
                              job->parent);
      g_clear_error (&error);
   } else if (result == GTK_PRINT_OPERATION_RESULT_APPLY) {
      g_set_object (&print_settings, gtk_print_operation_get_print_settings (op));
      g_set_object (&page_setup, gtk_print_operation_get_default_page_setup (op));
      print_options = job->options;
      save_settings ();
   }

   print_job_free (job);
   g_object_unref (op);
}


static void
print_job_run (PrintJob *job)
{
   GtkPrintOperation *op;
   GtkPrintOperationResult result;
   GError *error = NULL;
   gint n;

   n = g_list_length (job->items);
   if (n < 1) {
      gtkutil_message_dialog (_("Print"), _("There are no images to print."),
                              job->parent);
      print_job_free (job);
      return;
   }

   op = gtk_print_operation_new ();
   gtk_print_operation_set_n_pages (op, n);
   gtk_print_operation_set_unit (op, GTK_UNIT_POINTS);
   gtk_print_operation_set_allow_async (op, TRUE);
   gtk_print_operation_set_embed_page_setup (op, TRUE);
   gtk_print_operation_set_custom_tab_label (op, _("Image"));

   if (n == 1) {
      PrintItem *item = job->items->data;
      gchar *base = g_path_get_basename (gimv_image_info_get_path (item->info));
      gtk_print_operation_set_job_name (op, base);
      g_free (base);
   } else {
      gtk_print_operation_set_job_name (op, GIMV_PROG_NAME);
   }

   if (print_settings)
      gtk_print_operation_set_print_settings (op, print_settings);
   if (page_setup)
      gtk_print_operation_set_default_page_setup (op, page_setup);

   g_signal_connect (op, "draw-page", G_CALLBACK (cb_draw_page), job);
   g_signal_connect (op, "create-custom-widget",
                     G_CALLBACK (cb_create_custom_widget), job);
   g_signal_connect (op, "custom-widget-apply",
                     G_CALLBACK (cb_custom_widget_apply), job);
   g_signal_connect (op, "done", G_CALLBACK (cb_done), job);

   result = gtk_print_operation_run (op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                     job->parent, &error);

   /* errors are reported by cb_done (), which also frees the job */
   if (result == GTK_PRINT_OPERATION_RESULT_ERROR && error)
      g_warning ("print: %s", error->message);
   g_clear_error (&error);
}


static PrintJob *
print_job_new (GtkWindow *parent)
{
   PrintJob *job;

   load_settings ();

   job = g_new0 (PrintJob, 1);
   job->options = print_options;
   job->parent  = parent;
   if (parent)
      g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &job->parent);

   return job;
}


void
gimv_print_images (GList *infos, GtkWindow *parent)
{
   PrintJob *job;
   GList *node;

   job = print_job_new (parent);

   for (node = infos; node; node = g_list_next (node))
      print_job_add (job, node->data, GIMV_IMAGE_VIEW_ROTATE_0);

   print_job_run (job);
}


void
gimv_print_image_view (GimvImageView *iv, GtkWindow *parent)
{
   PrintJob *job;

   g_return_if_fail (GIMV_IS_IMAGE_VIEW (iv));

   job = print_job_new (parent);

   if (iv->info)
      print_job_add (job, iv->info, gimv_image_view_get_orientation (iv));

   print_job_run (job);
}
