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
 * $Id: prefs_ui_thumbwin.c,v 1.15 2004/09/29 03:45:56 makeinu Exp $
 */

#include <string.h>

#include "gimageview.h"

#include "dirview.h"
#include "gimv_prefs_ui_utils.h"
#include "gimv_prefs_win.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"
#include "gtkutils.h"
#include "gtk2-compat.h"
#include "menu.h"
#include "prefs.h"
#include "prefs_ui_thumbwin.h"


typedef struct PrefsWin_Tag {
   /* thumbnail window */
   GtkWidget *thumbwin_width_spin;
   GtkWidget *thumbwin_height_spin;
   GtkWidget *thumbwin_layout;
   GtkWidget *thumbwin_dirview_toggle;
   GtkWidget *thumbwin_preview_toggle;
   GtkWidget *thumbwin_preview_tab_toggle;
   GtkWidget *thumbwin_menubar_toggle;
   GtkWidget *thumbwin_toolbar_toggle;
   GtkWidget *thumbwin_statusbar_toggle;
   GtkWidget *thumbwin_tab_toggle;
   GtkWidget *thumbwin_player_label;
   GtkWidget *thumbwin_player_radio[3];
   GtkWidget *thumbwin_tab_pos;

   /* thumbnail view */
   GtkWidget *thumbview_display_mode;
   GtkWidget *thumbview_thumb_size;

   /* Directory View */
   GtkWidget *dirview_show_current_dir;
   GtkWidget *dirview_show_parent_dir;

   /* Preview */
   GtkWidget *preview_fit_only_zoom_out;
   GtkWidget *preview_scale_spin;
   GtkWidget *preview_scrollbar_toggle;
} PrefsWin;

static PrefsWin prefs_win = {
   /* thumbnail window */
   thumbwin_width_spin:         NULL,
   thumbwin_height_spin:        NULL,
   thumbwin_layout:             NULL,
   thumbwin_dirview_toggle:     NULL,
   thumbwin_preview_toggle:     NULL,
   thumbwin_preview_tab_toggle: NULL,
   thumbwin_menubar_toggle:     NULL,
   thumbwin_toolbar_toggle:     NULL,
   thumbwin_statusbar_toggle:   NULL,
   thumbwin_tab_toggle:         NULL,
   thumbwin_tab_pos:            NULL,

   /* thumbnail view */
   thumbview_display_mode:      NULL,
   thumbview_thumb_size:        NULL,

   /* Directory View */
   dirview_show_current_dir:    NULL,
   dirview_show_parent_dir:     NULL,

   /* Preview */
   preview_fit_only_zoom_out:   NULL,
   preview_scale_spin:          NULL,
   preview_scrollbar_toggle:    NULL,
};


static void set_sensitive_thumbwin     (void);
static void set_sensitive_thumbview    (void);
static void set_sensitive_preview      (void);
static void set_sensitive_thumbwin_tab (void);


static const gchar *layout_items[] = {
   N_("Layout0"),
   N_("Layout1"),
   N_("Layout2"),
   N_("Layout3"),
   N_("Layout4"),
   N_("Custom"),
   NULL
};


static const gchar *toolbar_style_items[] = {
   N_("Icon"),
   N_("Text"),
   N_("Both"),
   NULL
};


static const gchar *tab_pos_items[] = {
   N_("Left"),
   N_("Right"),
   N_("Top"),
   N_("Bottom"),
   NULL
};




static const gchar *thumbview_mouse_items[] = {
   N_("None"),
   N_("Popup menu"),
   N_("Open (Auto select window)"),
   N_("Open (Auto & Force)"),
   N_("Open in preview"),
   N_("Open in preview (Force)"),
   N_("Open in new window"),
   N_("Open in shared window"),
   N_("Open in shared window (Force)"),
   NULL
};


static const gchar *dirview_mouse_items[] = {
   N_("None"),
   N_("Load thumbnails"),
   N_("Load thumbnails recursively"),
   N_("Popup menu"),
   N_("Change top directory"),
   N_("Load recursively in a tab"),
   NULL
};


