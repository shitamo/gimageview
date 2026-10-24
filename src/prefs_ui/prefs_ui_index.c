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

   gimv_box_pack_start (GTK_BOX (main_vbox), frame, TRUE, TRUE, 0);
   gimv_widget_show_all (main_vbox);
   gtk_widget_set_sensitive (frame, conf.index_enable);

   return main_vbox;
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
       && !g_strcmp0 (dest->index_dirs, src->index_dirs))
      return FALSE;

   /* conf has the new values here */
   gimv_index_restart ();

   /* the "Date" tab opens as soon as directories are given */
   configured = gimv_index_is_configured ();
   for (node = gimv_thumb_win_get_list (); node; node = g_list_next (node)) {
      GimvThumbWin *tw = node->data;
      if (configured)
         gimv_index_tab_open (tw);
      else
         gimv_index_tab_close (tw);
   }

   return FALSE;
}
