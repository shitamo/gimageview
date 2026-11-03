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
 * $Id: gimv_comment_view.c,v 1.6 2004/09/22 15:37:11 makeinu Exp $
 */

#include "gimv_comment_view.h"

#include <string.h>

#include "gimageview.h"

#include "charset.h"
#include "dnd.h"
#include "gimv_comment.h"
#include "gimv_icon_stock.h"
#include "gimv_image_info.h"
#include "gimv_image_view.h"
#include "gimv_list_view.h"
#include "prefs.h"


typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_KEY,
   COLUMN_VALUE,
   COLUMN_RAW_ENTRY,
   N_COLUMN
} ListStoreColumn;


static void gimv_comment_view_set_sensitive  (GimvCommentView *cv);
static void gimv_comment_view_set_combo_list (GimvCommentView *cv);
static void gimv_comment_view_data_enter     (GimvCommentView *cv);
static void gimv_comment_view_reset_data     (GimvCommentView *cv);
static void combo_select_name                 (GimvCommentView *cv,
                                               const gchar     *name);


static GimvRow *
comment_row_new (void)
{
   return gimv_row_new (N_COLUMN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER);
}


/******************************************************************************
 *
 *   callback functions.
 *
 ******************************************************************************/
static void
cb_switch_page (GtkNotebook *notebook,
                GtkWidget *page,
                guint pagenum,
                GimvCommentView *cv)
{
   GtkWidget *widget;

   g_return_if_fail (page);
   g_return_if_fail (cv);

   if (!cv->button_area) return;

   widget = gtk_notebook_get_nth_page (notebook, pagenum);

   if (widget == cv->data_page || widget == cv->note_page)
      gtk_widget_set_visible (cv->button_area, TRUE);
   else
      gtk_widget_set_visible (cv->button_area, FALSE);

   gimv_comment_view_set_sensitive (cv);
}


static void
gimv_comment_view_delete_selected_data (GimvCommentView *cv)
{
   GimvCommentDataEntry *entry;

   g_return_if_fail (cv);

   {
      guint position = gimv_list_view_get_selected (cv->comment_clist);
      GListModel *model = gimv_list_view_get_model (cv->comment_clist);
      GimvRow *row = gimv_row_store_peek (model, position);

      if (!row) return;

      entry = gimv_row_peek_pointer (row, COLUMN_RAW_ENTRY);
      g_list_store_remove (G_LIST_STORE (model), position);
   }

   if (entry)
      gimv_comment_data_entry_remove (cv->comment, entry);

   gtk_drop_down_set_selected (GTK_DROP_DOWN (cv->key_combo),
                               GTK_INVALID_LIST_POSITION);
   gtk_editable_set_text (GTK_EDITABLE (cv->value_entry), "\0");

   gimv_comment_view_set_sensitive (cv);
}


static void
cb_save_button_pressed (GtkButton *button, GimvCommentView *cv)
{
   gchar *note;

   g_return_if_fail (cv);
   g_return_if_fail (cv->comment);

   {
      GtkTextBuffer *buffer;
      GtkTextIter start, end;

      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (cv->note_box));

      gtk_text_buffer_get_iter_at_offset (buffer, &start, 0);
      gtk_text_buffer_get_iter_at_offset (buffer, &end, -1);

      note = gtk_text_buffer_get_text (buffer, &start, &end, TRUE);
   }

   if (note && *note)
      gimv_comment_update_note (cv->comment, note);

   g_free (note);

   gimv_comment_save_file (cv->comment);
}


static void
cb_reset_button_pressed (GtkButton *button, GimvCommentView *cv)
{
   g_return_if_fail (cv);
   g_return_if_fail (cv->comment);

   gimv_comment_view_change_file (cv, cv->comment->info);
}


static void
cb_del_button_pressed (GtkButton *button, GimvCommentView *cv)
{
   g_return_if_fail (cv->comment);

   gimv_comment_delete_file (cv->comment);
   gimv_comment_unref (cv->comment);
   cv->comment = NULL;
   gimv_comment_view_clear (cv);
}