static const gchar *preview_mouse_items[] = {
   N_("None"),
   N_("Next image"),
   N_("Previous image"),
   N_("Open in shared window"),
   N_("Open in new window"),
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

/* next two imported from prefs_ui_imagewin.c */

extern const gchar *rotate_menu_items[];
extern const gchar *zoom_menu_items[];

extern Config   *config_changed;
extern Config   *config_prechanged;


static void
cb_thumbwin_page_destroy (GtkWidget *widget, gpointer data)
{
   prefs_win.thumbwin_width_spin         = NULL;
   prefs_win.thumbwin_height_spin        = NULL;
   prefs_win.thumbwin_layout             = NULL;
   prefs_win.thumbwin_dirview_toggle     = NULL;
   prefs_win.thumbwin_preview_toggle     = NULL;
   prefs_win.thumbwin_preview_tab_toggle = NULL;
   prefs_win.thumbwin_menubar_toggle     = NULL;
   prefs_win.thumbwin_toolbar_toggle     = NULL;
   prefs_win.thumbwin_statusbar_toggle   = NULL;
   prefs_win.thumbwin_tab_toggle         = NULL;
}


static void
cb_thumbwin_tab_page_destroy (GtkWidget *widget, gpointer data)
{
   prefs_win.thumbwin_tab_pos = NULL;  
}


static void
cb_thumbview_page_destroy (GtkWidget *widget, gpointer data)
{
   prefs_win.thumbview_display_mode = NULL;
   prefs_win.thumbview_thumb_size   = NULL;
}


static void
cb_dirview_page_destroy (GtkWidget *widget, gpointer data)
{
   prefs_win.dirview_show_current_dir = NULL;
   prefs_win.dirview_show_parent_dir  = NULL;
}


static void
cb_preview_page_destroy (GtkWidget *widget, gpointer data)
{
   prefs_win.preview_fit_only_zoom_out = NULL;
   prefs_win.preview_scale_spin        = NULL;
   prefs_win.preview_scrollbar_toggle  = NULL;
}


static void
cb_player_visible (GtkWidget *radio, gpointer data)
{
   gint idx = GPOINTER_TO_INT (data);

   if (!gimv_toggle_get_active (GTK_WIDGET (radio))) return;

   config_changed->preview_player_visible = idx;
}


static void
set_sensitive_thumbwin (void)
{
   GtkWidget *widgets[] = {
      prefs_win.thumbwin_width_spin,
      prefs_win.thumbwin_height_spin,
      prefs_win.thumbwin_layout,
      prefs_win.thumbwin_dirview_toggle,
      prefs_win.thumbwin_preview_toggle,
      prefs_win.thumbwin_preview_tab_toggle,
      prefs_win.thumbwin_menubar_toggle,
      prefs_win.thumbwin_toolbar_toggle,
      prefs_win.thumbwin_statusbar_toggle,
      prefs_win.thumbwin_tab_toggle,
      prefs_win.thumbwin_player_label,
      prefs_win.thumbwin_player_radio[0],
      prefs_win.thumbwin_player_radio[1],
      prefs_win.thumbwin_player_radio[2],
   };
   gint i, num = sizeof (widgets) / sizeof (widgets[0]);

   for (i = 0; i < num; i++) {
      if (widgets[i])
         gtk_widget_set_sensitive (widgets[i],
                                   !config_changed->thumbwin_save_win_state);
   }

   set_sensitive_thumbview    ();
   set_sensitive_preview      ();
   set_sensitive_thumbwin_tab ();
}


static void
set_sensitive_thumbwin_tab (void)
{
   if (prefs_win.thumbwin_tab_pos)
      gtk_widget_set_sensitive (prefs_win.thumbwin_tab_pos,
                                !config_changed->thumbwin_save_win_state);
}


static void
set_sensitive_thumbview (void)
{
   GtkWidget *widgets[] = {
      prefs_win.thumbview_display_mode,
      prefs_win.thumbview_thumb_size,
   };
   gint i, num = sizeof (widgets) / sizeof (widgets[0]);

   for (i = 0; i < num; i++) {
      if (widgets[i])
         gtk_widget_set_sensitive (widgets[i],
                                   !config_changed->thumbwin_save_win_state);
   }
}


static void
set_sensitive_preview (void)
{
   if (prefs_win.preview_scrollbar_toggle)
      gtk_widget_set_sensitive (prefs_win.preview_scrollbar_toggle,
                                !config_changed->thumbwin_save_win_state);
   if (prefs_win.preview_scale_spin)
      gtk_widget_set_sensitive (prefs_win.preview_scale_spin,
                                config_changed->preview_zoom == 0 ||
                                config_changed->preview_zoom == 1);
}


static void
cb_save_thumbwin_state (GtkWidget *toggle)
{
   config_changed->thumbwin_save_win_state =
      gimv_toggle_get_active (GTK_WIDGET (toggle));

   set_sensitive_thumbwin ();
}


static void
cb_default_disp_mode (GtkWidget *widget)
{
   const gchar *tmpstr;
   gint disp_mode;

   disp_mode = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "num"));
   tmpstr = gimv_thumb_view_num_to_label (disp_mode);
   if (tmpstr)
      config_changed->thumbwin_disp_mode = (gchar *) tmpstr;
   else
      config_changed->thumbwin_disp_mode = GIMV_THUMB_VIEW_DEFAULT_SUMMARY_MODE;
}


