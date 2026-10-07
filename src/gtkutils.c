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
 * $Id: gtkutils.c,v 1.30 2004/10/03 14:53:55 makeinu Exp $
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"
#include "gimv_object.h"

#include "auto_completion.h"
#include "charset.h"
#include "fileutil.h"
#include "gtkutils.h"
#include "gimv_icon_stock.h"
#include "intl.h"
#include "prefs.h"

#ifndef BUF_SIZE
#define BUF_SIZE 4096
#endif

/* callback functions for confirm dialog */
static gint cb_dummy                 (GtkWidget   *button,
                                      gpointer     data);
static void cb_confirm_yes           (GtkWidget   *button,
                                      ConfirmType *type);
static void cb_confirm_yes_to_all    (GtkWidget   *button,
                                      ConfirmType *type);
static void cb_confirm_no            (GtkWidget   *button,
                                      ConfirmType *type);
static void cb_confirm_cancel        (GtkWidget   *button,
                                      ConfirmType *type);

/* callback functions for message dialog */
static void cb_message_dialog_quit   (GtkWidget   *button,
                                      gpointer     data);

/* callback functions for progress bar window */
static void cb_progress_win_cancel   (GtkWidget   *button,
                                      gboolean    *cancel_pressed);

/* callback functions for text entry window */
static void cb_textpop_enter         (GtkWidget   *button,
                                      gboolean    *ok_pressd);
static void cb_textpop_ok_button     (GtkWidget   *button,
                                      gboolean    *ok_pressd);
static void cb_textpop_cancel_button (GtkWidget   *button,
                                      gboolean    *ok_pressd);



/******************************************************************************
 *
 *   misc
 *
 ******************************************************************************/
const gchar *
boolean_to_text (gboolean boolval)
{
   if (boolval)
      return "TRUE";
   else
      return "FALSE";
}


gboolean
text_to_boolean (gchar *text)
{
   g_return_val_if_fail (text && *text, FALSE);

   if (!g_ascii_strcasecmp (text, "TRUE") || !g_ascii_strcasecmp (text, "ENABLE"))
      return TRUE;
   else
      return FALSE;
}


void
gtkutil_get_widget_area (GtkWidget    *widget,
                         GdkRectangle *area)
{
   g_return_if_fail (widget);
   g_return_if_fail (area);

   {
      graphene_rect_t bounds;
      GtkWidget *parent = gtk_widget_get_parent (widget);

      area->x = area->y = 0;
      if (parent && gtk_widget_compute_bounds (widget, parent, &bounds)) {
         area->x = bounds.origin.x;
         area->y = bounds.origin.y;
      }
   }
   area->width   = gtk_widget_get_width (GTK_WIDGET (widget));
   area->height  = gtk_widget_get_height (GTK_WIDGET (widget));

   /* FIXME? */
   area->x = 0;
   area->y = 0;
   /* END FIXME? */
}


/*
 *  create_toggle_button:
 *     @ Create toggle button widget.
 *
 *  label   : Label text for toggle button.
 *  def_val : Default value.
 *  Return  : Toggle button widget.
 */
GtkWidget *
gtkutil_create_check_button (const gchar *label_text, gboolean def_val,
                             gpointer func, gpointer data)
{
   GtkWidget *toggle;

   toggle = gtk_check_button_new_with_label (_(label_text));
   gimv_toggle_set_active (GTK_WIDGET (toggle), def_val);

   if (func)
      g_signal_connect (G_OBJECT (toggle), "toggled",
                          G_CALLBACK (func), data);

   return toggle;
}


GtkWidget *
gtkutil_create_toolbar (void)
{
   GtkWidget *toolbar;

   /* GTK4: GtkToolbar is gone, a toolbar is a horizontal box of buttons */
   toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   gtk_widget_add_css_class (toolbar, "gimv-toolbar");
   gtk_widget_add_css_class (toolbar, "toolbar");

   return toolbar;
}


static void
toolbar_item_set_style (GtkWidget *widget, gint style)
{
   GtkWidget *child;

   if (GTK_IS_IMAGE (widget)) {
      gtk_widget_set_visible (widget, style != 1);
      return;
   }
   if (GTK_IS_LABEL (widget)) {
      gtk_widget_set_visible (widget, style != 0);
      return;
   }
   if (GTK_IS_BOX (widget)
       && !gtk_widget_has_css_class (widget, "gimv-toolbar"))
   {
      gtk_orientable_set_orientation (GTK_ORIENTABLE (widget),
                                      style == 3
                                      ? GTK_ORIENTATION_HORIZONTAL
                                      : GTK_ORIENTATION_VERTICAL);
   }
   /* don't touch arbitrary widgets (spin buttons, entries, ...) */
   if (!GTK_IS_BUTTON (widget) && !GTK_IS_BOX (widget))
      return;

   for (child = gtk_widget_get_first_child (widget);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      toolbar_item_set_style (child, style);
   }
}


