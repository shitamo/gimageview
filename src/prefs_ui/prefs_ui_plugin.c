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
 * $Id: prefs_ui_plugin.c,v 1.4 2004/09/22 15:37:12 makeinu Exp $
 */

#include <string.h>

#include "gimageview.h"

#include "charset.h"
#include "gtkutils.h"
#include "gimv_plugin.h"
#include "prefs.h"
#include "prefs_ui_plugin.h"
#include "gimv_prefs_ui_utils.h"
#include "gimv_prefs_win.h"


extern Config   *config_changed;
extern Config   *config_prechanged;


static void
cb_plugin_use_default_dir_list(GtkWidget *button, GtkWidget *widget)
{
   g_return_if_fail (GTK_IS_WIDGET(widget));

   if (gimv_toggle_get_active (button)) {
      config_changed->plugin_use_default_search_dir_list = TRUE;
      gtk_widget_hide (widget);
   } else {
      config_changed->plugin_use_default_search_dir_list = FALSE;
      gtk_widget_show (widget);
   }
}


/*******************************************************************************
 *  prefs_plugin_page:
 *     @ Create plugin preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_ui_plugin (void)
{
   GtkWidget *main_vbox, *toggle, *frame;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);

   frame = gimv_prefs_ui_dir_list_prefs (_("Directories list to search plugins"),
                                         _("Select plugin directory"),
                                         config_prechanged->plugin_search_dir_list,
                                         &config_changed->plugin_search_dir_list,
                                         ',');
   toggle = gtkutil_create_check_button (_("Use default directories list to search plugins"),
                                         conf.plugin_use_default_search_dir_list,
                                         cb_plugin_use_default_dir_list,
                                         frame);

   gimv_box_pack_start(GTK_BOX (main_vbox), toggle, FALSE, FALSE, 0);
   gimv_box_pack_start(GTK_BOX (main_vbox), frame, TRUE, TRUE, 0);

   gtk_widget_show (toggle);
   gimv_widget_show_all (frame);
   gtk_widget_show (main_vbox);

   if (config_changed->plugin_use_default_search_dir_list)
      gtk_widget_hide (frame);
   else
      gtk_widget_show (frame);

   return main_vbox;
}



static GtkWidget *
create_plugin_admin_widget (GList *plugin_list)
{
   GtkWidget *hbox, *scrollwin;
   GtkWidget *clist;
   GList *list;
   gint i;
   gchar *titles[] = {N_("Plugin Name"), N_("Version"), N_("Module Name")};
   gint titles_num = sizeof (titles)/ sizeof (gchar *);
   const gchar *text[32];

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);

   scrollwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrollwin), TRUE);
   gimv_container_set_border_width (GTK_WIDGET (scrollwin), 5);
   gimv_box_pack_start (GTK_BOX (hbox), scrollwin, TRUE, TRUE, 0);
   gimv_widget_set_size (scrollwin, -1, 200);

   {
      GtkListStore *store;

      store = gtk_list_store_new (titles_num,
                                  G_TYPE_STRING,
                                  G_TYPE_STRING,
                                  G_TYPE_STRING);
      clist = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
      gimv_tree_view_widen_column_resize (GTK_TREE_VIEW (clist));
      /* GTK4: gtk_tree_view_set_rules_hint () removed (theme decides) */
      gimv_container_add (GTK_WIDGET (scrollwin), clist);

      /* set columns */
      for (i = 0; i < titles_num; i++) {
         GtkTreeViewColumn *col;
         GtkCellRenderer *render;

         col = gtk_tree_view_column_new();
         gtk_tree_view_column_set_resizable (col, TRUE);
         gtk_tree_view_column_set_title (col, _(titles[i]));
         if (i == 0) {
            gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
            gtk_tree_view_column_set_fixed_width (col, 200);
         }
         render = gtk_cell_renderer_text_new ();
         gtk_tree_view_column_pack_start (col, render, TRUE);
         gtk_tree_view_column_add_attribute (col, render, "text", i);
         gtk_tree_view_append_column (GTK_TREE_VIEW (clist), col);
      }

      for (list = plugin_list; list; list = g_list_next (list)) {
         GModule *module = list->data;
         GtkTreeIter iter;

         text[0] = _(gimv_plugin_get_name (module));
         text[1] = _(gimv_plugin_get_version_string (module));
         text[2] = _(gimv_plugin_get_module_name (module));

         gtk_list_store_append (store, &iter);
         gtk_list_store_set (store, &iter,
                             0, text[0],
                             1, text[1],
                             2, text[2],
                             -1);
      }
   }

   return hbox;
}


#define PREFS_PLUGIN_CREATE_ADMIN_PAGE(title, plugin_type)                      \
{                                                                               \
   GtkWidget *main_vbox, *frame, *frame_vbox, *hbox;                            \
   GList *list;                                                                 \
                                                                                \
   main_vbox = gimv_vbox_new (FALSE, 0);                                         \
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);                 \
                                                                                \
   gimv_prefs_ui_create_frame (title,                                           \
                               frame, frame_vbox, main_vbox, TRUE);             \
                                                                                \
   /* clist */                                                                  \
   list = gimv_plugin_get_list (plugin_type);                                   \
   hbox = create_plugin_admin_widget (list);                                    \
   gimv_box_pack_start (GTK_BOX (frame_vbox), hbox, TRUE, TRUE, 0);              \
                                                                                \
   gimv_widget_show_all (main_vbox);                                             \
                                                                                \
   return main_vbox;                                                            \
}


GtkWidget *
prefs_ui_plugin_image_loader (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Image Loader Plugins"),
                                  GIMV_PLUGIN_IMAGE_LOADER);
}
GtkWidget *
prefs_ui_plugin_image_saver (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Image Saver Plugins"),
                                  GIMV_PLUGIN_IMAGE_SAVER);
}
GtkWidget *
prefs_ui_plugin_io_stream (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("IO Stream Plugins"),
                                  GIMV_PLUGIN_IO_STREAMER);
}
GtkWidget *
prefs_ui_plugin_archiver (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Archiver Plugins"),
                                  GIMV_PLUGIN_EXT_ARCHIVER);
}
GtkWidget *
prefs_ui_plugin_thumbnail (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Thumbnail Cache Plugins"),
                                  GIMV_PLUGIN_THUMB_CACHE);
}
GtkWidget *
prefs_ui_plugin_imageview (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Image View Embedder Plugins"),
                                  GIMV_PLUGIN_IMAGEVIEW_EMBEDER);
}
GtkWidget *
prefs_ui_plugin_thumbview (void)
{
   PREFS_PLUGIN_CREATE_ADMIN_PAGE(_("Thumbnail View Embedder Plugins"),
                                  GIMV_PLUGIN_THUMBVIEW_EMBEDER);
}


gboolean
prefs_ui_plugin_apply (GimvPrefsWinAction action)
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

   if ((dest->plugin_use_default_search_dir_list
        != src->plugin_use_default_search_dir_list) ||
       (dest->plugin_search_dir_list
        != src->plugin_search_dir_list))
   {
      gimv_plugin_create_search_dir_list ();
   }

   return FALSE;
}
