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
 * $Id: prefs_xine.c,v 1.4 2004/10/03 19:18:57 makeinu Exp $
 */

#include "prefs_xine.h"

#ifdef ENABLE_XINE

#include <stdlib.h>
#include "gtkutils.h"
#include "gimv_prefs_ui_utils.h"

#define CONF_THUMBNAIL_ENABLE_KEY "thumbnail_enable"
#define CONF_THUMBNAIL_ENABLE     "FALSE"
#define CONF_THUMBNAIL_POS_KEY    "thumbnail_pos"
#define CONF_THUMBNAIL_POS        "1.0"
#define CONF_THUMBNAIL_DELAY_KEY  "create_thumbnail_delay"
#define CONF_THUMBNAIL_DELAY      "3.0"

extern GimvPluginInfo *gimv_xine_plugin_get_info (void);
static GtkWidget      *prefs_xine_page           (void);
static gboolean        prefs_xine_apply          (GimvPrefsWinAction action);

static GimvPrefsWinPage gimv_prefs_page_xine =
{
   path:           N_("/Movie and Audio/Xine"),
   priority_hint:  0,
   icon:           NULL,
   icon_open:      NULL,
   create_page_fn: prefs_xine_page,
   apply_fn:       prefs_xine_apply,
};

typedef struct XineConf_Tag
{
#if 0
   gchar    *vo_driver;
   gchar    *ao_driver;
#endif
   gboolean  thumb;
   gfloat    thumb_pos;
   gfloat    delay;
} XineConf;

static XineConf xineconf, xineconf_pre;


gboolean
gimv_prefs_ui_xine_get_page (guint idx, GimvPrefsWinPage **page, guint *size)
{
   g_return_val_if_fail(page, FALSE);
   *page = NULL;
   g_return_val_if_fail(size, FALSE);
   *size = 0;

   if (idx == 0) {
      *page = &gimv_prefs_page_xine;
      *size = sizeof (gimv_prefs_page_xine);
      return TRUE;
   } else {
      return FALSE;
   }
}


gboolean
gimv_prefs_xine_get_thumb_enable (void)
{
   GimvPluginInfo *this = gimv_xine_plugin_get_info();
   gboolean enable = !strcasecmp("TRUE", CONF_THUMBNAIL_ENABLE) ? TRUE : FALSE;
   gboolean success;

   success = gimv_plugin_prefs_load_value (this->name,
                                           GIMV_PLUGIN_IMAGE_LOADER,
                                           CONF_THUMBNAIL_ENABLE_KEY,
                                           GIMV_PLUGIN_PREFS_BOOL,
                                           (gpointer) &enable);
   if (!success) {
      enable = !strcasecmp("TRUE", CONF_THUMBNAIL_ENABLE) ? TRUE : FALSE;
      gimv_plugin_prefs_save_value (this->name,
                                    GIMV_PLUGIN_IMAGE_LOADER,
                                    CONF_THUMBNAIL_ENABLE_KEY,
                                    CONF_THUMBNAIL_ENABLE);
   }

   return enable;
}


gfloat
gimv_prefs_xine_get_thumb_pos (void)
{
   GimvPluginInfo *this = gimv_xine_plugin_get_info();
   gfloat delay = atof (CONF_THUMBNAIL_POS);
   gboolean success;

   success = gimv_plugin_prefs_load_value (this->name,
                                           GIMV_PLUGIN_IMAGE_LOADER,
                                           CONF_THUMBNAIL_POS_KEY,
                                           GIMV_PLUGIN_PREFS_FLOAT,
                                           (gpointer) &delay);
   if (!success) {
      delay = atof (CONF_THUMBNAIL_POS);
      gimv_plugin_prefs_save_value (this->name,
                                    GIMV_PLUGIN_IMAGE_LOADER,
                                    CONF_THUMBNAIL_POS_KEY,
                                    CONF_THUMBNAIL_POS);
   }

   return delay;
}


gfloat
gimv_prefs_xine_get_delay (GimvPluginInfo *this)
{
   gfloat delay = atof (CONF_THUMBNAIL_DELAY);
   gboolean success;

   success = gimv_plugin_prefs_load_value (this->name,
                                           GIMV_PLUGIN_IMAGEVIEW_EMBEDER,
                                           CONF_THUMBNAIL_DELAY_KEY,
                                           GIMV_PLUGIN_PREFS_FLOAT,
                                           (gpointer) &delay);
   if (!success) {
      delay = atof (CONF_THUMBNAIL_DELAY);
      gimv_plugin_prefs_save_value (this->name,
                                    GIMV_PLUGIN_IMAGEVIEW_EMBEDER,
                                    CONF_THUMBNAIL_DELAY_KEY,
                                    CONF_THUMBNAIL_DELAY);
   }

   return delay;
}