/*
 *  GTK4: replacement of gtk_toolbar_set_style () for toolbars created by
 *  gtkutil_create_toolbar ().  Toolbar buttons are expected to contain a
 *  GtkImage and/or a GtkLabel (optionally inside a GtkBox).
 */
void
gtkutil_toolbar_set_style (GtkWidget *toolbar, gint style)
{
   GtkWidget *child;

   g_return_if_fail (GTK_IS_WIDGET (toolbar));

   g_object_set_data (G_OBJECT (toolbar), "gimv-toolbar-style",
                      GINT_TO_POINTER (style));

   for (child = gtk_widget_get_first_child (toolbar);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      if (GTK_IS_BUTTON (child) || GTK_IS_TOGGLE_BUTTON (child))
         toolbar_item_set_style (child, style);
   }
}


GtkWidget *
gtkutil_create_spin_button (GtkAdjustment *adj)
{
   GtkWidget *spinner = gtk_spin_button_new (adj, 0, 0);
   gtk_spin_button_set_wrap (GTK_SPIN_BUTTON (spinner), TRUE);

   return spinner;
}


/*
 *  GTK4: option menus are GtkDropDowns (see gimv_option_menu_new ()), their
 *  items aren't widgets any more.  Returns the selected item object of the
 *  drop down's model (a GtkStringObject for gimv_option_menu_new ()), which
 *  can carry data set with g_object_set_data ().
 */
GObject *
gtkutil_option_menu_get_current (GtkWidget *option_menu)
{
   gpointer item;

   g_return_val_if_fail (GTK_IS_DROP_DOWN (option_menu), NULL);

   item = gtk_drop_down_get_selected_item (GTK_DROP_DOWN (option_menu));
   return item ? G_OBJECT (item) : NULL;
}


GList *
gtkutil_list_insert_sorted (GList        *list,
                            gpointer      data,
                            GCompareFunc  func,
                            gboolean      reverse)
{
   GList *tmp_list = list;
   GList *new_list;
   gint cmp;

   g_return_val_if_fail (func != NULL, list);

   if (!list) {
      new_list = g_list_alloc();
      new_list->data = data;
      return new_list;
   }

   cmp = (*func) (data, tmp_list->data);

   while ((tmp_list->next) &&
          ((!reverse && cmp > 0) || (reverse && cmp <= 0)))
   {
      tmp_list = tmp_list->next;
      cmp = (*func) (data, tmp_list->data);
   }

   new_list = g_list_alloc();
   new_list->data = data;

   if ((!tmp_list->next) && (cmp > 0)) {
      tmp_list->next = new_list;
      new_list->prev = tmp_list;
      return list;
   }

   if (tmp_list->prev) {
      tmp_list->prev->next = new_list;
      new_list->prev = tmp_list->prev;
   }
   new_list->next = tmp_list;
   tmp_list->prev = new_list;

   if (tmp_list == list)
      return new_list;
   else
      return list;
}



/******************************************************************************
 *
 *   Confirm Dialog Window
 *
 ******************************************************************************/
static gint
cb_dummy (GtkWidget *button, gpointer data)
{
   return TRUE;
}


static void
cb_confirm_yes (GtkWidget *button, ConfirmType *type)
{
   *type = CONFIRM_YES;
   gimv_main_quit ();
}


static void
cb_confirm_yes_to_all (GtkWidget *button, ConfirmType *type)
{
   *type = CONFIRM_YES_TO_ALL;
   gimv_main_quit ();
}


static void
cb_confirm_no (GtkWidget *button, ConfirmType *type)
{
   *type = CONFIRM_NO;
   gimv_main_quit ();
}


static void
cb_confirm_no_to_all (GtkWidget *button, ConfirmType *type)
{
   *type = CONFIRM_NO_TO_ALL;
   gimv_main_quit ();
}


static void
cb_confirm_cancel (GtkWidget *button, ConfirmType *type)
{
   *type = CONFIRM_CANCEL;
   gimv_main_quit ();
}


