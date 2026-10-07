/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2003 Takuro Ashie
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
 * $Id: prefs_ui_movie.c,v 1.3 2003/07/06 16:46:21 makeinu Exp $
 */

#include "prefs_ui_movie.h"

#include <string.h>
#include "gimv_image_view.h"
#include "gtkutils.h"
#include "menu.h"
#include "prefs.h"

extern Config   *config_changed;
extern Config   *config_prechanged;

static const gchar **movie_view_modes = NULL;
static gint movie_view_modes_len = 0;


static void
cb_movie_view_mode (GtkWidget *widget, gpointer data)
{
   gint idx;

   idx = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "num"));
   g_return_if_fail (idx >=0 && idx < movie_view_modes_len);

   if (config_changed->movie_default_view_mode != config_prechanged->movie_default_view_mode)
      g_free (config_changed->movie_default_view_mode);
   config_changed->movie_default_view_mode = g_strdup (movie_view_modes[idx]);
}


GtkWidget *
prefs_movie_page (void)
{
   GtkWidget *main_vbox, *hbox, *vbox;
   GtkWidget *label, *option_menu;
   gint i, defval = 0;

   main_vbox = gimv_vbox_new (FALSE, 0);

   /* create label array */
   if (!movie_view_modes) {
      GList *node, *list = gimv_image_view_plugin_get_list();

      movie_view_modes_len = g_list_length(list);
      if (movie_view_modes_len > 1) {
         movie_view_modes = g_new0(const gchar *, movie_view_modes_len + 1);
         for (i = 0, node = list;
              i < movie_view_modes_len && node;
              i++, node = g_list_next (node))
         {
            GimvImageViewPlugin *plugin = node->data;
            if (!plugin || !plugin->label || !*plugin->label) continue;
            movie_view_modes[i] = g_strdup(plugin->label);
         }
         movie_view_modes[movie_view_modes_len] = NULL;
      }
   }

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (main_vbox), hbox, FALSE, FALSE, 0);
   gtk_widget_show (hbox);

   if (movie_view_modes_len > 1) {
      vbox = gimv_vbox_new (FALSE, 0);
      gimv_container_set_border_width (GTK_WIDGET (vbox), 0);
      gimv_box_pack_start (GTK_BOX (hbox), vbox, FALSE, FALSE, 0);
      gtk_widget_show (vbox);

      label = gtk_label_new (_("Default view mode for movie and audio: "));
      gimv_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 2);
      gtk_widget_show (label);

      for (i = 0; conf.movie_default_view_mode && movie_view_modes[i]; i++) {
         if (!strcmp (conf.movie_default_view_mode, movie_view_modes[i])) {
            defval = i;
            break;
         }
      }

      option_menu = create_option_menu (movie_view_modes, defval,
                                        cb_movie_view_mode, NULL);
      gimv_box_pack_start (GTK_BOX (vbox), option_menu, FALSE, FALSE, 2);
      gtk_widget_show (option_menu);

   } else {
      label = gtk_label_new (_("No movie plugins are available."));
      gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 2);
      gtk_widget_show (label);
   }

   /* continuous play */
   {
      GtkWidget *toggle;

      toggle = gtkutil_create_check_button (_("Play the next file when a movie or audio file ends (continuous play)"),
                                            conf.imgview_movie_continuance,
                                            gtkutil_get_data_from_toggle_cb,
                                            &config_changed->imgview_movie_continuance);
      gimv_container_set_border_width (GTK_WIDGET (toggle), 5);
      gimv_box_pack_start (GTK_BOX (main_vbox), toggle, FALSE, FALSE, 0);

      /* GTK2 versions: the movie player took the clicks itself */
      toggle = gtkutil_create_check_button (_("Ignore mouse actions for next/previous image, zoom, rotation and scrolling while a movie or audio file is playing"),
                                            conf.imgview_movie_lock_mouse,
                                            gtkutil_get_data_from_toggle_cb,
                                            &config_changed->imgview_movie_lock_mouse);
      gimv_container_set_border_width (GTK_WIDGET (toggle), 5);
      gimv_box_pack_start (GTK_BOX (main_vbox), toggle, FALSE, FALSE, 0);
   }

   return main_vbox;
}


/* the open views follow the setting (their Movie > Continuous Play too) */
gboolean
prefs_movie_apply (GimvPrefsWinAction action)
{
   Config *src;
   GList *node;

   switch (action) {
   case GIMV_PREFS_WIN_ACTION_OK:
   case GIMV_PREFS_WIN_ACTION_APPLY:
      src = config_changed;
      break;
   default:
      src = config_prechanged;
      break;
   }

   /* not changed here: keep what each view's menu says */
   if (config_changed->imgview_movie_continuance
       == config_prechanged->imgview_movie_continuance)
      return FALSE;

   for (node = gimv_image_view_get_list (); node; node = g_list_next (node))
      gimv_image_view_set_continuance (GIMV_IMAGE_VIEW (node->data),
                                       src->imgview_movie_continuance);

   return FALSE;
}