static void
cb_zoom_menu (GtkWidget *menu)
{
   config_changed->preview_zoom =
      GPOINTER_TO_INT (g_object_get_data(G_OBJECT (menu), "num"));
   set_sensitive_preview();
}



/*******************************************************************************
 *  prefs_thumbwin_page:
 *     @ Create Thumbnail View preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_thumbwin_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *frame, *vbox, *vbox2, *hbox, *hbox2, *hbox3;
   GtkWidget *label;
   GtkAdjustment *adj;
   GtkWidget *spinner;
   GtkWidget *option_menu;
   GtkWidget *toggle, *radio[3];

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);
   g_signal_connect (G_OBJECT (main_vbox), "destroy",
                       G_CALLBACK (cb_thumbwin_page_destroy), NULL);


   /**********************************************
    * Window Frame
    **********************************************/
   gimv_prefs_ui_create_frame (_("Window"), frame, vbox, main_vbox, FALSE);

   /* Save window state on exit */
   toggle = gtkutil_create_check_button (_("Save window state on exit"),
                                         conf.thumbwin_save_win_state,
                                         cb_save_thumbwin_state,
                                         &config_changed->thumbwin_save_win_state);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Window width and height spinner */
   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new (_("width"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.thumbwin_width,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   spinner = prefs_win.thumbwin_width_spin = gtkutil_create_spin_button (adj);
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->thumbwin_width);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   label = gtk_label_new (_("height"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.thumbwin_height,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   spinner = prefs_win.thumbwin_height_spin = gtkutil_create_spin_button (adj);
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->thumbwin_height);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   /* Toolbar Style */
   label = gtk_label_new (_("Default Layout"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 5);

   option_menu = create_option_menu_simple (layout_items,
                                            conf.thumbwin_layout_type,
                                            (gint *) &config_changed->thumbwin_layout_type);
   prefs_win.thumbwin_layout = option_menu;
   gimv_box_pack_start (GTK_BOX (hbox), option_menu, FALSE, FALSE, 0);

   /* Show/Hide frame */
   gimv_prefs_ui_create_frame (_("Show/Hide"), frame, vbox2, vbox, FALSE);

   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);
   gimv_box_pack_start (GTK_BOX (vbox2), hbox, FALSE, FALSE, 0);

   hbox2 = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox2), 0);
   gimv_box_pack_start (GTK_BOX (vbox2), hbox2, FALSE, FALSE, 0);

   hbox3 = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox3), 0);
   gimv_box_pack_start (GTK_BOX (vbox2), hbox3, FALSE, FALSE, 0);

   /* Show Directory or not */
   toggle = gtkutil_create_check_button (_("Directory view"),
                                         conf.thumbwin_show_dir_view,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_dir_view);
   prefs_win.thumbwin_dirview_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Preview or not */
   toggle = gtkutil_create_check_button (_("Preview"),
                                         conf.thumbwin_show_preview,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_preview);
   prefs_win.thumbwin_preview_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Preview Tab or not */
   toggle = gtkutil_create_check_button (_("Preview Tab"),
                                         conf.thumbwin_show_preview_tab,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_preview_tab);
   prefs_win.thumbwin_preview_tab_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 0);

   /* Show Menubar or not */
   toggle = gtkutil_create_check_button (_("Menubar"),
                                         conf.thumbwin_show_menubar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_menubar);
   prefs_win.thumbwin_menubar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* Show Toolbar or not */
   toggle = gtkutil_create_check_button (_("Toolbar"),
                                         conf.thumbwin_show_toolbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_toolbar);
   prefs_win.thumbwin_toolbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* Show Statusbar or not */
   toggle = gtkutil_create_check_button (_("Statusbar"),
                                         conf.thumbwin_show_statusbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_statusbar);
   prefs_win.thumbwin_statusbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* Show Tab or not */
   toggle = gtkutil_create_check_button (_("Tab"),
                                         conf.thumbwin_show_tab,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_tab);
   prefs_win.thumbwin_tab_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (hbox2), toggle, FALSE, FALSE, 0);

   /* player */
   label = gtk_label_new (_("Player toolbar:"));
   gimv_box_pack_start (GTK_BOX (hbox3), label, FALSE, FALSE, 0);
   prefs_win.thumbwin_player_label = label;

   radio[0] = gimv_radio_button_new_with_label (NULL, _("Show"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[0], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[0]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleShow));
   prefs_win.thumbwin_player_radio[0] = radio[0];

   radio[1] = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio[0]), _("Hide"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[1], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[1]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleHide));
   prefs_win.thumbwin_player_radio[1] = radio[1];

   radio[2] = gimv_radio_button_new_with_label_from_widget (GTK_WIDGET (radio[1]), _("Auto"));
   gimv_box_pack_start (GTK_BOX (hbox3), radio[2], FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (radio[2]), "toggled",
                       G_CALLBACK (cb_player_visible),
                       GINT_TO_POINTER(GimvImageViewPlayerVisibleAuto));
   prefs_win.thumbwin_player_radio[2] = radio[2];

   switch (config_changed->preview_player_visible) {
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

   /* Raise window or not */
   toggle = gtkutil_create_check_button (_("Raise window when opening thumbnails"),
                                         conf.thumbwin_raise_window,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_raise_window);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   /* Toolbar Style */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   label = gtk_label_new (_("Toolbar Style"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 5);

   option_menu = create_option_menu_simple (toolbar_style_items,
                                            conf.thumbwin_toolbar_style,
                                            (gint *) &config_changed->thumbwin_toolbar_style);
   gimv_box_pack_start (GTK_BOX (hbox), option_menu, FALSE, FALSE, 5);


   /* "Loading" frame */
   gimv_prefs_ui_create_frame (_("Loading"), frame, vbox, main_vbox, FALSE);

   /* GUI redraw interval */
   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   label = gtk_label_new (_("GUI redraw interval while loading: Every"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.thumbwin_redraw_interval,
                                               1.0, 1000.0, 1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->thumbwin_redraw_interval);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);

   label = gtk_label_new (_("files"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   /* show detail of progress */
   toggle = gtkutil_create_check_button (_("Show detail of loading progress"),
                                         conf.thumbwin_show_progress_detail,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_progress_detail);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);


   /* set sensitive */
   set_sensitive_thumbwin ();

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


GtkWidget *
prefs_thumbview_toolbar_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *label;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);

   label = gtk_label_new (_("Not implemented yet."));
   gimv_box_pack_start (GTK_BOX (main_vbox), label, FALSE, FALSE, 5);

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


/*******************************************************************************
 *  prefs_thumbwin_tab_page:
 *     @ Create Thumbnail View preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_thumbwin_tab_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *frame, *vbox, *hbox;
   GtkWidget *label;
   GtkWidget *option_menu;
   GtkWidget *toggle;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);
   g_signal_connect (G_OBJECT (main_vbox), "destroy",
                       G_CALLBACK (cb_thumbwin_tab_page_destroy), NULL);


   /**********************************************
    * Tab Frame
    **********************************************/
   gimv_prefs_ui_create_frame (_("Tab"), frame, vbox, main_vbox, FALSE);

   /* Tab Position Selection */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new (_("Tab Position"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 5);

   option_menu = create_option_menu_simple (tab_pos_items,
                                            conf.thumbwin_tab_pos,
                                            (gint *) &config_changed->thumbwin_tab_pos);
   gimv_box_pack_start (GTK_BOX (hbox), option_menu, FALSE, FALSE, 5);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   prefs_win.thumbwin_tab_pos = option_menu;

   /* Move to new tab automatically or not */
   toggle = gtkutil_create_check_button (_("Move to new tab automatically"),
                                         conf.thumbwin_move_to_newtab,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_move_to_newtab);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);

   /* show tab close button */
   toggle = gtkutil_create_check_button (_("Show close button"),
                                         conf.thumbwin_show_tab_close,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_show_tab_close);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   /* show full path in tab */
   toggle = gtkutil_create_check_button (_("Show full path"),
                                         conf.thumbwin_tab_fullpath,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_tab_fullpath);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   /* force open tab */
   toggle = gtkutil_create_check_button (_("Open a new tab even if the directory has no images"),
                                         conf.thumbwin_force_open_tab,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbwin_force_open_tab);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);


   set_sensitive_thumbwin_tab ();

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


GtkWidget *
prefs_thumbview_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *frame, *vbox, *hbox;
   GtkWidget *toggle, *spinner, *label, *option_menu;
   GtkAdjustment *adj;
   gint num, disp_mode;
   gchar **disp_mode_labels;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);
   g_signal_connect (G_OBJECT (main_vbox), "destroy",
                       G_CALLBACK (cb_thumbview_page_destroy), NULL);


   /**********************************************
    * Show/Hide Frame
    **********************************************/
   gimv_prefs_ui_create_frame (_("Show/Hide"), frame, vbox, main_vbox, FALSE);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   /* show directory */
   toggle = gtkutil_create_check_button (_("Directory"),
                                         conf.thumbview_show_dir,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbview_show_dir);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);

   /* show archive */
   toggle = gtkutil_create_check_button (_("Archive"),
                                         conf.thumbview_show_archive,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->thumbview_show_archive);
   gimv_box_pack_start (GTK_BOX (hbox), toggle, FALSE, FALSE, 5);


   /* Default display mode */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (main_vbox), hbox, FALSE, FALSE, 0);

   disp_mode_labels = gimv_thumb_view_get_summary_mode_labels (&num);

   label = gtk_label_new (_("Default display mode"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 5);

   disp_mode = gimv_thumb_view_label_to_num (conf.thumbwin_disp_mode);
   option_menu = create_option_menu ((const gchar **)disp_mode_labels,
                                     disp_mode,
                                     cb_default_disp_mode,
                                     NULL);
   gimv_box_pack_start (GTK_BOX (hbox), option_menu, FALSE, FALSE, 5);
   prefs_win.thumbview_display_mode = option_menu;

   /* Thumbnail size spinner */
   hbox = gimv_hbox_new (FALSE, 10);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (main_vbox), hbox, FALSE, FALSE, 0);

   label = gtk_label_new (_("Thumbnail Size"));
   gimv_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);

   adj = (GtkAdjustment *) gtk_adjustment_new (conf.thumbwin_thumb_size,
                                               GIMV_THUMB_WIN_MIN_THUMB_SIZE,
                                               GIMV_THUMB_WIN_MAX_THUMB_SIZE,
                                               1.0, 5.0, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   prefs_win.thumbview_thumb_size = spinner;
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_int_cb),
                       &config_changed->thumbwin_thumb_size);
   gimv_box_pack_start(GTK_BOX(hbox), spinner, FALSE, TRUE, 0);


   set_sensitive_thumbview  ();

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


