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
#include "gimv_index_faces.h"
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


/* a page: let it scroll instead of making the preferences window grow
 * off the screen */
static GtkWidget *
page_new (GtkWidget **vbox)
{
   GtkWidget *scroll = gtk_scrolled_window_new ();

   *vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (*vbox), 5);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), *vbox);
   return scroll;
}


static GtkWidget *
note_label (const gchar *text, gboolean dim)
{
   GtkWidget *l = gtk_label_new (text);

   gtk_label_set_xalign (GTK_LABEL (l), 0.0);
   gtk_label_set_wrap (GTK_LABEL (l), TRUE);
   gtk_widget_set_margin_bottom (l, 4);
   if (dim) gtk_widget_add_css_class (l, "dim-label");
   return l;
}


GtkWidget *
prefs_ui_index (void)
{
   GtkWidget *page, *main_vbox, *toggle, *frame, *label;

   page = page_new (&main_vbox);

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

   gtk_widget_set_size_request (frame, -1, 240);
   gimv_box_pack_start (GTK_BOX (main_vbox), frame, FALSE, FALSE, 0);

   gimv_box_pack_start (GTK_BOX (main_vbox),
                        note_label (_("New images, automatic tags, faces and missing images "
                                      "are set in the pages below this one."), TRUE),
                        FALSE, FALSE, 0);

   gimv_widget_show_all (main_vbox);
   gtk_widget_set_sensitive (frame, conf.index_enable);
   return page;
}


GtkWidget *
prefs_ui_index_newimg (void)
{
   GtkWidget *page, *main_vbox, *label;

   page = page_new (&main_vbox);
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
   gimv_widget_show_all (main_vbox);
   return page;
}


GtkWidget *
prefs_ui_index_auto (void)
{
   GtkWidget *page, *main_vbox;

   page = page_new (&main_vbox);
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

   /* folder names not made tags */
   {
      GtkWidget *xframe, *xvbox, *list;

      gimv_prefs_ui_create_frame (_("Folder Names Not Made Tags"), xframe, xvbox, main_vbox, FALSE);
      gimv_box_pack_start (GTK_BOX (xvbox),
                           note_label (_("Folders with these names give no folder tag "
                                         "(\"*\" matches any letters, \"?\" one; capitals and "
                                         "small letters are the same).  Names of numbers only "
                                         "and \"DCIM\" are never made tags.  A name can also be "
                                         "added from the menu of the folder in the \"Tag\" tab."),
                                       FALSE),
                           FALSE, FALSE, 0);
      list = gimv_prefs_ui_text_list_prefs (NULL, _("Name to add: "),
                                            config_prechanged->index_auto_folder_exclude,
                                            &config_changed->index_auto_folder_exclude,
                                            ';');
      gtk_widget_set_size_request (list, -1, 180);
      gimv_box_pack_start (GTK_BOX (xvbox), list, FALSE, FALSE, 0);
      (void) xframe;
   }
   gimv_widget_show_all (main_vbox);
   return page;
}


GtkWidget *
prefs_ui_index_faces (void)
{
   GtkWidget *page, *main_vbox;

   page = page_new (&main_vbox);
   if (!gimv_index_faces_available ()) {
      gimv_box_pack_start (GTK_BOX (main_vbox),
                           note_label (_("GImageView was built without finding faces."), FALSE),
                           FALSE, FALSE, 0);
   } else {
      GtkWidget *fframe, *fvbox, *l, *dirs;
      gchar *models = gimv_index_faces_model_dir (), *udir, *text;

      gimv_prefs_ui_create_frame (_("Faces"), fframe, fvbox, main_vbox, FALSE);
      l = gtk_label_new (_("Faces are looked for in the images of these folders (photos, "
                           "for example), and faces that look alike are put in groups. "
                           "The folders must be in the index. It runs after the rest of "
                           "the index, slowly, while GImageView runs."));
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gimv_box_pack_start (GTK_BOX (fvbox), l, FALSE, FALSE, 0);

      dirs = gimv_prefs_ui_dir_list_prefs (_("Folders to find faces in"),
                                           _("Select a folder to find faces in"),
                                           config_prechanged->index_face_dirs,
                                           &config_changed->index_face_dirs,
                                           ';');
      gtk_widget_set_size_request (dirs, -1, 160);
      gimv_box_pack_start (GTK_BOX (fvbox), dirs, FALSE, FALSE, 0);

      if (models) {
         text = g_strdup_printf (_("The model files: %s"), models);
      } else {
         udir = gimv_index_faces_user_model_dir ();
         text = g_strdup_printf (
            _("The model files (OpenCV YuNet and SFace, about 40 MB) are not found. "
              "Download these two into %s:\n"
              "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx\n"
              "https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx"),
            udir);
         g_free (udir);
      }
      l = gtk_label_new (text);
      g_free (text);
      g_free (models);
      gtk_label_set_xalign (GTK_LABEL (l), 0.0);
      gtk_label_set_wrap (GTK_LABEL (l), TRUE);
      gtk_label_set_wrap_mode (GTK_LABEL (l), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_selectable (GTK_LABEL (l), TRUE);
      gtk_widget_add_css_class (l, "dim-label");
      gimv_box_pack_start (GTK_BOX (fvbox), l, FALSE, FALSE, 0);
      (void) fframe;
   }
   gimv_widget_show_all (main_vbox);
   return page;
}


GtkWidget *
prefs_ui_index_missing (void)
{
   GtkWidget *page, *main_vbox;

   page = page_new (&main_vbox);
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
   return page;
}


/* the pages all have this one: what is to be done is gathered and done
 * once, after them */
static guint    apply_id = 0;
static gboolean apply_index = FALSE, apply_faces = FALSE;

static gboolean
apply_idle (gpointer data)
{
   GList *node;

   apply_id = 0;
   /* conf has the new values here */
   if (apply_index)
      gimv_index_restart ();
   else if (apply_faces)
      gimv_index_faces_restart ();
   if (apply_index || apply_faces) {
      /* the "Date" and "Tag" tabs open as soon as directories are given */
      for (node = gimv_thumb_win_get_list (); node; node = g_list_next (node))
         gimv_index_tabs_sync (node->data);
   }
   apply_index = apply_faces = FALSE;
   return G_SOURCE_REMOVE;
}


gboolean
prefs_ui_index_apply (GimvPrefsWinAction action)
{
   Config *src, *dest;

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

   if (dest->index_enable != src->index_enable
       || dest->index_log_keep != src->index_log_keep
       || dest->index_auto_folder != src->index_auto_folder
       || dest->index_auto_camera != src->index_auto_camera
       || dest->index_auto_shape  != src->index_auto_shape
       || g_strcmp0 (dest->index_auto_folder_exclude ? dest->index_auto_folder_exclude : "",
                     src->index_auto_folder_exclude ? src->index_auto_folder_exclude : "")
       || g_strcmp0 (dest->index_dirs, src->index_dirs))
      apply_index = TRUE;
   /* only the folders of the faces: the indexer goes on */
   else if (g_strcmp0 (dest->index_face_dirs, src->index_face_dirs))
      apply_faces = TRUE;

   if ((apply_index || apply_faces) && !apply_id)
      apply_id = g_idle_add (apply_idle, NULL);

   return FALSE;
}