static void
cb_destroyed (GtkWidget *widget, GimvCommentView *cv)
{
   g_return_if_fail (cv);

   /* GTK4: when the window is closed (close-request, Esc), the notebook
      can outlive this box and still emit "switch-page" while it removes its
      pages; disconnect it before cv is freed (cv->notebook is a weak
      pointer, NULL if the notebook is already gone) */
   if (cv->notebook) {
      g_signal_handlers_disconnect_by_data (cv->notebook, cv);
      g_object_remove_weak_pointer (G_OBJECT (cv->notebook),
                                    (gpointer *) &cv->notebook);
      cv->notebook = NULL;
   }

   if (cv->comment) {
      gimv_comment_unref (cv->comment);
      cv->comment = NULL;
   }

   /* GTK4: no accel group any more (mnemonics are used) */
   cv->accel_group = NULL;

   g_free (cv);
}


static void
cb_data_list_selection_changed (GtkSelectionModel *selection,
                                guint position, guint n_items,
                                GimvCommentView *cv)
{
   GimvRow *row;
   const gchar *key = NULL, *value = NULL;

   if (!cv->comment_clist) return;

   row = gimv_list_view_get_selected_row (cv->comment_clist);
   if (row) {
      key   = gimv_row_peek_string (row, COLUMN_KEY);
      value = gimv_row_peek_string (row, COLUMN_VALUE);
   }

   combo_select_name (cv, key);
   gtk_editable_set_text (GTK_EDITABLE (cv->value_entry), value ? value : "");

   gimv_comment_view_set_sensitive (cv);
}



static gboolean
cb_data_list_key_press (GtkWidget *widget, GimvEventKey *event, GimvCommentView *cv)
{
   g_return_val_if_fail (cv, FALSE);

   switch (event->keyval) {
   case GDK_KEY_KP_Enter:
   case GDK_KEY_Return:
      if (cv->selected_item) {
         gtk_widget_grab_focus (cv->value_entry);
         return TRUE;
      }
      break;

   case GDK_KEY_Delete:
      gimv_comment_view_delete_selected_data  (cv);
      return TRUE;
      break;

   default:
      break;
   }

   return FALSE;
}


static gboolean
cb_entry_key_press (GtkWidget *widget, GimvEventKey *event, GimvCommentView *cv)
{
   g_return_val_if_fail (cv, FALSE);

   switch (event->keyval) {
   case GDK_KEY_Tab:
      if (event->state & GDK_SHIFT_MASK) {
         gtk_widget_grab_focus (cv->key_combo);
      } else {
         gtk_widget_grab_focus (cv->save_button);
      }
      return TRUE;
      break;

   default:
      break;
   }

   return FALSE;
}


static void
cb_entry_changed (GtkEditable *entry, GimvCommentView *cv)
{
   g_return_if_fail (cv);

   gimv_comment_view_set_sensitive (cv);
}


static void
cb_entry_enter (GtkEditable *entry, GimvCommentView *cv)
{
   g_return_if_fail (cv);

   gimv_comment_view_data_enter (cv);
   gtk_widget_grab_focus (cv->comment_clist);
}


/*
 *  GTK4: GtkCombo is gone, the key combo is a GtkDropDown (its entry was
 *  not editable).  The keys and the display names of the items are attached
 *  to the combo as "keys" and "names" (GPtrArray).  When an item is selected
 *  the key of it is attached as "key" and cv->selected_item points to the
 *  combo.
 */
static const gchar *
combo_get_selected_name (GimvCommentView *cv)
{
   GPtrArray *names = g_object_get_data (G_OBJECT (cv->key_combo), "names");
   guint idx = gtk_drop_down_get_selected (GTK_DROP_DOWN (cv->key_combo));

   if (!names || idx == GTK_INVALID_LIST_POSITION || idx >= names->len)
      return "";
   return g_ptr_array_index (names, idx);
}


/* select the item whose display name is "name" (none if not found) */
static void
combo_select_name (GimvCommentView *cv, const gchar *name)
{
   GPtrArray *names = g_object_get_data (G_OBJECT (cv->key_combo), "names");
   guint i, idx = GTK_INVALID_LIST_POSITION;

   for (i = 0; name && names && i < names->len; i++) {
      if (!strcmp (name, g_ptr_array_index (names, i))) {
         idx = i;
         break;
      }
   }
   gtk_drop_down_set_selected (GTK_DROP_DOWN (cv->key_combo), idx);
}