/*******************************************************************************
 *  prefs_thumbview_mouse_page:
 *     @ Create mouse button action on thumbnail view preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_thumbview_mouse_page (void)
{
   GtkWidget *vbox;

   vbox = gimv_prefs_ui_mouse_prefs (thumbview_mouse_items,
                                     conf.thumbview_mouse_button,
                                     &config_changed->thumbview_mouse_button);

   gimv_widget_show_all (vbox);

   return vbox;
}


/*******************************************************************************
 *  prefs_dirview_page:
 *     @ Create Directory View preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_dirview_page (void)
{
   GtkWidget *main_vbox;
   GtkWidget *frame, *vbox;
   GtkWidget *toggle;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);
   g_signal_connect (G_OBJECT (main_vbox), "destroy",
                       G_CALLBACK (cb_dirview_page_destroy), NULL);


   /**********************************************
    * Shoe/Hide Frame
    **********************************************/
   gimv_prefs_ui_create_frame (_("Show/Hide"), frame, vbox, main_vbox, FALSE);

   /* show/hide toolbar */
   toggle = gtkutil_create_check_button (_("Show toolbar"),
                                         conf.dirview_show_toolbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->dirview_show_toolbar);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* show/hide dot file */
   toggle = gtkutil_create_check_button (_("Show dotfiles"),
                                         conf.dirview_show_dotfile,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->dirview_show_dotfile);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* show/hide dot file */
   toggle = gtkutil_create_check_button (_("Show the \".\" directory even when dotfiles are hidden"),
                                         conf.dirview_show_current_dir,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->dirview_show_current_dir);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* show/hide dot file */
   toggle = gtkutil_create_check_button (_("Show the \"..\" directory even when dotfiles are hidden"),
                                         conf.dirview_show_parent_dir,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->dirview_show_parent_dir);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);



   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


