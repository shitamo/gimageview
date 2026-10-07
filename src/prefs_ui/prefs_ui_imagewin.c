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
 * $Id: prefs_ui_imagewin.c,v 1.8 2004/09/29 03:45:56 makeinu Exp $
 */

#include <string.h>

#include "gimageview.h"

#include "gimv_image.h"
#include "gtk2-compat.h"
#include "gtkutils.h"
#include "menu.h"
#include "prefs.h"
#include "gimv_prefs_ui_utils.h"
#include "gimv_prefs_win.h"
#include "prefs_ui_imagewin.h"
#include "gimv_image_win.h"


typedef struct PrefsWin_Tag {
   GtkWidget *imgwin_width_spin;
   GtkWidget *imgwin_height_spin;
   GtkWidget *imgwin_menubar_toggle;
   GtkWidget *imgwin_toolbar_toggle;
   GtkWidget *imgwin_player_toggle;
   GtkWidget *imgwin_statusbar_toggle;
   GtkWidget *imgwin_scrollbar_toggle;
   GtkWidget *imgwin_player_label;
   GtkWidget *imgwin_player_radio[3];
   GtkWidget *image_scale_spin;
} PrefsWin;


static PrefsWin prefs_win;
extern Config   *config_changed;
extern Config   *config_prechanged;


const gchar *rotate_menu_items[] = {
   N_("0 degrees"),
   N_("90 degrees counterclockwise"),
   N_("180 degrees"),
   N_("90 degrees clockwise"),
   N_("Do not change"),
#ifdef ENABLE_EXIF
   N_("Automatic EXIF"),
#endif
   NULL
};

const gchar *zoom_menu_items[] = {
   N_("Default image scale"),
   N_("Do not change"),
   N_("Fit image to window"),
   N_("Fit image to window (zoom out only)"),
   N_("Fit image to width"),
   N_("Fit image to height"),
   NULL
};

static void
set_sensitive_imgwin_state (void)
{
   GtkWidget *widgets[] = {
      prefs_win.imgwin_width_spin,
      prefs_win.imgwin_height_spin,
      prefs_win.imgwin_menubar_toggle,
      prefs_win.imgwin_toolbar_toggle,
      prefs_win.imgwin_player_toggle,
      prefs_win.imgwin_statusbar_toggle,
      prefs_win.imgwin_scrollbar_toggle,
      prefs_win.imgwin_player_label,
      prefs_win.imgwin_player_radio[0],
      prefs_win.imgwin_player_radio[1],
      prefs_win.imgwin_player_radio[2],
   };
   gint i, num = sizeof (widgets) / sizeof (widgets[0]);

   for (i = 0; i < num; i++)
      gtk_widget_set_sensitive (widgets[i],
                                !config_changed->imgwin_save_win_state);
}


static void
cb_save_imgwin_state (GtkWidget *toggle)
{
   config_changed->imgwin_save_win_state =
      gimv_toggle_get_active (GTK_WIDGET (toggle));

   set_sensitive_imgwin_state ();
}


static void
cb_zoom_menu (GtkWidget *menu)
{
   config_changed->imgview_default_zoom =
      GPOINTER_TO_INT (g_object_get_data(G_OBJECT (menu), "num"));

   gtk_widget_set_sensitive (prefs_win.image_scale_spin,
                             config_changed->imgview_default_zoom == 0 ||
                             config_changed->imgview_default_zoom == 1);
}


static void
cb_player_visible (GtkWidget *radio, gpointer data)
{
   gint idx = GPOINTER_TO_INT (data);

   if (!gimv_toggle_get_active (GTK_WIDGET (radio))) return;

   config_changed->imgview_player_visible = idx;
}


static const gchar *imageview_mouse_items[] = {
   N_("None"),
   N_("Next image"),
   N_("Previous image"),
   N_("Popup menu"),
   N_("Zoom in"),
   N_("Zoom out"),
   N_("Fit image size to frame"),
   N_("Rotate CCW"),
   N_("Rotate CW"),
   N_("Open the navigation window"),
   N_("Scroll up"),
   N_("Scroll down"),
   N_("Scroll left"),
   N_("Scroll right"),
   NULL
};