static void
combo_select_key (GimvCommentView *cv, const gchar *key)
{
   GtkWidget *clist;

   g_return_if_fail (cv);
   g_return_if_fail (key);

   g_object_set_data_full (G_OBJECT (cv->key_combo), "key",
                           g_strdup (key), (GDestroyNotify) g_free);
   cv->selected_item = cv->key_combo;
   clist = cv->comment_clist;

   {
      GListModel *model = gimv_list_view_get_model (clist);
      guint i, n = g_list_model_get_n_items (model);

      for (i = 0; i < n; i++) {
         GimvCommentDataEntry *entry;

         entry = gimv_row_peek_pointer (gimv_row_store_peek (model, i),
                                        COLUMN_RAW_ENTRY);
         if (!entry) continue;

         if (entry->key && !strcmp (key, entry->key)) {
            if (gimv_list_view_get_selected (clist) != i)
               gimv_list_view_select (clist, i);
            break;
         }
      }
   }
}


static void
cb_combo_changed (GtkDropDown *combo, GParamSpec *pspec, GimvCommentView *cv)
{
   GPtrArray *keys;
   guint idx;

   g_return_if_fail (cv);

   keys = g_object_get_data (G_OBJECT (combo), "keys");
   if (!keys) {
      cv->selected_item = NULL;
      return;
   }

   idx = gtk_drop_down_get_selected (combo);
   if (idx != GTK_INVALID_LIST_POSITION && idx < keys->len)
      combo_select_key (cv, g_ptr_array_index (keys, idx));
   else
      cv->selected_item = NULL;
}


static void
cb_file_saved (GimvComment *comment, GimvCommentView *cv)
{
   g_return_if_fail (cv);
   if (!cv->comment) return;

   gimv_comment_view_reset_data (cv);
}


/******************************************************************************
 *
 *   other private functions.
 *
 ******************************************************************************/
static GimvCommentView *
gimv_comment_view_new ()
{
   GimvCommentView *cv;

   cv = g_new0 (GimvCommentView, 1);
   g_return_val_if_fail (cv, NULL);

   cv->comment       = NULL;

   cv->window        = NULL;
   cv->main_vbox     = NULL;
   cv->notebook      = NULL;
   cv->button_area   = NULL;

   cv->data_page     = NULL;
   cv->note_page     = NULL;

   cv->comment_clist = NULL;
   cv->selected_row  = -1;
   cv->key_combo     = NULL;
   cv->value_entry   = NULL;
   cv->note_box      = NULL;
   cv->selected_item = NULL;

   cv->save_button   = NULL;
   cv->reset_button  = NULL;
   cv->delete_button = NULL;

   cv->accel_group   = NULL;
   cv->iv            = NULL;
   cv->next_button   = NULL;
   cv->prev_button   = NULL;
   cv->changed       = FALSE;

   return cv;
}


static GtkWidget *
create_data_page (GimvCommentView *cv)
{
   GtkWidget *vbox, *vbox1, *hbox, *hbox1;
   GtkWidget *scrolledwin, *clist, *combo, *entry;
   GtkWidget *label;
   gchar *titles[] = {_("Key"), _("Value")};

   label = gtk_label_new (_("Data"));
   gtk_widget_set_name (label, "TabLabel");
   gtk_widget_set_visible (label, TRUE);
   cv->data_page = vbox = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_visible (vbox, TRUE);

   gtk_notebook_append_page (GTK_NOTEBOOK(cv->notebook),
                             vbox, label);

   /* scrolled window & clist */
   scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gimv_box_pack_start(GTK_BOX(vbox), scrolledwin, TRUE, TRUE, 0);

{
   GtkColumnViewColumn *col;

   /* GTK4: GtkTreeView (deprecated) -> GtkColumnView */
   clist = gimv_column_view_new (G_LIST_MODEL (gimv_row_store_new ()),
                                 GTK_SELECTION_SINGLE);
   cv->comment_clist = clist;

   /* set column for key */
   col = gimv_column_view_append_text (GTK_COLUMN_VIEW (clist),
                                       titles[0], COLUMN_KEY);
   gtk_column_view_column_set_resizable (col, TRUE);

   /* set column for value */
   col = gimv_column_view_append_text (GTK_COLUMN_VIEW (clist),
                                       titles[1], COLUMN_VALUE);
   gtk_column_view_column_set_resizable (col, TRUE);
   gtk_column_view_column_set_expand (col, TRUE);

   g_signal_connect (gimv_list_view_get_selection (clist), "selection-changed",
                     G_CALLBACK (cb_data_list_selection_changed), cv);

   gimv_container_add (GTK_WIDGET (scrolledwin), clist);
}
   gimv_event_connect (GTK_WIDGET (clist), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_data_list_key_press), cv);
   /* entry area */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);

   vbox1 = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox1, TRUE, TRUE, 0);
   hbox1 = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox1), hbox1, TRUE, TRUE, 0);
   label = gtk_label_new (_("Key: "));
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
   gimv_box_pack_start (GTK_BOX (hbox1), label, FALSE, FALSE, 0);

   cv->key_combo = combo = gtk_drop_down_new_from_strings (NULL);
   gimv_box_pack_start (GTK_BOX (vbox1), combo, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (combo), "notify::selected",
                     G_CALLBACK (cb_combo_changed), cv);

   vbox1 = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox1, TRUE, TRUE, 0);
   hbox1 = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (vbox1), hbox1, TRUE, TRUE, 0);
   label = gtk_label_new (_("Value: "));
   gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
   gimv_box_pack_start (GTK_BOX (hbox1), label, FALSE, FALSE, 0);
   cv->value_entry = entry = gtk_entry_new ();
   gimv_box_pack_start (GTK_BOX (vbox1), entry, TRUE, TRUE, 0);
   g_signal_connect (G_OBJECT (entry), "changed",
                       G_CALLBACK (cb_entry_changed), cv);
   g_signal_connect (G_OBJECT (entry), "activate",
                       G_CALLBACK (cb_entry_enter), cv);
   gimv_event_connect (GTK_WIDGET (entry), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_entry_key_press), cv);

   gimv_widget_show_all (cv->data_page);

   return vbox;
}