ConfirmType
gtkutil_confirm_dialog (const gchar *title, const gchar *message,
                        ConfirmDialogFlags flags, GtkWindow *parent)
{
   ConfirmType retval = CONFIRM_NO;
   GtkWidget *window;
   GtkWidget *vbox, *hbox, *button, *label;
   GtkWidget *icon;

   window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (window));
   gtk_window_set_title (GTK_WINDOW (window), title); 
   gtk_window_set_default_size (GTK_WINDOW (window), 300, 120);
   /* gimv_container_set_border_width (GTK_WIDGET (gimv_dialog_get_vbox (GTK_WIDGET (window))), 5); */
   /* GTK4: window position can't be set */
   gimv_event_connect (GTK_WIDGET (window), GIMV_EVENT_DELETE, G_CALLBACK (cb_dummy), NULL);

   /* message area */
   vbox = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), vbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (vbox), 15);
   gtk_widget_show (vbox);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gtk_widget_show (hbox);

   /* icon */
   icon = gimv_icon_stock_get_widget ("question");
   gimv_box_pack_start (GTK_BOX (hbox), icon, TRUE, TRUE, 0);
   gtk_widget_show (icon);

   /* message */
   label = gtk_label_new (message);
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
   gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);
   gtk_widget_show (label);

   /* buttons */
   button = gtk_button_new_with_label (_("Yes"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_yes),
                       &retval);
   gtk_widget_show (button);
   gtk_widget_grab_focus (button);

   if (flags & ConfirmDialogMultipleFlag) {
      button = gtk_button_new_with_label (_("Yes to All"));
      gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                          button, TRUE, TRUE, 0);
      g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (cb_confirm_yes_to_all),
                          &retval);
         gtk_widget_show (button);
   }

   button = gtk_button_new_with_label (_("No"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_no),
                       &retval);
   gtk_widget_show (button);

   if (flags & ConfirmDialogMultipleFlag) {
      button = gtk_button_new_with_label (_("Cancel"));
      gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                          button, TRUE, TRUE, 0);
      g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (cb_confirm_cancel),
                          &retval);
         gtk_widget_show (button);
   }

   gtk_widget_show (window);

   gimv_grab_add (window);
   gimv_main ();
   gimv_grab_remove (window);
   gimv_widget_destroy (window);

   return retval;
}


#include "gimv_image_view.h"

typedef struct OverWriteDialog_Tag
{
   GimvImageInfo *info1, *info2;
   GtkWidget *window, *iv1, *iv2, *show_compare_button, *compare_area, *entry;
   gchar *new_path;
   gint new_path_len;
   ConfirmType retval;
} OverWriteDialog;


static void
cb_show_compare (GtkButton *button, OverWriteDialog *dialog)
{
   gimv_image_view_change_image (GIMV_IMAGE_VIEW (dialog->iv1), dialog->info1);
   gimv_image_view_change_image (GIMV_IMAGE_VIEW (dialog->iv2), dialog->info2);
}


static void
cb_show_image1 (GtkButton *button, OverWriteDialog *dialog)
{
   gimv_image_view_change_image (GIMV_IMAGE_VIEW (dialog->iv1), dialog->info1);
}



static void
cb_show_image2 (GtkButton *button, OverWriteDialog *dialog)
{
   gimv_image_view_change_image (GIMV_IMAGE_VIEW (dialog->iv2), dialog->info2);
}


static void
overwrite_confirm_rename (OverWriteDialog *dialog)
{
   const gchar *filename_internal
      = gtk_editable_get_text (GTK_EDITABLE (dialog->entry));
   gchar *dirname, *filename;

   if (filename_internal && strrchr (filename_internal, '/'))
      filename_internal = strrchr (filename_internal, '/') + 1;
   if (!filename_internal || !*filename_internal) return;
   g_return_if_fail (dialog->new_path && dialog->new_path_len > 0);

   dirname = g_path_get_dirname (gimv_image_info_get_path (dialog->info1));
   g_return_if_fail (dirname);
   if (!*dirname) {
      g_free (dirname);
      g_return_if_fail (FALSE);
   }

   filename = charset_internal_to_locale (filename_internal);
   g_return_if_fail (filename);
   if (!*filename) {
      g_free (dirname);
      g_free (filename);
      g_return_if_fail (FALSE);
   }

   g_snprintf (dialog->new_path, dialog->new_path_len, "%s/%s",
               dirname, filename);

   /* check the new path */
   if (file_exists (dialog->new_path)) {
      gchar error_message[BUF_SIZE];
      g_snprintf (error_message, BUF_SIZE,
                  _("The file exists: %s"),
                  dialog->new_path);
      gtkutil_message_dialog (_("Error!"), error_message,
                              GTK_WINDOW (dialog->window));
      g_free (dirname);
      g_free (filename);
      return;
   }

   g_free (dirname);
   g_free (filename);

   dialog->retval = CONFIRM_NO;

   gimv_main_quit ();
}


static void
cb_confirm_rename (GtkButton *button, OverWriteDialog *dialog)
{
   overwrite_confirm_rename (dialog);
}


static void
cb_confirm_rename_enter (GtkEntry *entry, OverWriteDialog *dialog)
{
   overwrite_confirm_rename (dialog);
}