/*******************************************************************************
 *  prefs_imagewin_page:
 *     @ Create Image Window preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_imagewin_page (void)
{
   GtkWidget *main_vbox, *frame, *hbox, *hbox2, *hbox3, *vbox;
   GtkWidget *label, *button, *spinner, *toggle, *radio[3];
   GtkAdjustment *adj;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);

   /**********************************************
    * Window Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Window"), frame, vbox, main_vbox, FALSE);

   /* Save window state on exit */
   toggle = gtkutil_create_check_button (_("Save window state on exit"),
                                         conf.imgwin_save_win_state,
                                         cb_save_imgwin_state,
                                         &config_changed->imgwin_save_win_state);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Window width and height spinner */
   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   label = gtk_label_new (_("Initial window size: "));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   label = gtk_label_new (_("width"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.imgwin_width,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   prefs_win.imgwin_width_spin = spinner;
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->imgwin_width);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   label = gtk_label_new (_("height"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.imgwin_height,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   prefs_win.imgwin_height_spin = spinner;
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->imgwin_height);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   /* Auto resize window to image size */
   toggle = gtkutil_create_check_button (_("Auto resize to image size"),
                                         conf.imgwin_fit_to_image,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_fit_to_image);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Open New Window or not */
   toggle = gtkutil_create_check_button (_("Open each file in separate window"),
                                         conf.imgwin_open_new_win,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_open_new_win);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Raise window or not */
   toggle = gtkutil_create_check_button (_("Raise the window when opening an image in the shared window"),
                                         conf.imgwin_raise_window,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_raise_window);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /********************************************** 
    * Show/Hide Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Show/Hide"), frame, vbox, main_vbox, FALSE);

   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   hbox2 = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox2), 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox2, FALSE, FALSE, 0);

   hbox3 = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox3), 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox3, FALSE, FALSE, 0);

   /* Show Menubar or not */
   toggle = gtkutil_create_check_button (_("Menubar"),
                                         conf.imgwin_show_menubar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_show_menubar);
   prefs_win.imgwin_menubar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Toolbar or not */
   toggle = gtkutil_create_check_button (_("Toolbar"),
                                         conf.imgwin_show_toolbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_show_toolbar);
   prefs_win.imgwin_toolbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Player toolbar or not */
   toggle = gtkutil_create_check_button (_("Slideshow player"),
                                         conf.imgwin_show_player,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_show_player);
   prefs_win.imgwin_player_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Statusbar or not */
   toggle = gtkutil_create_check_button (_("Statusbar"),
                                         conf.imgwin_show_statusbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_show_statusbar);
   prefs_win.imgwin_statusbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* Show Scrollbar or not */
   toggle = gtkutil_create_check_button (_("Scrollbar"),
                                         conf.imgview_scrollbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgview_scrollbar);
   prefs_win.imgwin_scrollbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* player */
   label = gtk_label_new (_("Player toolbar:"));
   gimv_box_pack_start (GTK_BOX (hbox3), label, FALSE, FALSE, 0);
   prefs_win.imgwin_player_label = label;

   radio[0] = gimv_radio_button_new_with_label (NULL, _("Show"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[0], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[0]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleShow));
   prefs_win.imgwin_player_radio[0] = radio[0];

   radio[1] = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio[0]), _("Hide"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[1], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[1]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleHide));
   prefs_win.imgwin_player_radio[1] = radio[1];

   radio[2] = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio[1]), _("Auto"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[2], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[2]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleAuto));
   prefs_win.imgwin_player_radio[2] = radio[2];

   switch (config_changed->imgview_player_visible) {
   case GimvImageViewPlayerVisibleShow:
      gimv_toggle_set_active (GTK_WIDGET (radio[0]), TRUE);
      break;
   case GimvImageViewPlayerVisibleHide:
      gimv_toggle_set_active (GTK_WIDGET (radio[1]), TRUE);
      break;
   case GimvImageViewPlayerVisibleAuto:
   default:
      gimv_toggle_set_active (GTK_WIDGET (radio[2]), TRUE);
      break;
   }

   /********************************************** 
    * Back Ground Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Background"), frame, vbox, main_vbox, FALSE);

   /* back ground color of normal window */
   hbox = gimv_hbox_new (FALSE, 5);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   toggle = gtkutil_create_check_button (_("Specify the background color"),
                                         conf.imgwin_set_bg,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_set_bg);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   button = gtkutil_color_sel_button (_("Choose Color"),
                                      config_changed->imgwin_bg_color);
   gimv_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);

   /* transparency */
   toggle = gtkutil_create_check_button (_("Show a checkerboard behind transparent parts of images"),
                                         conf.imgview_alpha_checker,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgview_alpha_checker);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* back ground color of fullscreen window */
   hbox = gimv_hbox_new (FALSE, 5);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   toggle = gtkutil_create_check_button (_("Specify the background color in fullscreen"),
                                         conf.imgwin_fullscreen_set_bg,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgwin_fullscreen_set_bg);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   button = gtkutil_color_sel_button (_("Choose Color"),
                                      config_changed->imgwin_fullscreen_bg_color);
   gimv_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);

   set_sensitive_imgwin_state ();

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