static void
cb_text_buffer_changed (GtkTextBuffer *buffer, GimvCommentView *cv)
{
   if (cv->comment) {
      cv->changed = TRUE;
   }
}


static GtkWidget *
create_note_page (GimvCommentView *cv)
{
   GtkWidget *scrolledwin;
   GtkWidget *label;

   /* "Note" page */
   label = gtk_label_new (_("Note"));
   gtk_widget_set_name (label, "TabLabel");
   gtk_widget_set_visible (label, TRUE);

   cv->note_page = scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gtk_widget_set_visible (scrolledwin, TRUE);

   {
      GtkTextBuffer *textbuf;

      cv->note_box = gtk_text_view_new ();
      textbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (cv->note_box));
      g_signal_connect (G_OBJECT (textbuf), "changed",
                        G_CALLBACK (cb_text_buffer_changed), cv);
   }
   gimv_container_add (GTK_WIDGET (scrolledwin), cv->note_box);
   gtk_widget_set_visible (cv->note_box, TRUE);

   gtk_notebook_append_page (GTK_NOTEBOOK(cv->notebook),
                             scrolledwin, label);

   return cv->note_page;
}


static void
gimv_comment_view_set_sensitive_all (GimvCommentView *cv, gboolean sensitive)
{
   g_return_if_fail (cv);

   gtk_widget_set_sensitive (cv->comment_clist, sensitive);
   gtk_widget_set_sensitive (cv->note_box, sensitive);

   gtk_widget_set_sensitive (cv->key_combo, sensitive);
   gtk_widget_set_sensitive (cv->value_entry, sensitive);

   gtk_widget_set_sensitive (cv->save_button, sensitive);
   gtk_widget_set_sensitive (cv->reset_button, sensitive);
   gtk_widget_set_sensitive (cv->delete_button, sensitive);
}


static void
gimv_comment_view_set_sensitive (GimvCommentView *cv)
{
   const gchar *key_str, *value_str;
   gboolean selected = FALSE;

   g_return_if_fail (cv);

   key_str   = combo_get_selected_name (cv);
   value_str = gtk_editable_get_text (GTK_EDITABLE (cv->value_entry));

   if (!cv->comment || !gtk_widget_get_visible (GTK_WIDGET (cv->button_area))) {
      gimv_comment_view_set_sensitive_all (cv, FALSE);
      return;
   } else {
      gimv_comment_view_set_sensitive_all (cv, TRUE);
   }

   selected = gimv_list_view_get_selected (cv->comment_clist)
      != GTK_INVALID_LIST_POSITION;
}