ConfirmType
gtkutil_overwrite_confirm_dialog (const gchar *title, const gchar *message,
                                  const gchar *dest_file, const gchar *src_file,
                                  gchar *new_path, gint new_path_len,
                                  ConfirmDialogFlags flags,
                                  GtkWindow *parent)
{
   OverWriteDialog dialog;
   GtkWidget *window;
   GtkWidget *vbox, *hbox, *button, *label, *entry;
   GtkWidget *icon;
   GtkWidget *vbox2;
   gchar *filename;

   dialog.retval = CONFIRM_NO;
   dialog.info1 = gimv_image_info_get (dest_file);
   dialog.info2 = gimv_image_info_get (src_file);
   dialog.new_path = new_path;
   dialog.new_path_len = new_path_len;

   dialog.window = window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (window));
   gtk_window_set_title (GTK_WINDOW (window), title); 
   gtk_window_set_default_size (GTK_WINDOW (window), 300, 120);
   /* gimv_container_set_border_width (GTK_WIDGET (gimv_dialog_get_vbox (GTK_WIDGET (window))), 5); */
   /* GTK4: window position can't be set */
   gimv_event_connect (GTK_WIDGET (window), GIMV_EVENT_DELETE, G_CALLBACK (cb_dummy), NULL);

   /* message area */
   vbox = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), vbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (vbox), 15);
   gtk_widget_show (vbox);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gtk_widget_show (hbox);

   /* icon */
   icon = gimv_icon_stock_get_widget ("question");
   gimv_box_pack_start (GTK_BOX (hbox), icon, TRUE, TRUE, 0);
   gtk_widget_show (icon);

   /* message */
   label = gtk_label_new (message);
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
   gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);
   gtk_widget_show (label);

   /* compare area */
   {
      /* show image buttons */
      hbox = gimv_hbox_new (TRUE, 0);
      gimv_box_pack_start (GTK_BOX (vbox),
                          hbox, FALSE, FALSE, 2);
      gtk_widget_show (hbox);

      label = gtk_button_new_with_label (_("Show destination"));
      gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 2);
      g_signal_connect (G_OBJECT (label), "clicked",
                          G_CALLBACK (cb_show_image1),
                          &dialog);
      gtk_widget_show (label);

      button = gtk_button_new_with_label (_("Show both images"));
      dialog.show_compare_button = button;
      gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 2);
      g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (cb_show_compare),
                          &dialog);
      gtk_widget_show (button);

      label = gtk_button_new_with_label (_("Show source"));
      gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 2);
      g_signal_connect (G_OBJECT (label), "clicked",
                          G_CALLBACK (cb_show_image2),
                          &dialog);
      gtk_widget_show (label);

      /* view */
      dialog.compare_area = hbox = gimv_hbox_new (TRUE, 0);
      gimv_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 0);
      gtk_widget_show (hbox);

      /* destination */
      vbox2 = gimv_vbox_new (FALSE, 0);
      gimv_box_pack_start (GTK_BOX (hbox), vbox2, TRUE, TRUE, 2);
      gtk_widget_show (vbox2);

      dialog.iv1 = gimv_image_view_new (NULL);
      g_object_set (G_OBJECT (dialog.iv1),
                      "default_zoom",     3,
                      "default_rotation", 0,
                      "keep_aspect",     TRUE,
                      NULL);
      gimv_image_view_hide_scrollbar (GIMV_IMAGE_VIEW (dialog.iv1));
      gimv_widget_set_size (dialog.iv1, -1, 150);
      gimv_box_pack_start (GTK_BOX (vbox2), dialog.iv1, TRUE, TRUE, 0);
      gtk_widget_show (dialog.iv1);

      /* gimv_image_view_set_text (dialog.iv1, information) */


      /* source */
      vbox2 = gimv_vbox_new (FALSE, 0);
      gimv_box_pack_start (GTK_BOX (hbox), vbox2, TRUE, TRUE, 2);
      gtk_widget_show (vbox2);

      dialog.iv2 = gimv_image_view_new (NULL);
      g_object_set (G_OBJECT (dialog.iv2),
                      "default_zoom",     3,
                      "default_rotation", 0,
                      "keep_aspect",     TRUE,
                      NULL);
      gimv_image_view_hide_scrollbar (GIMV_IMAGE_VIEW (dialog.iv2));
      gimv_widget_set_size (dialog.iv2, -1, 150);
      gimv_box_pack_start (GTK_BOX (vbox2), dialog.iv2, TRUE, TRUE, 0);
      gtk_widget_show (dialog.iv2);

      /* gimv_image_view_set_text (dialog.iv2, information) */
   }

   vbox = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))),
                       vbox, FALSE, FALSE, 0);
   gtk_widget_show (vbox);

   /* rename entry */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 0);
   gtk_widget_show (hbox);

   dialog.entry = entry = gtk_entry_new ();
   gimv_box_pack_start (GTK_BOX (hbox), entry, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (entry), "activate",
                       G_CALLBACK (cb_confirm_rename_enter), &dialog);
   filename = charset_to_internal (g_basename (src_file),
                                   conf.charset_filename,
                                   conf.charset_auto_detect_fn,
                                   conf.charset_filename_mode);
   gtk_editable_set_text (GTK_EDITABLE (entry), filename);
   g_free (filename);
   filename = NULL;
   gtk_widget_show (entry);

   button = gtk_button_new_with_label (_("Rename"));
   gimv_box_pack_start (GTK_BOX (hbox), button, FALSE, FALSE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_rename),
                       &dialog);
   gtk_widget_show (button);

   /* buttons */
   hbox = gimv_hbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (vbox),
                       hbox, TRUE, TRUE, 0);
   gtk_widget_show (hbox);

   button = gtk_button_new_with_label (_("Yes"));
   gimv_box_pack_start (GTK_BOX (hbox),  button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_yes),
                       &dialog.retval);
   gtk_widget_show (button);
   gtk_widget_grab_focus (button);

   if (flags & ConfirmDialogMultipleFlag) {
      button = gtk_button_new_with_label (_("Yes to All"));
      gimv_box_pack_start (GTK_BOX (hbox),button, TRUE, TRUE, 0);
      g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (cb_confirm_yes_to_all),
                          &dialog.retval);
         gtk_widget_show (button);
   }

   button = gtk_button_new_with_label (_("Skip"));
   gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_no),
                       &dialog.retval);
   gtk_widget_show (button);

   button = gtk_button_new_with_label (_("Skip all"));
   gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_confirm_no_to_all),
                       &dialog.retval);
   gtk_widget_show (button);

   if (flags & ConfirmDialogMultipleFlag) {
      button = gtk_button_new_with_label (_("Cancel"));
      gimv_box_pack_start (GTK_BOX (hbox), button, TRUE, TRUE, 0);
      g_signal_connect (G_OBJECT (button), "clicked",
                          G_CALLBACK (cb_confirm_cancel),
                          &dialog.retval);
         gtk_widget_show (button);
   }

   gtk_widget_show (window);

   gimv_grab_add (window);
   gimv_main ();
   gimv_grab_remove (window);
   gimv_widget_destroy (window);

   if (dialog.info1) gimv_image_info_unref (dialog.info1);
   if (dialog.info2) gimv_image_info_unref (dialog.info2);

   return dialog.retval;
}