GtkWidget *
prefs_imagewin_image_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *frame;
   GtkWidget *hbox, *vbox;
   GtkWidget *label;
   GtkWidget *option_menu;
   GtkAdjustment *adj;
   GtkWidget *spinner;
   GtkWidget *toggle;
   GtkWidget *table, *alignment;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);

   /********************************************** 
    * Image Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Image"), frame, vbox, main_vbox, FALSE);

   alignment = gimv_alignment_new (0.0, 0.5, 0.0, 0.0);
   gimv_box_pack_start (GTK_BOX (vbox), alignment, FALSE, FALSE, 0);

   table = gimv_table_new (2, 2, FALSE);
   gimv_container_add (GTK_WIDGET (alignment), table);

   /* Default zoom action menu */
   label = gtk_label_new (_("Zoom:"));
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), label);
   gimv_table_attach (GTK_WIDGET (table), alignment, 0, 1, 0, 1, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   option_menu = create_option_menu (zoom_menu_items,
                                     conf.imgview_default_zoom,
                                     cb_zoom_menu, NULL);
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), option_menu);
   gimv_table_attach (GTK_WIDGET (table), alignment, 1, 2, 0, 1, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   /* Rotate on image change */
   label = gtk_label_new (_("Rotation:"));
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), label);
   gimv_table_attach (GTK_WIDGET (table), alignment, 0, 1, 1, 2, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   option_menu = create_option_menu_simple (rotate_menu_items,
                                            conf.imgview_default_rotation,
                                            &config_changed->imgview_default_rotation);
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), option_menu);
   gimv_table_attach (GTK_WIDGET (table), alignment, 1, 2, 1, 2, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   /* remember the rotation of each image (in its comment file) */
   toggle = gtkutil_create_check_button (_("Remember the rotation of each image (saved with its comment)"),
                                         conf.imgview_remember_rotation,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgview_remember_rotation);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Keep Aspect Ratio */
   toggle = gtkutil_create_check_button (_("Keep aspect ratio"),
                                         conf.imgview_keep_aspect,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgview_keep_aspect);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Default Image Scale Spinner */
   hbox = gimv_hbox_new (FALSE, 5);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new (_("Default Image Scale"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.imgview_scale,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gimv_widget_set_size(spinner, 50, -1);
   prefs_win.image_scale_spin = spinner;
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_float_cb),
                       &config_changed->imgview_scale);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);
   label = gtk_label_new (_("%"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   /* keep original image on memory or not */
   toggle = gtkutil_create_check_button (_("Keep the original image in memory"),
                                         conf.imgview_buffer,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->imgview_buffer);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

  
   gtk_widget_set_sensitive (prefs_win.image_scale_spin,
                             conf.imgview_default_zoom == 0 ||
                             conf.imgview_default_zoom == 1);

   /* show all */
   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


/*******************************************************************************
 *  prefs_imagewin_mouse_page:
 *     @ Create mouse button action on image window preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_imagewin_mouse_page (void)
{
   GtkWidget *vbox;

   vbox = gimv_prefs_ui_mouse_prefs (imageview_mouse_items,
                                     conf.imgview_mouse_button,
                                     &config_changed->imgview_mouse_button);

   gimv_widget_show_all (vbox);

   return vbox;
}



gboolean
prefs_ui_imagewin_image_apply (GimvPrefsWinAction action)
{
   GimvImageWin *iw;
   GList *node;
   Config *dest;

   switch (action) {
   case GIMV_PREFS_WIN_ACTION_OK:
   case GIMV_PREFS_WIN_ACTION_APPLY:
      dest = config_changed;
      break;
   default:
      dest = config_prechanged;
      break;
   }

   for (node = gimv_image_win_get_list(); node; node = g_list_next (node)) {
      iw = node->data;

      if (iw->iv) {
         if (!dest->imgview_default_zoom) {
            g_object_set (G_OBJECT (iw->iv),
                          "x_scale", dest->imgview_scale,
                          "y_scale", dest->imgview_scale,
                          NULL);
         }
         g_object_set (G_OBJECT (iw->iv),
                       "default_zoom",     dest->imgview_default_zoom,
                       "default_rotation", dest->imgview_default_rotation,
                       NULL);
      }
   }

   return FALSE;
}