static void
gimv_comment_view_set_combo_list (GimvCommentView *cv)
{
   GList *list;
   GPtrArray *keys, *names;
   GtkDropDown *combo;
   GtkStringList *strings;

   g_return_if_fail (cv);
   g_return_if_fail (cv->key_combo);

   combo = GTK_DROP_DOWN (cv->key_combo);

   g_signal_handlers_block_by_func (G_OBJECT (combo),
                                    G_CALLBACK (cb_combo_changed), cv);

   strings = gtk_string_list_new (NULL);
   cv->selected_item = NULL;

   keys  = g_ptr_array_new_with_free_func (g_free);
   names = g_ptr_array_new_with_free_func (g_free);
   g_object_set_data_full (G_OBJECT (combo), "keys", keys,
                           (GDestroyNotify) g_ptr_array_unref);
   g_object_set_data_full (G_OBJECT (combo), "names", names,
                           (GDestroyNotify) g_ptr_array_unref);

   list = gimv_comment_get_data_entry_list ();
   while (list) {
      GimvCommentDataEntry *data_entry = list->data;
      GimvImageInfo *info;

      list = g_list_next (list);

      if (!data_entry) continue;
      if (!data_entry->display) continue;
      if (!data_entry->key || !*data_entry->key) continue;

      if (cv->comment)
         info = cv->comment->info;
      else
         info = NULL;

      if (!strcmp ("X-IMG-File-Path-In-Arcvhie", data_entry->key)
          && info && !gimv_image_info_is_in_archive (info))
      {
         continue;
      }

      g_ptr_array_add (keys,  g_strdup (data_entry->key));
      g_ptr_array_add (names, g_strdup (_(data_entry->display_name)));
      gtk_string_list_append (strings, _(data_entry->display_name));
   }

   gtk_drop_down_set_model (combo, G_LIST_MODEL (strings));
   g_object_unref (strings);
   gtk_drop_down_set_selected (combo, GTK_INVALID_LIST_POSITION);

   g_signal_handlers_unblock_by_func (G_OBJECT (combo),
                                      G_CALLBACK (cb_combo_changed), cv);

   /* select first item */
   if (keys->len > 0)
      gtk_drop_down_set_selected (combo, 0);
}


static void
gimv_comment_view_data_enter (GimvCommentView *cv)
{
   GimvCommentDataEntry *entry;
   const gchar *key, *dname, *value;
   gchar *text[16];

   g_return_if_fail (cv);
   if (!cv->selected_item) return;

   key = g_object_get_data (G_OBJECT (cv->selected_item), "key");
   g_return_if_fail (key);

   dname = combo_get_selected_name (cv);
   value = gtk_editable_get_text (GTK_EDITABLE (cv->value_entry));
   g_return_if_fail (dname && *dname);

   entry = gimv_comment_append_data (cv->comment, key, value);

   g_return_if_fail (entry);

   {
      GListModel *model = gimv_list_view_get_model (cv->comment_clist);
      guint i, n = g_list_model_get_n_items (model);
      GimvRow *row = NULL;

      for (i = 0; i < n; i++) {
         GimvRow *r = gimv_row_store_peek (model, i);
         if (gimv_row_peek_pointer (r, COLUMN_RAW_ENTRY) == entry) {
            row = r;
            break;
         }
      }

      text[0] = entry->display_name;
      text[1] = entry->value;
      if (!entry->userdef) text[0] = _(text[0]);

      if (row) {
         g_object_ref (row);
      } else {
         row = comment_row_new ();
         g_list_store_append (G_LIST_STORE (model), row);
      }
      gimv_row_set (row,
                    COLUMN_KEY,       text[0],
                    COLUMN_VALUE,     text[1],
                    COLUMN_RAW_ENTRY, entry,
                    COLUMN_TERMINATOR);
      g_object_unref (row);
   }

   cv->changed = TRUE;

   gimv_comment_view_set_sensitive (cv);
}