static GtkWidget *
prefs_xine_page (void)
{
   GtkWidget *main_vbox, *frame, *vbox, *hbox, *table, *alignment;
   GtkWidget *label, *spinner, *toggle;
   GtkWidget *vo_combo, *ao_combo;
   GtkAdjustment *adj;
   GimvPluginInfo *this = gimv_xine_plugin_get_info ();

   main_vbox = gtk_vbox_new (FALSE, 0);

   xineconf.thumb = xineconf_pre.thumb
      = gimv_prefs_xine_get_thumb_enable();
   xineconf.thumb_pos = xineconf_pre.thumb_pos
      = gimv_prefs_xine_get_thumb_pos();
   xineconf.delay = xineconf_pre.delay
      = gimv_prefs_xine_get_delay (this);

   /**********************************************
    * Driver Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Driver (Not implemented yet)"),
                              frame, vbox, main_vbox, FALSE);

#if 0
   /* video driver combo */
   hbox = gtk_hbox_new (FALSE, 0);
   gtk_container_set_border_width(GTK_CONTAINER (hbox), 5);
   gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   gtk_widget_show (hbox);

   label = gtk_label_new (_("Video driver : "));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 2);
   gtk_widget_show (label);

   vo_combo = gtk_combo_new ();
   gtk_box_pack_start (GTK_BOX (hbox), vo_combo, FALSE, FALSE, 2);
   gtk_widget_show (vo_combo);
   gtk_widget_set_usize (vo_combo, 100, -1);

   /* audio driver combo */
   label = gtk_label_new (_("Audio driver : "));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 2);
   gtk_widget_show (label);

   ao_combo = gtk_combo_new ();
   gtk_box_pack_start (GTK_BOX (hbox), ao_combo, FALSE, FALSE, 2);
   gtk_widget_show (ao_combo);
   gtk_widget_set_usize (ao_combo, 100, -1);
#else
   alignment = gtk_alignment_new (0.0, 0.5, 0.0, 0.0);
   gtk_box_pack_start (GTK_BOX (vbox), alignment, FALSE, FALSE, 0);
   gtk_widget_show (alignment);

   table = gtk_table_new (2, 2, FALSE);
   gtk_container_add (GTK_CONTAINER (alignment), table);
   gtk_widget_show (table);

   /* video driver combo */
   label = gtk_label_new (_("Video driver : "));
   alignment = gtk_alignment_new(0.0, 0.5, 0.0, 0.0);
   gtk_container_add (GTK_CONTAINER (alignment), label);
   gtk_table_attach (GTK_TABLE (table), alignment, 0, 1, 0, 1,
                     GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 1);
   gtk_widget_show (alignment);
   gtk_widget_show (label);

   vo_combo = gtk_combo_new ();
   alignment = gtk_alignment_new(0.0, 0.5, 0.0, 0.0);
   gtk_container_add (GTK_CONTAINER (alignment), vo_combo);
   gtk_table_attach (GTK_TABLE (table), alignment, 1, 2, 0, 1,
                     GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 1);
   gtk_widget_show (alignment);
   gtk_widget_show (vo_combo);
   gtk_widget_set_usize (vo_combo, 100, -1);

   /* audio driver combo */
   label = gtk_label_new (_("Audio driver : "));
   alignment = gtk_alignment_new(0.0, 0.5, 0.0, 0.0);
   gtk_container_add (GTK_CONTAINER (alignment), label);
   gtk_table_attach (GTK_TABLE (table), alignment, 0, 1, 1, 2,
                     GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 1);
   gtk_widget_show (alignment);
   gtk_widget_show (label);

   ao_combo = gtk_combo_new ();
   alignment = gtk_alignment_new(0.0, 0.5, 0.0, 0.0);
   gtk_container_add (GTK_CONTAINER (alignment), ao_combo);
   gtk_table_attach (GTK_TABLE (table), alignment, 1, 2, 1, 2,
                     GTK_EXPAND | GTK_FILL, GTK_FILL, 5, 1);
   gtk_widget_show (alignment);
   gtk_widget_show (ao_combo);
   gtk_widget_set_usize (ao_combo, 100, -1);
