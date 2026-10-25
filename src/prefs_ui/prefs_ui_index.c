/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView preferences: image index (dates and tags).
 *
 * The directories given here are watched by gimv-indexer while gimv runs;
 * their images can then be seen by date in the "Date" tab.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"
#include "gtkutils.h"
#include "gimv_index.h"
#include "gimv_index_view.h"
#include "gimv_prefs_ui_utils.h"
#include "gimv_prefs_win.h"
#include "gimv_thumb_win.h"
#include "prefs.h"
#include "prefs_ui_index.h"

extern Config *config_changed;
extern Config *config_prechanged;


static void
cb_enable_toggled (GtkWidget *toggle, GtkWidget *frame)
{
   config_changed->index_enable =
      gtk_check_button_get_active (GTK_CHECK_BUTTON (toggle));
   gtk_widget_set_sensitive (frame, config_changed->index_enable);
}


GtkWidget *
prefs_ui_index (void)
{
   GtkWidget *main_vbox, *toggle, *frame, *label;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);

   frame = gimv_prefs_ui_dir_list_prefs (_("Directories to index"),
                                         _("Select a directory to index"),
                                         config_prechanged->index_dirs,
                                         &config_changed->index_dirs,
                                         ';');
   toggle = gtkutil_create_check_button (_("Index images by date"),
                                         conf.index_enable,
                                         cb_enable_toggled,
                                         frame);
   gimv_box_pack_start (GTK_BOX (main_vbox), toggle, FALSE, FALSE, 0);

   label = gtk_label_new (_("The images below these directories are looked at "
                            "while GImageView runs, and can then be seen by "
                            "date in the \"Date\" tab."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_set_margin_top (label, 4);
   gtk_widget_set_margin_bottom (label, 4);
   gimv_box_pack_start (GTK_BOX (main_vbox), label, FALSE, FALSE, 0);

   gtk_widget_set_size_request (frame, -1, 200);
   gimv_box_pack_start (GTK_BOX (main_vbox), frame, FALSE, FALSE, 0);

   /* new images */
   {
      GtkWidget *nframe, *nvbox, *grid, *toggle2, *excl;
      static const struct {
         const gchar *label;
         gint         min, max;
         gsize        offset;
      } spins[] = {
         { N_("Gather new images for (seconds)"),   0, 3600, G_STRUCT_OFFSET (Config, index_notify_wait) },
         { N_("At most one notification every (seconds)"), 0, 3600, G_STRUCT_OFFSET (Config, index_notify_interval) },
         { N_("Show a notification for (seconds, 0: the default)"), 0, 600, G_STRUCT_OFFSET (Config, index_notify_timeout) },
      };
      guint i;

      gimv_prefs_ui_create_frame (_("New Images"), nframe, nvbox, main_vbox, FALSE);
      toggle2 = gtkutil_create_check_button (_("Notify new images on the desktop"),
                                             conf.index_notify,
                                             gtkutil_get_data_from_toggle_cb,
                                             &config_changed->index_notify);
      gimv_box_pack_start (GTK_BOX (nvbox), toggle2, FALSE, FALSE, 0);

      grid = gtk_grid_new ();
      gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
      gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
      gtk_widget_set_margin_start (grid, 20);
      for (i = 0; i < G_N_ELEMENTS (spins); i++) {
         gint *val = G_STRUCT_MEMBER_P (&conf, spins[i].offset);
         GtkAdjustment *adj = gtk_adjustment_new (*val, spins[i].min, spins[i].max, 1, 10, 0);
         GtkWidget *l = gtk_label_new (_(spins[i].label)), *spin;

         gtk_label_set_xalign (GTK_LABEL (l), 0.0);
         spin = gtkutil_create_spin_button (adj);
         g_signal_connect (adj, "value_changed",
                           G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                           G_STRUCT_MEMBER_P (config_changed, spins[i].offset));
         gtk_grid_attach (GTK_GRID (grid), l, 0, i, 1, 1);
         gtk_grid_attach (GTK_GRID (grid), spin, 1, i, 1, 1);
      }
      gimv_box_pack_start (GTK_BOX (nvbox), grid, FALSE, FALSE, 0);

      label = gtk_label_new (_("Where the notifications appear is set in the "
                               "desktop's notification settings."));
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_wrap (GTK_LABEL (label), TRUE);
      gtk_widget_add_css_class (label, "dim-label");
      gtk_widget_set_margin_start (label, 20);
      gimv_box_pack_start (GTK_BOX (nvbox), label, FALSE, FALSE, 0);

      excl = gimv_prefs_ui_dir_list_prefs (_("Folders not to notify"),
                                           _("Select a folder not to notify"),
                                           config_prechanged->index_notify_exclude,
                                           &config_changed->index_notify_exclude,
                                           ';');
      gtk_widget_set_size_request (excl, -1, 160);
      gimv_box_pack_start (GTK_BOX (nvbox), excl, FALSE, FALSE, 0);
      (void) nframe;
   }

   /* tags put by rules */
   {
      GtkWidget *aframe, *avbox, *l, *t;
      static const struct {
         const gchar *label;
         gsize        offset;
      } kinds[] = {
         { N_("The folder name (\"2019_Kyoto\", ...)"),     G_STRUCT_OFFSET (Config, index_auto_folder) },
         { N_("The camera (EXIF)"),                             G_STRUCT_OFFSET (Config, index_auto_camera) },
         { N_("The shape (portrait, landscape, square)"),       G_STRUCT_OFFSET (Config, index_auto_shape) },
      };
      guint i;

      gimv_prefs_ui_create_frame (_("Automatic Tags"), aframe, avbox, main_vbox, FALSE);
      l = gtk_label_new (_("Tags put by the index itself, shown in groups in the "
                           "\"Tag\" tab.  They are kept only in the index, not in "
                           "the comment files."));
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gimv_box_pack_start (GTK_BOX (avbox), l, FALSE, FALSE, 0);
      for (i = 0; i < G_N_ELEMENTS (kinds); i++) {
         gboolean *val = G_STRUCT_MEMBER_P (&conf, kinds[i].offset);
         t = gtkutil_create_check_button (_(kinds[i].label), *val,
                                          gtkutil_get_data_from_toggle_cb,
                                          G_STRUCT_MEMBER_P (config_changed, kinds[i].offset));
         gimv_box_pack_start (GTK_BOX (avbox), t, FALSE, FALSE, 0);
      }
      l = gtk_label_new (_("Turning the camera or the shape on reads every image "
                           "again, which takes long with many images."));
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gtk_widget_add_css_class (l, "dim-label");
      gimv_box_pack_start (GTK_BOX (avbox), l, FALSE, FALSE, 0);
      (void) aframe;
   }

   /* missing images */
   {
      GtkWidget *mframe, *mvbox, *hbox, *l, *spin;
      GtkAdjustment *adj;
      gchar *dir = g_build_filename (g_get_user_data_dir (), "gimageview", NULL), *text;

      gimv_prefs_ui_create_frame (_("Missing Images"), mframe, mvbox, main_vbox, FALSE);
      text = g_strdup_printf (_("Images with comments (tags) that are gone are listed "
                                "in a log in %s, one log for each start."), dir);
      l = gtk_label_new (text);
      g_free (text);
      g_free (dir);
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gimv_box_pack_start (GTK_BOX (mvbox), l, FALSE, FALSE, 0);

      hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
      l = gtk_label_new (_("Logs kept (0: none)"));
      adj = gtk_adjustment_new (conf.index_log_keep, 0, 1000, 1, 10, 0);
      spin = gtkutil_create_spin_button (adj);
      g_signal_connect (adj, "value_changed",
                        G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                        &config_changed->index_log_keep);
      gtk_box_append (GTK_BOX (hbox), l);
      gtk_box_append (GTK_BOX (hbox), spin);
      gimv_box_pack_start (GTK_BOX (mvbox), hbox, FALSE, FALSE, 0);
      (void) mframe;
   }

   gimv_widget_show_all (main_vbox);
   gtk_widget_set_sensitive (frame, conf.index_enable);

   /* taller than the other pages: let it scroll instead of making the
    * preferences window grow off the screen */
   {
      GtkWidget *scroll = gtk_scrolled_window_new ();
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                      GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scroll), TRUE);
      gtk_widget_set_vexpand (scroll, TRUE);
      gtk_widget_set_size_request (scroll, -1, 420);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), main_vbox);
      return scroll;
   }
}