/*******************************************************************************
 *  prefs_dirview_mouse_page:
 *     @ Create mouse button action on dirview preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_dirview_mouse_page (void)
{
   GtkWidget *vbox;

   vbox = gimv_prefs_ui_mouse_prefs (dirview_mouse_items,
                                     conf.dirview_mouse_button,
                                     &config_changed->dirview_mouse_button);

   gimv_widget_show_all (vbox);

   return vbox;
}


/*******************************************************************************
 *  prefs_preview_page:
 *     @ Create pewview preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_preview_page (void)
{
   GtkWidget *main_vbox, *frame, *vbox, *hbox, *table, *alignment;
   GtkWidget *spinner, *toggle, *label, *option_menu;
   GtkAdjustment *adj;

   main_vbox = gimv_vbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (main_vbox), 5);
   g_signal_connect (G_OBJECT (main_vbox), "destroy",
                       G_CALLBACK (cb_preview_page_destroy), NULL);


   /********************************************** 
    * Image Frame
    **********************************************/
   gimv_prefs_ui_create_frame (_("Image"), frame, vbox, main_vbox, FALSE);

   alignment = gimv_alignment_new (0.0, 0.5, 0.0, 0.0);
   gimv_box_pack_start (GTK_BOX (vbox), alignment, FALSE, FALSE, 0);

   table = gimv_table_new (2, 2, FALSE);
   gimv_container_add (GTK_WIDGET (alignment), table);

   /* Zoom menu */
   label = gtk_label_new (_("Zoom:"));
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), label);
   gimv_table_attach (GTK_WIDGET (table), alignment, 0, 1, 0, 1, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   option_menu = create_option_menu (zoom_menu_items,
                                     conf.preview_zoom,
                                     cb_zoom_menu, NULL);
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), option_menu);
   gimv_table_attach (GTK_WIDGET (table), alignment, 1, 2, 0, 1, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   /* Rotate menu */
   label = gtk_label_new (_("Rotation:"));
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), label);
   gimv_table_attach (GTK_WIDGET (table), alignment, 0, 1, 1, 2, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);

   option_menu = create_option_menu_simple (rotate_menu_items,
                                            conf.preview_rotation,
                                            &config_changed->preview_rotation);
   alignment = gimv_alignment_new(0.0, 0.5, 0.0, 0.0);
   gimv_container_add (GTK_WIDGET (alignment), option_menu);
   gimv_table_attach (GTK_WIDGET (table), alignment, 1, 2, 1, 2, GIMV_EXPAND | GIMV_FILL, GIMV_FILL, 5, 1);