/******************************************************************************
 *
 *   Message Dialog Window
 *
 ******************************************************************************/
static void
cb_message_dialog_quit (GtkWidget *button, gpointer data)
{
   gimv_main_quit ();
}


void
gtkutil_message_dialog (const gchar *title, const gchar *message, GtkWindow *parent)
{
   GtkWidget *window;
   GtkWidget *button, *label, *vbox, *hbox;
   GtkWidget *alert_icon;

   window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (window));
   gtk_window_set_title (GTK_WINDOW (window), title); 
   /* GTK4: window position can't be set */
   gimv_event_connect (GTK_WIDGET (window), GIMV_EVENT_DELETE, G_CALLBACK (cb_dummy), NULL);

   /* message area */
   vbox = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), vbox,
                       TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (vbox), 15);
   gtk_widget_show (vbox);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gtk_widget_show (hbox);

   /* icon */
   alert_icon = gimv_icon_stock_get_widget ("alert");
   gimv_box_pack_start (GTK_BOX (hbox), alert_icon, TRUE, TRUE, 0);
   gtk_widget_show (alert_icon);

   /* message */
   label = gtk_label_new (message);
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
   gimv_box_pack_start (GTK_BOX (hbox), label, TRUE, TRUE, 0);
   gtk_widget_show (label);

   /* button */
   button = gtk_button_new_with_label (_("OK"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK (cb_message_dialog_quit), NULL);
   gtk_widget_show (button);

   gtk_widget_grab_focus (button);

   gtk_widget_show (window);

   gimv_grab_add (window);
   gimv_main ();
   gimv_grab_remove (window);
   gimv_widget_destroy (window);
}



/******************************************************************************
 *
 *   Progress Bar Window
 *
 ******************************************************************************/
static void
cb_progress_win_cancel (GtkWidget *button, gboolean *cancel_pressed)
{
   *cancel_pressed = TRUE;
}


void
gtkutil_progress_window_update (GtkWidget *window,
                                gchar *title, gchar *message,
                                gchar *progress_text, gfloat progress)
{
   GtkWidget *label;
   GtkWidget *progressbar;

   g_return_if_fail (window);

   label = g_object_get_data (G_OBJECT (window), "label");
   progressbar = g_object_get_data (G_OBJECT (window), "progressbar");

   g_return_if_fail (label && progressbar);

   if (title)
      gtk_window_set_title (GTK_WINDOW (window), _(title));
   if (message)
      gtk_label_set_text (GTK_LABEL (label), message);
   if (progress_text)
      gtk_progress_bar_set_text (GTK_PROGRESS_BAR (progressbar),
                                 progress_text);
   if (progress > 0.0 && progress < 1.0)
      gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progressbar), progress);
}