gboolean
prefs_ui_index_apply (GimvPrefsWinAction action)
{
   Config *src, *dest;
   GList *node;
   gboolean configured;

   switch (action) {
   case GIMV_PREFS_WIN_ACTION_OK:
   case GIMV_PREFS_WIN_ACTION_APPLY:
      src  = config_prechanged;
      dest = config_changed;
      break;
   default:
      src  = config_changed;
      dest = config_prechanged;
      break;
   }

   if (dest->index_enable == src->index_enable
       && dest->index_log_keep == src->index_log_keep
       && dest->index_auto_folder == src->index_auto_folder
       && dest->index_auto_camera == src->index_auto_camera
       && dest->index_auto_shape  == src->index_auto_shape
       && !g_strcmp0 (dest->index_dirs, src->index_dirs))
      return FALSE;

   /* conf has the new values here */
   gimv_index_restart ();

   /* the "Date" and "Tag" tabs open as soon as directories are given */
   configured = gimv_index_is_configured ();
   for (node = gimv_thumb_win_get_list (); node; node = g_list_next (node)) {
      GimvThumbWin *tw = node->data;
      if (configured) {
         gimv_index_tab_open (tw, GIMV_INDEX_TAB_DATE);
         gimv_index_tab_open (tw, GIMV_INDEX_TAB_TAG);
      } else {
         gimv_index_tab_close (tw, GIMV_INDEX_TAB_DATE);
         gimv_index_tab_close (tw, GIMV_INDEX_TAB_TAG);
      }
      gimv_index_tab_update_menu (tw);
   }

   return FALSE;
}