#if 0
   gimv_box_pack_start (GTK_BOX (hbox), option_menu, FALSE, FALSE, 0);
#endif

   /* Keep Aspect Ratio */
   toggle = gtkutil_create_check_button (_("Keep aspect ratio"),
                                         conf.preview_keep_aspect,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->preview_keep_aspect);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* Default Image Scale Spinner */
   hbox = gimv_hbox_new (FALSE, 5);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   label = gtk_label_new (_("Default Image Scale"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   adj = (GtkAdjustment *) gtk_adjustment_new (conf.preview_scale,
                                               1.0, 10000.0, 1.0, 5.0, 0.0);
   prefs_win.preview_scale_spin = spinner = gtkutil_create_spin_button (adj);
   gimv_widget_set_size(spinner, 50, -1);
   g_signal_connect (G_OBJECT (adj), "value_changed",
                       G_CALLBACK (gtkutil_get_data_from_adjustment_by_float_cb),
                       &config_changed->preview_scale);
   gimv_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);
   label = gtk_label_new (_("%"));
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);

   /* Show Scrollbar or not */
   toggle = gtkutil_create_check_button (_("Show scrollbar"),
                                         conf.preview_scrollbar,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->preview_scrollbar);
   prefs_win.preview_scrollbar_toggle = toggle;
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   /* keep original image on memory or not */
   toggle = gtkutil_create_check_button (_("Keep the original image in memory"),
                                         conf.preview_buffer,
                                         gtkutil_get_data_from_toggle_cb,
                                         &config_changed->preview_buffer);
   gimv_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);

   set_sensitive_preview  ();

   gimv_widget_show_all (main_vbox);

   return main_vbox;
}