GtkWidget *
gtkutil_create_progress_window (gchar *title, gchar *initial_message,
                                gboolean *cancel_pressed,
                                gint width, gint height, GtkWindow *parent)
{
   GtkWidget *window;
   GtkWidget *vbox;
   GtkWidget *label;
   GtkWidget *progressbar;
   GtkWidget *button;

   g_return_val_if_fail (title && initial_message && cancel_pressed, NULL);

   *cancel_pressed = FALSE;

   /* create dialog window */
   window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (window));
   gimv_container_set_border_width (window, 3);
   gtk_window_set_title (GTK_WINDOW (window), title);
   /* GTK4: window position can't be set */
   gtk_window_set_default_size (GTK_WINDOW (window), width, height);
   gimv_event_connect (GTK_WIDGET (window), GIMV_EVENT_DELETE, G_CALLBACK (cb_dummy), NULL);

   /* message area */
   vbox = gimv_vbox_new (FALSE, 5);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), vbox,
                       TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (vbox), 5);
   gtk_widget_show (vbox);

   /* label */
   label = gtk_label_new (initial_message);
   gimv_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

   /* progress bar */
   progressbar = gtk_progress_bar_new();
   gtk_progress_bar_set_show_text (GTK_PROGRESS_BAR (progressbar), TRUE);
   gimv_box_pack_start (GTK_BOX (vbox), progressbar, FALSE, FALSE, 0);

   /* cancel button */
   button = gtk_button_new_with_label (_("Cancel"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), button,
                       TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK(cb_progress_win_cancel), cancel_pressed);

   g_object_set_data (G_OBJECT (window), "label", label);
   g_object_set_data (G_OBJECT (window), "progressbar", progressbar);

   gimv_widget_show_all (window);

   return window;
}



/******************************************************************************
 *
 *   Text Entry Window
 *
 ******************************************************************************/
static void
cb_textpop_enter (GtkWidget *button, gboolean *ok_pressd)
{
   *ok_pressd = TRUE;
   gimv_main_quit ();
}


static void
cb_textpop_ok_button (GtkWidget *button, gboolean *ok_pressd)
{
   *ok_pressd = TRUE;
   gimv_main_quit ();
}


static void
cb_textpop_cancel_button (GtkWidget *button, gboolean *ok_pressd)
{
   *ok_pressd = FALSE;
   gimv_main_quit ();
}


static gint
cb_textpop_key_press (GtkWidget   *widget, 
                      GimvEventKey *event,
                      gboolean *ok_pressd)
{
   const gchar *path;
   gchar *text;
   gint   n, len;
   guint comp_key1 = 0, comp_key2 = 0;
   GdkModifierType comp_mods1 = 0, comp_mods2 = 0;

   if (akey.common_auto_completion1)
      gtk_accelerator_parse (akey.common_auto_completion1,
                             &comp_key1, &comp_mods1);
   if (akey.common_auto_completion2)
      gtk_accelerator_parse (akey.common_auto_completion2,
                             &comp_key2, &comp_mods2);

   if (event->keyval == GDK_KEY_Tab
       || (event->keyval == comp_key1 && (!comp_mods1 || (event->state & comp_mods1)))
       || (event->keyval == comp_key2 && (!comp_mods1 || (event->state & comp_mods2))))
   {
      path = gtk_editable_get_text (GTK_EDITABLE (widget));
      n = auto_compl_get_n_alternatives (path);

      if (n < 1) return TRUE;

      text = auto_compl_get_common_prefix ();

      if (n == 1) {
         auto_compl_hide_alternatives ();
         if (*text && text[strlen(text) - 1] != '/') {
            gchar *tmp = g_strconcat (text, "/", NULL);
            gtk_editable_set_text (GTK_EDITABLE (widget), tmp);
            g_free (tmp);
         } else {
            gtk_editable_set_text (GTK_EDITABLE (widget), text);
         }
         gtk_editable_set_position (GTK_EDITABLE (widget), -1);
      } else {
         gtk_editable_set_text (GTK_EDITABLE (widget), text);
         gtk_editable_set_position (GTK_EDITABLE (widget), -1);
         auto_compl_show_alternatives (widget);
      }
	 
      if (text)
         g_free (text);
      return TRUE;

   } else {
      switch (event->keyval) {
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
         path = gtk_editable_get_text (GTK_EDITABLE (widget));

         if (!isdir (path)) return FALSE;

         len = strlen (path);
         if (path[len - 1] != '/') {
            text = g_strconcat (path, "/", NULL);
         } else {
            text = g_strdup (path);
         }
         g_free (text);
         break;
      case GDK_KEY_Right:
      case GDK_KEY_Left:
      case GDK_KEY_Up:
      case GDK_KEY_Down:
         break;
      case GDK_KEY_Escape:
         *ok_pressd = FALSE;
         gimv_main_quit ();
         break;
      default:
         break;
      }
   }

   return FALSE;
}