static void
gimv_comment_view_reset_data (GimvCommentView *cv)
{
   GimvImageInfo *info;
   GList *node;
   gchar *text[2];

   gimv_comment_view_clear (cv);
   if (cv->comment) {
      info = cv->comment->info;
      node = cv->comment->data_list;
      while (node) {
         GimvCommentDataEntry *entry = node->data;

         node = g_list_next (node);

         if (!entry) continue;
         if (!entry->display) continue;

         if (!strcmp ("X-IMG-File-Path-In-Arcvhie", entry->key)
             && info && !gimv_image_info_is_in_archive (info))
         {
            continue;
         }

         text[0] = _(entry->display_name);
         text[1] = entry->value;

         {
            GListModel *model = gimv_list_view_get_model (cv->comment_clist);
            GimvRow *row = comment_row_new ();

            gimv_row_set (row,
                          COLUMN_KEY,       text[0],
                          COLUMN_VALUE,     text[1],
                          COLUMN_RAW_ENTRY, entry,
                          COLUMN_TERMINATOR);
            g_list_store_append (G_LIST_STORE (model), row);
            g_object_unref (row);
         }
      }

      if (cv->comment->note && *cv->comment->note) {
         GtkTextBuffer *buffer;

         buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (cv->note_box));
         gtk_text_buffer_set_text (buffer, cv->comment->note, -1);
      }
   }

   cv->changed = FALSE;

   gimv_comment_view_set_sensitive (cv);
}


/******************************************************************************
 *
 *   public functions.
 *
 ******************************************************************************/
void
gimv_comment_view_clear (GimvCommentView *cv)
{
   g_return_if_fail (cv);

   g_list_store_remove_all (G_LIST_STORE (gimv_list_view_get_model (cv->comment_clist)));

   gtk_drop_down_set_selected (GTK_DROP_DOWN (cv->key_combo),
                               GTK_INVALID_LIST_POSITION);
   gtk_editable_set_text (GTK_EDITABLE (cv->value_entry), "\0");

   {
      GtkTextBuffer *buffer;
      buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (cv->note_box));
      gtk_text_buffer_set_text (buffer, "\0", -1);
   }

   cv->changed = FALSE;

   gimv_comment_view_set_sensitive (cv);
}


gboolean
gimv_comment_view_change_file (GimvCommentView *cv, GimvImageInfo *info)
{
   g_return_val_if_fail (cv, FALSE);

   gimv_comment_view_clear (cv);

   if (cv->comment) {
      gimv_comment_unref (cv->comment);
      cv->comment = NULL;
   }

   cv->comment = gimv_comment_get_from_image_info (info);
   g_signal_connect (G_OBJECT (cv->comment), "file_saved",
                       G_CALLBACK (cb_file_saved), cv);

   gimv_comment_view_reset_data (cv);
   gimv_comment_view_set_combo_list (cv);

   return TRUE;
}


GimvCommentView *
gimv_comment_view_create (void)
{
   GimvCommentView *cv;
   GtkWidget *hbox, *hbox1;
   GtkWidget *button;

   cv = gimv_comment_view_new ();

   cv->accel_group = NULL; /* GTK4: mnemonics are used instead */
   cv->iv = NULL;
   cv->next_button = NULL;
   cv->prev_button = NULL;
   cv->changed = FALSE;

   cv->main_vbox = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_name (cv->main_vbox, "GimvCommentView");
   g_signal_connect (G_OBJECT (cv->main_vbox), "destroy",
                       G_CALLBACK (cb_destroyed), cv);

   cv->notebook = gtk_notebook_new ();
   gimv_container_set_border_width (GTK_WIDGET (cv->notebook), 1);
   gtk_notebook_set_scrollable (GTK_NOTEBOOK (cv->notebook), TRUE);
   gimv_box_pack_start(GTK_BOX(cv->main_vbox), cv->notebook, TRUE, TRUE, 0);
   gtk_widget_set_visible (cv->notebook, TRUE);

   g_signal_connect (G_OBJECT (cv->notebook), "switch-page",
                       G_CALLBACK(cb_switch_page), cv);
   g_object_add_weak_pointer (G_OBJECT (cv->notebook), (gpointer *) &cv->notebook);

   create_data_page (cv);
   create_note_page (cv);

   /* button area */
   hbox = cv->button_area = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start(GTK_BOX(cv->main_vbox), cv->button_area, FALSE, FALSE, 2);
   gtk_widget_set_visible (cv->main_vbox, TRUE);

   hbox1 = cv->inner_button_area = gimv_hbox_new (TRUE, 0);
   gimv_box_pack_end (GTK_BOX (hbox), hbox1, FALSE, TRUE, 0);
   gtk_box_set_homogeneous (GTK_BOX(hbox1), FALSE);

   button = gtk_button_new_with_mnemonic (_("_Save"));
   cv->save_button = button;
   g_signal_connect (G_OBJECT (button),"clicked",
                       G_CALLBACK (cb_save_button_pressed), cv);
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);

   button = gtk_button_new_with_mnemonic (_("_Reset"));
   cv->reset_button = button;
   g_signal_connect (G_OBJECT (button),"clicked",
                       G_CALLBACK (cb_reset_button_pressed), cv);
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);

   button = gtk_button_new_with_mnemonic (_("_Delete"));
   cv->delete_button = button;
   g_signal_connect (G_OBJECT (button),"clicked",
                       G_CALLBACK (cb_del_button_pressed), cv);
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);

   gimv_widget_show_all (cv->main_vbox);

   gimv_comment_view_set_sensitive (cv);

   return cv;
}