/*******************************************************************************
 *  prefs_preview_mouse_page:
 *     @ Create mouse button action on preview preference page.
 *
 *  Return : Packed widget (GtkVbox)
 *******************************************************************************/
GtkWidget *
prefs_preview_mouse_page (void)
{
   GtkWidget *vbox;

   vbox = gimv_prefs_ui_mouse_prefs (preview_mouse_items,
                                     conf.preview_mouse_button,
                                     &config_changed->preview_mouse_button);

   gimv_widget_show_all (vbox);

   return vbox;
}


gboolean
prefs_ui_thumbwin_apply (GimvPrefsWinAction action)
{
   GimvThumbWin *tw;
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

   for (node = gimv_thumb_win_get_list(); node; node = g_list_next (node)) {
      tw = node->data;
      gtkutil_toolbar_set_style (tw->toolbar,
                                 dest->thumbwin_toolbar_style);

      gtk_widget_queue_resize (tw->toolbar_handle);
   }

   return FALSE;
}


gboolean
prefs_ui_thumbwin_tab_apply (GimvPrefsWinAction action)
{
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

   /* tab label */
   for (node = gimv_thumb_view_get_list(); node; node = g_list_next (node)) {
      GimvThumbView *tv = node->data;
      gimv_thumb_view_reset_tab_label (tv, NULL);
   }

   return FALSE;
}


gboolean
prefs_ui_dirview_apply (GimvPrefsWinAction action)
{
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


   return FALSE;
}


gboolean
prefs_ui_preview_apply (GimvPrefsWinAction action)
{
   GimvThumbWin *tw;
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

   for (node = gimv_thumb_win_get_list(); node; node = g_list_next (node)) {
      tw = node->data;

      if (tw->iv) {
         if (dest->preview_zoom == 0) {
            g_object_set (G_OBJECT (tw->iv),
                          "x_scale", dest->preview_scale,
                          "y_scale", dest->preview_scale,
                          NULL);
         }
         g_object_set (G_OBJECT (tw->iv),
                       "default_zoom",       dest->preview_zoom,
                       "default_rotation",   dest->preview_rotation,
                       "keep_aspect",        dest->preview_keep_aspect,
                       "keep_buffer",        dest->preview_buffer,
                       "show_scrollbar",     dest->preview_scrollbar,
                       NULL);
      }
   }

   return FALSE;
}