gchar *
gtkutil_popup_textentry (const gchar   *title,
                         const gchar   *label_text,
                         const gchar   *entry_text,
                         GList         *text_list,
                         gint           entry_width,
                         TextEntryFlags flags,
                         GtkWindow *parent)
{
   GtkWidget *window, *box, *hbox, *vbox, *button, *label, *combo, *entry;
   gboolean ok_pressed = FALSE;
   gchar *str = NULL;

   /* dialog window */
   window = gtk_dialog_new ();
   if (parent)
      gtk_window_set_transient_for (GTK_WINDOW (window), parent);
   else
      gimv_window_set_default_transient (GTK_WINDOW (window));
   gtk_window_set_title (GTK_WINDOW (window), title); 
   /* GTK4: window position can't be set */
   gimv_event_connect (GTK_WIDGET (window), GIMV_EVENT_DELETE, G_CALLBACK (cb_dummy), NULL);

   /* main area */
   vbox = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_vbox (GTK_WIDGET (window))), vbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (vbox), 5);
   gtk_widget_show (vbox);

   hbox = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, TRUE, TRUE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gtk_widget_show (hbox);

   /* label */
   label = gtk_label_new (label_text);
   gimv_box_pack_start (GTK_BOX (hbox), label, FALSE, FALSE, 0);
   gtk_widget_show (label);

   /* entry */
   if (flags & TEXT_ENTRY_WRAP_ENTRY)
      box = vbox;
   else
      box = hbox;

   if (text_list) {
      combo = gimv_combo_new();
      entry = gimv_combo_get_entry (GTK_WIDGET (combo));
      gimv_combo_set_popdown_strings (GTK_WIDGET (combo), text_list);
   } else {
      /* GTK4: a combo box without its button is just an entry */
      combo = entry = gtk_entry_new ();
   }

   gimv_box_pack_start (GTK_BOX (box), combo, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (entry), "activate",
                       G_CALLBACK (cb_textpop_enter), &ok_pressed);
   if (entry_text)
      gtk_editable_set_text (GTK_EDITABLE (entry), entry_text);
   if (flags & TEXT_ENTRY_CURSOR_TOP)
      gtk_editable_set_position (GTK_EDITABLE (entry), 0);
   if (entry_width > 0)
      gimv_widget_set_size (combo, entry_width, -1);
   if (flags & TEXT_ENTRY_AUTOCOMP_PATH)
      gimv_event_connect_after (GTK_WIDGET (entry), GIMV_EVENT_KEY_PRESS, G_CALLBACK(cb_textpop_key_press), &ok_pressed);
   gtk_widget_show (combo);

   if (flags & TEXT_ENTRY_NO_EDITABLE)
      gtk_editable_set_editable (GTK_EDITABLE (entry), FALSE);

   gtk_widget_grab_focus (entry);

   /* button */
   button = gtk_button_new_with_label (_("OK"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK(cb_textpop_ok_button), &ok_pressed);
   gtk_widget_show (button);

   button = gtk_button_new_with_label (_("Cancel"));
   gimv_box_pack_start (GTK_BOX (gimv_dialog_get_action_area (GTK_WIDGET (window))), 
                       button, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (button), "clicked",
                       G_CALLBACK(cb_textpop_cancel_button), &ok_pressed);
   gtk_widget_show (button);

   gtk_widget_show (window);

   gimv_grab_add (window);
   gimv_main ();
   gimv_grab_remove (window);

   if (ok_pressed)
      str = g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));

   gimv_widget_destroy (window);

   return str;
}


/******************************************************************************
 *
 *   modal file dialog
 *
 ******************************************************************************/