static void
cb_prev_button_pressed (GtkButton *button, GimvCommentView *cv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (cv->iv));

   gimv_image_view_prev (cv->iv);
}


static void
cb_next_button_pressed (GtkButton *button, GimvCommentView *cv)
{
   g_return_if_fail (GIMV_IS_IMAGE_VIEW (cv->iv));

   gimv_image_view_next (cv->iv);
}


GimvCommentView *
gimv_comment_view_create_window (GimvImageInfo *info)
{
   GimvCommentView *cv;
   gchar buf[BUF_SIZE];

   g_return_val_if_fail (info, NULL);

   cv = gimv_comment_view_create ();
   if (!cv) return NULL;

   gimv_container_set_border_width (GTK_WIDGET (cv->main_vbox), 5);

   cv->window = gtk_window_new ();
   g_snprintf (buf, BUF_SIZE, _("Edit Comment (%s)"),
               gimv_image_info_get_path (info));
   gtk_window_set_title (GTK_WINDOW (cv->window), buf); 
   gtk_window_set_default_size (GTK_WINDOW (cv->window), 400, 350);
   /* GTK4: gtk_window_set_position (GTK_WIN_POS_MOUSE) is not available */
   gimv_container_add (GTK_WIDGET (cv->window), cv->main_vbox);

   /* GTK4 port: a Close button and Esc for the stand-alone window (the
      Save / Reset / Delete row is hidden on some pages, so it has its own
      row) */
   {
      GtkWidget *hbox, *button;
      GtkEventController *keys;
      GtkShortcut *shortcut;

      hbox = gimv_hbox_new (FALSE, 0);
      gimv_box_pack_end (GTK_BOX (cv->main_vbox), hbox, FALSE, FALSE, 2);
      button = gtk_button_new_with_mnemonic (_("_Close"));
      g_signal_connect_swapped (button, "clicked",
                                G_CALLBACK (gtk_window_destroy), cv->window);
      gimv_box_pack_end (GTK_BOX (hbox), button, FALSE, FALSE, 2);

      keys = gtk_shortcut_controller_new ();
      shortcut = gtk_shortcut_new (gtk_keyval_trigger_new (GDK_KEY_Escape, 0),
                                   gtk_named_action_new ("window.close"));
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
                                            shortcut);
      gtk_widget_add_controller (cv->window, keys);
   }

   gimv_widget_show_all (cv->window);

   gimv_comment_view_change_file (cv, info);

   gimv_icon_stock_set_window_icon (cv->window, "gimv_icon");

   return cv;
}


/* FIXME */
void
gimv_comment_view_set_image_view (GimvCommentView *cv, GimvImageView *iv)
{
   g_return_if_fail (cv);

   /* add next and prev button */
   if (iv) {
      GtkWidget *hbox, *sep, *button;

      /* cv->iv = iv; */

      hbox = gimv_hbox_new (TRUE, 0);
      gimv_box_pack_start (GTK_BOX (cv->button_area), hbox, TRUE, TRUE, 0);

      sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
      gimv_box_pack_start (GTK_BOX (cv->button_area),
                          sep, TRUE, TRUE, 2);

      button = gtk_button_new_with_mnemonic (_("_Prev"));
      cv->prev_button = button;
      g_signal_connect (G_OBJECT (button),"clicked",
                          G_CALLBACK (cb_prev_button_pressed), cv);
      gimv_box_pack_start (GTK_BOX (hbox),
                          button, FALSE, TRUE, 2);

      button = gtk_button_new_with_mnemonic (_("_Next"));
      cv->next_button = button;
      g_signal_connect (G_OBJECT (button),"clicked",
                          G_CALLBACK (cb_next_button_pressed), cv);
      gimv_box_pack_start (GTK_BOX (hbox),
                          button, FALSE, TRUE, 2);
   }
}