#endif

   gtk_widget_set_sensitive (frame, FALSE);

   /**********************************************
    * Thumbnail Frame
    **********************************************/
   gimv_prefs_ui_create_frame(_("Thumbnail"), frame, vbox, main_vbox, FALSE);

#if 1
   /* use this feature or not */
   toggle = gtkutil_create_check_button (_("Enable creating thumbnail of movie using Xine"),
                                         xineconf.thumb,
                                         gtkutil_get_data_from_toggle_cb,
                                         &xineconf.thumb);
   gtk_container_set_border_width (GTK_CONTAINER(toggle), 5);
   gtk_box_pack_start (GTK_BOX (vbox), toggle, FALSE, FALSE, 0);
   gtk_widget_show (toggle);

   /* stream position */
   hbox = gtk_hbox_new (FALSE, 5);
   gtk_container_set_border_width (GTK_CONTAINER(hbox), 5);
   gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   gtk_widget_show (hbox);
   label = gtk_label_new (_("Stream position : "));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   gtk_widget_show (label);
   adj = (GtkAdjustment *) gtk_adjustment_new (xineconf.thumb_pos,
                                               0.0, 100.0, 0.01, 0.1, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gtk_widget_set_usize(spinner, 70, -1);
   gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spinner), 2);
   gtk_signal_connect (GTK_OBJECT (adj), "value_changed",
                       GTK_SIGNAL_FUNC (gtkutil_get_data_from_adjustment_by_float_cb),
                       &xineconf.thumb_pos);
   gtk_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);
   gtk_widget_show (spinner);

   label = gtk_label_new (_("[%]"));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   gtk_widget_show (label);
#endif

   /* delay */
   hbox = gtk_hbox_new (FALSE, 5);
   gtk_container_set_border_width (GTK_CONTAINER(hbox), 5);
   gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   gtk_widget_show (hbox);
   label = gtk_label_new (_("Delay time to create thumbnail from starting play : "));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   gtk_widget_show (label);
   adj = (GtkAdjustment *) gtk_adjustment_new (xineconf.delay,
                                               0.0, 7200.0, 0.01, 0.1, 0.0);
   spinner = gtkutil_create_spin_button (adj);
   gtk_widget_set_usize(spinner, 70, -1);
   gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spinner), 2);
   gtk_signal_connect (GTK_OBJECT (adj), "value_changed",
                       GTK_SIGNAL_FUNC (gtkutil_get_data_from_adjustment_by_float_cb),
                       &xineconf_pre.delay);
   gtk_box_pack_start (GTK_BOX (hbox), spinner, FALSE, FALSE, 0);
   gtk_widget_show (spinner);

   label = gtk_label_new (_("[sec]"));
   gtk_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   gtk_widget_show (label);

   return main_vbox;
}


static gboolean
prefs_xine_apply (GimvPrefsWinAction action)
{
   gchar delay_str[32];
   GimvPluginInfo *this = gimv_xine_plugin_get_info();
   gchar pos_str[32], *enable;

   switch (action) {
   case GIMV_PREFS_WIN_ACTION_OK:
   case GIMV_PREFS_WIN_ACTION_APPLY:
      enable = xineconf.thumb ? "TRUE" : "FALSE";
      g_snprintf(pos_str, 32, "%f", xineconf.thumb_pos);
      g_snprintf(delay_str, 32, "%f", xineconf.delay);
      break;
   default:
      enable = xineconf_pre.thumb ? "TRUE" : "FALSE";
      g_snprintf(pos_str, 32, "%f", xineconf_pre.thumb_pos);
      g_snprintf(delay_str, 32, "%f", xineconf_pre.delay);
      break;
   }

   gimv_plugin_prefs_save_value (this->name,
                                 GIMV_PLUGIN_IMAGE_LOADER,
                                 CONF_THUMBNAIL_ENABLE_KEY,
                                 enable);
   gimv_plugin_prefs_save_value (this->name,
                                 GIMV_PLUGIN_IMAGE_LOADER,
                                 CONF_THUMBNAIL_POS_KEY,
                                 pos_str);
   gimv_plugin_prefs_save_value (this->name,
                                 GIMV_PLUGIN_IMAGEVIEW_EMBEDER,
                                 CONF_THUMBNAIL_DELAY_KEY,
                                 delay_str);

   return FALSE;
}

#endif /* ENABLE_XINE */