gchar *
gtkutil_modal_file_dialog (const gchar   *title,
                           const gchar   *default_path,
                           ModalFileDialogFlags flags,
                           GtkWindow *parent)
{
   GtkWidget *filesel;
   GtkFileChooserAction action;
   gchar *filename = NULL;
   gint response;

   /* GTK4: GtkFileSelection -> GtkFileChooserDialog.
      MODAL_FILE_DIALOG_HIDE_FILEOP has no meaning any more. */
   if (flags & MODAL_FILE_DIALOG_DIR_ONLY)
      action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
   else
      action = GTK_FILE_CHOOSER_ACTION_SAVE;  /* allow any (new) file name */

   filesel = gtk_file_chooser_dialog_new (title, parent, action,
                                          _("_Cancel"), GTK_RESPONSE_CANCEL,
                                          _("_OK"),     GTK_RESPONSE_ACCEPT,
                                          NULL);
   gtk_dialog_set_default_response (GTK_DIALOG (filesel), GTK_RESPONSE_ACCEPT);
   if (action == GTK_FILE_CHOOSER_ACTION_SAVE)
      gtk_file_chooser_set_create_folders (GTK_FILE_CHOOSER (filesel), TRUE);

   if (default_path && *default_path) {
      GFile *file = g_file_new_for_path (default_path);

      if (g_file_test (default_path, G_FILE_TEST_IS_DIR)) {
         gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (filesel),
                                              file, NULL);
      } else if (action == GTK_FILE_CHOOSER_ACTION_SAVE) {
         GFile *parent_dir = g_file_get_parent (file);
         gchar *basename = g_file_get_basename (file);

         if (parent_dir && g_file_query_exists (parent_dir, NULL))
            gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (filesel),
                                                 parent_dir, NULL);
         if (basename && g_file_test (default_path, G_FILE_TEST_EXISTS)) {
            gtk_file_chooser_set_file (GTK_FILE_CHOOSER (filesel), file, NULL);
         } else if (basename) {
            gchar *name = g_filename_to_utf8 (basename, -1, NULL, NULL, NULL);
            if (name)
               gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (filesel),
                                                  name);
            g_free (name);
         }
         if (parent_dir) g_object_unref (parent_dir);
         g_free (basename);
      } else {
         GFile *parent_dir = g_file_get_parent (file);
         if (parent_dir && g_file_query_exists (parent_dir, NULL))
            gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (filesel),
                                                 parent_dir, NULL);
         if (parent_dir) g_object_unref (parent_dir);
      }

      g_object_unref (file);
   }

   response = gimv_dialog_run (GTK_DIALOG (filesel));

   if (response == GTK_RESPONSE_ACCEPT) {
      GFile *file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (filesel));
      if (file) {
         filename = g_file_get_path (file);
         g_object_unref (file);
      }
   }

   if (GTK_IS_WINDOW (filesel))
      gimv_widget_destroy (filesel);

   return filename;
}



/******************************************************************************
 *
 *   Color selection button
 *
 ******************************************************************************/
static void
cb_choose_color (GtkWidget *widget, gint color[3])
{
   GtkWidget *dialog;
   GdkRGBA rgba;
   gint response;

   g_return_if_fail (color);

   /* GTK4: GtkColorSelectionDialog -> GtkColorChooserDialog */
   dialog = gtk_color_chooser_dialog_new (_("Choose Color"),
                                          GTK_WINDOW (gimv_widget_get_toplevel (widget)));
   gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (dialog), FALSE);
   rgba.red   = (gdouble) color[0] / 0xffff;
   rgba.green = (gdouble) color[1] / 0xffff;
   rgba.blue  = (gdouble) color[2] / 0xffff;
   rgba.alpha = 1.0;
   gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (dialog), &rgba);

   response = gimv_dialog_run (GTK_DIALOG (dialog));
   if (response == GTK_RESPONSE_OK) {
      gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (dialog), &rgba);
      color[0] = rgba.red   * 0xffff;
      color[1] = rgba.green * 0xffff;
      color[2] = rgba.blue  * 0xffff;
   }
   if (GTK_IS_WINDOW (dialog))
      gimv_widget_destroy (dialog);
}


GtkWidget *
gtkutil_color_sel_button (const gchar *label, gint color[3])
{
   GtkWidget *button;

   button = gtk_button_new_with_label (label);
   g_signal_connect (G_OBJECT (button),"clicked",
                       G_CALLBACK (cb_choose_color),
                       color);

   return button;
}


/******************************************************************************
 *
 *   Compare functions
 *
 ******************************************************************************/
gint
gtkutil_comp_spel (gconstpointer data1, gconstpointer data2)
{
   const gchar *str1 = data1;
   const gchar *str2 = data2;

   return strcmp (str1, str2);
}


gint
gtkutil_comp_casespel (gconstpointer data1, gconstpointer data2)
{
   const gchar *str1 = data1;
   const gchar *str2 = data2;

   return g_ascii_strcasecmp (str1, str2);
}


/******************************************************************************
 *
 *   simple callback functions
 *
 ******************************************************************************/
void
gtkutil_get_data_from_toggle_cb (GtkWidget *toggle, gboolean *data)
{
   g_return_if_fail (data);

   *data = gimv_toggle_get_active (GTK_WIDGET (toggle));
}


void
gtkutil_get_data_from_toggle_negative_cb (GtkWidget *toggle, gboolean *data)
{
   g_return_if_fail (data);

   *data = !(gimv_toggle_get_active (GTK_WIDGET (toggle)));
}


void
gtkutil_get_data_from_adjustment_by_int_cb (GtkWidget *widget, gint *data)
{
   g_return_if_fail (data);

   *data = gtk_adjustment_get_value (GTK_ADJUSTMENT (widget));
}


void
gtkutil_get_data_from_adjustment_by_float_cb (GtkWidget *widget, gfloat *data)
{
   g_return_if_fail (data);

   *data = gtk_adjustment_get_value (GTK_ADJUSTMENT (widget));
}
