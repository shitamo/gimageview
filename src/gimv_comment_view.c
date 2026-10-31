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
#ifdef ENABLE_INDEX
#  include "gimv_index.h"
#  include "gimv_index_view.h"
#  include "gimv_thumb_win.h"
#endif
#include "dnd.h"
#include "gimv_comment.h"
#include "gimv_file_comment.h"
#include "gimv_icon_stock.h"
#include "gimv_image_info.h"
#include "gimv_image_view.h"
#include "prefs.h"


typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_KEY,
   COLUMN_VALUE,
   COLUMN_RAW_ENTRY,
   COLUMN_FILE,       /* a comment written in the image file: shown only */
   COLUMN_STYLE,      /* PangoStyle: those in italics */
   COLUMN_TIP,        /* their whole text (markup) */
   N_COLUMN
} ListStoreColumn;


static void gimv_comment_view_set_sensitive  (GimvCommentView *cv);
static void gimv_comment_view_set_combo_list (GimvCommentView *cv);
static void gimv_comment_view_data_enter     (GimvCommentView *cv);
static void gimv_comment_view_reset_data     (GimvCommentView *cv);


/* the row selected is a comment of the image file (not to edit) */
static gboolean
selected_is_file (GimvCommentView *cv)
{
   GtkTreeSelection *selection;
   GtkTreeModel *model;
   GtkTreeIter iter;
   gboolean file = FALSE;

   if (!cv->comment_clist) return FALSE;
   selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (cv->comment_clist));
   if (gtk_tree_selection_get_selected (selection, &model, &iter))
      gtk_tree_model_get (model, &iter, COLUMN_FILE, &file, COLUMN_TERMINATOR);
   return file;
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
      gtk_widget_show (cv->button_area);
   else
      gtk_widget_hide (cv->button_area);

   gimv_comment_view_set_sensitive (cv);
}


static void
gimv_comment_view_delete_selected_data (GimvCommentView *cv)
{
   GimvCommentDataEntry *entry;

   g_return_if_fail (cv);
   if (selected_is_file (cv)) return;

   {
      GtkTreeView *treeview = GTK_TREE_VIEW (cv->comment_clist);
      GtkTreeSelection *selection  = gtk_tree_view_get_selection (treeview);
      GtkTreeModel *model;
      GtkTreeIter iter;
      gboolean found;

      found = gtk_tree_selection_get_selected (selection, &model, &iter);
      if (!found) return;

      gtk_tree_model_get (model, &iter,
                          COLUMN_RAW_ENTRY, &entry,
                          COLUMN_TERMINATOR);
      gtk_list_store_remove (GTK_LIST_STORE (model), &iter);
   }

   if (entry)
      gimv_comment_data_entry_remove (cv->comment, entry);

   gtk_editable_set_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo))), "\0");
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

#ifdef ENABLE_INDEX
   if (cv->tags_watch) gimv_index_remove_watch (cv->tags_watch);
   if (cv->tags_cancel) {
      g_cancellable_cancel (cv->tags_cancel);
      g_clear_object (&cv->tags_cancel);
   }
#endif

   g_free (cv);
}


static void
cb_tree_view_cursor_changed (GtkTreeView *treeview, GimvCommentView *cv)
{
   GtkTreeSelection *selection = gtk_tree_view_get_selection (treeview);
   GtkTreeModel *model;
   GtkTreeIter iter;
   gboolean success;
   gchar *key = NULL, *value = NULL;
   gboolean file = FALSE;
   GtkEntry *entry1, *entry2;

   /* GTK4: also emitted while the tree view is disposed (without model) */
   if (!gtk_tree_view_get_model (treeview)) return;

   entry1 = GTK_ENTRY (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo)));
   entry2 = GTK_ENTRY(cv->value_entry);

   success = gtk_tree_selection_get_selected (selection, &model, &iter);
   if (success) {
      gtk_tree_model_get (model, &iter,
                          COLUMN_KEY,   &key,
                          COLUMN_VALUE, &value,
                          COLUMN_FILE,  &file,
                          COLUMN_TERMINATOR);
   }
   /* a comment of the file: its whole text (the list shows its first line) */
   if (file) {
      gchar *tip = NULL, *text = NULL;
      gtk_tree_model_get (model, &iter, COLUMN_TIP, &tip, COLUMN_TERMINATOR);
      if (tip && pango_parse_markup (tip, -1, 0, NULL, &text, NULL, NULL)) {
         g_free (value);
         value = g_strdelimit (text, "\n", ' ');
      }
      g_free (tip);
   }

   if (key)
      gtk_editable_set_text (GTK_EDITABLE (entry1), key);
   else
      gtk_editable_set_text (GTK_EDITABLE (entry1), "\0");
   if (value)
      gtk_editable_set_text (GTK_EDITABLE (entry2), value);
   else
      gtk_editable_set_text (GTK_EDITABLE (entry2), "\0");

   g_free (key);
   g_free (value);

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
 *  GTK4: GtkCombo is gone, the key combo is a GtkComboBoxText with an entry.
 *  The keys and the display names of the items are attached to the combo as
 *  "keys" and "names" (GPtrArray).  When an item is selected (or the entry
 *  text matches an item, like GtkCombo did), the key of it is attached as
 *  "key" and cv->selected_item points to the combo.
 */
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
      GtkTreeView *treeview = GTK_TREE_VIEW (clist);
      GtkTreeModel *model = gtk_tree_view_get_model (treeview);
      GtkTreeIter iter;
      gboolean go_next;

      go_next = gtk_tree_model_get_iter_first (model, &iter);

      for (; go_next; go_next = gtk_tree_model_iter_next (model, &iter)) {
         GimvCommentDataEntry *entry;

         gtk_tree_model_get (model, &iter,
                             COLUMN_RAW_ENTRY, &entry,
                             COLUMN_TERMINATOR);
         if (!entry) continue;

         if (entry->key && !strcmp (key, entry->key)) {
            GtkTreePath *treepath = gtk_tree_model_get_path (model, &iter);
            GtkTreePath *cursor = NULL;

            if (!treepath) continue;
            gtk_tree_view_get_cursor (treeview, &cursor, NULL);
            if (!cursor || gtk_tree_path_compare (cursor, treepath))
               gtk_tree_view_set_cursor (treeview, treepath, NULL, FALSE);
            if (cursor)
               gtk_tree_path_free (cursor);
            gtk_tree_path_free (treepath);
            break;
         }
      }
   }
}


static void
cb_combo_changed (GtkComboBox *combo, GimvCommentView *cv)
{
   GPtrArray *keys, *names;
   gint idx;

   g_return_if_fail (cv);

   keys  = g_object_get_data (G_OBJECT (combo), "keys");
   names = g_object_get_data (G_OBJECT (combo), "names");
   if (!keys || !names) {
      cv->selected_item = NULL;
      return;
   }

   idx = gtk_combo_box_get_active (combo);
   if (idx < 0) {
      /* find the item which matches with the entry text */
      const gchar *text;
      guint i;

      text = gtk_editable_get_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (combo))));
      for (i = 0; text && i < names->len; i++) {
         if (!strcmp (text, g_ptr_array_index (names, i))) {
            idx = i;
            break;
         }
      }
   }

   if (idx >= 0 && (guint) idx < keys->len)
      combo_select_key (cv, g_ptr_array_index (keys, idx));
   else
      cv->selected_item = NULL;
}


static void
cb_combo_entry_changed (GtkEditable *entry, GimvCommentView *cv)
{
   g_return_if_fail (cv);
   if (!cv->key_combo) return;

   cb_combo_changed (GTK_COMBO_BOX (cv->key_combo), cv);
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


/******************************************************************************
 *  the tags of the image in the index, above the data: each a button that
 *  shows the images with the tag.  Read in a worker thread.
 ******************************************************************************/
#ifdef ENABLE_INDEX

#define TAG_RULE_COLOR "#3584e4"   /* as in the tag tab */

static void
cb_tag_clicked (GtkButton *button, gpointer data)
{
   const gchar *key = g_object_get_data (G_OBJECT (button), "gimv-tag-key");
   GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
   GimvThumbWin *tw = NULL;

   if (GIMV_IS_THUMB_WIN (root)) {
      tw = GIMV_THUMB_WIN (root);
   } else {
      /* the comment window, the image window: the first thumbnail window */
      GList *list = gimv_thumb_win_get_list ();
      if (list) tw = list->data;
   }
   if (tw && key) {
      gimv_index_open_tag (tw, key);
      gtk_window_present (GTK_WINDOW (tw));
   }
}


static void
tags_clear (GimvCommentView *cv)
{
   GtkWidget *child;

   if (!cv->tags_flow) return;
   while ((child = gtk_widget_get_first_child (cv->tags_flow)))
      gtk_flow_box_remove (GTK_FLOW_BOX (cv->tags_flow), child);
}


static void
cb_got_tags (GObject *source, GAsyncResult *res, gpointer data)
{
   GimvCommentView *cv = data;
   GPtrArray *keys;
   guint i;

   /* cv may be gone (cancelled then) */
   if (g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res)))) return;
   keys = gimv_index_get_tags_finish (res);
   if (!cv->tags_flow) {
      if (keys) g_ptr_array_unref (keys);
      return;
   }

   tags_clear (cv);
   /* not in the index (in an archive, a folder not indexed): no row */
   gtk_widget_set_visible (cv->tags_row, keys != NULL);
   if (!keys) return;

   if (!keys->len) {
      GtkWidget *none = gtk_label_new (_("none"));
      gtk_widget_add_css_class (none, "dim-label");
      gtk_flow_box_append (GTK_FLOW_BOX (cv->tags_flow), none);
   }
   for (i = 0; i < keys->len; i++) {
      const gchar *key = g_ptr_array_index (keys, i);
      gint kind = gimv_index_tag_kind (key);
      gchar *text = gimv_index_tag_label (key);
      GtkWidget *button = gtk_button_new ();
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
      GtkWidget *label = gtk_label_new (NULL);

      if (kind == 1)
         gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("avatar-default-symbolic"));
      if (kind >= 2) {
         /* the tags of rules and of people: kept in the index only */
         gchar *markup = g_markup_printf_escaped ("<span foreground=\"" TAG_RULE_COLOR "\">%s</span>",
                                                  text);
         gtk_label_set_markup (GTK_LABEL (label), markup);
         g_free (markup);
      } else {
         gtk_label_set_text (GTK_LABEL (label), text);
      }
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 30);
      gtk_box_append (GTK_BOX (box), label);
      gtk_button_set_child (GTK_BUTTON (button), box);
      gtk_widget_add_css_class (button, "flat");
      gtk_widget_set_tooltip_text (button,
         kind == 0 ? _("A tag put by a person: click to show the images with it")
         : kind == 1 ? _("A person in the image: click to show the images of the person")
         : kind == 2 ? _("A tag of a person in the image: click to show the images of these people")
         : _("An automatic tag: click to show the images with it"));
      g_object_set_data_full (G_OBJECT (button), "gimv-tag-key", g_strdup (key), g_free);
      g_signal_connect (button, "clicked", G_CALLBACK (cb_tag_clicked), NULL);
      gtk_flow_box_append (GTK_FLOW_BOX (cv->tags_flow), button);
      g_free (text);
   }
   g_ptr_array_unref (keys);
}


static void
tags_refresh (GimvCommentView *cv)
{
   GimvImageInfo *info = cv->comment ? cv->comment->info : NULL;

   if (!cv->tags_flow) return;
   if (cv->tags_cancel) {
      g_cancellable_cancel (cv->tags_cancel);
      g_clear_object (&cv->tags_cancel);
   }
   if (!info || gimv_image_info_is_in_archive (info) || !gimv_index_is_configured ()) {
      tags_clear (cv);
      gtk_widget_set_visible (cv->tags_row, FALSE);
      return;
   }
   /* the old ones stay until the new ones come (no flicker) */
   cv->tags_cancel = g_cancellable_new ();
   gimv_index_get_tags_async (gimv_image_info_get_path (info), cv->tags_cancel,
                              cb_got_tags, cv);
}


static void
cb_index_changed (GimvIndexChange what, gpointer data)
{
   if (what & (GIMV_INDEX_CHANGED_TAGS | GIMV_INDEX_CHANGED_FACES))
      tags_refresh (data);
}


static void
create_tags_row (GimvCommentView *cv, GtkWidget *vbox)
{
   GtkWidget *label;

   cv->tags_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
   gtk_widget_set_margin_start (cv->tags_row, 4);
   gtk_widget_set_margin_end (cv->tags_row, 4);
   gtk_widget_set_margin_top (cv->tags_row, 2);
   gtk_widget_set_margin_bottom (cv->tags_row, 2);
   label = gtk_label_new (_("Tags:"));
   gtk_widget_set_valign (label, GTK_ALIGN_START);
   gtk_widget_set_margin_top (label, 6);
   gtk_box_append (GTK_BOX (cv->tags_row), label);

   cv->tags_flow = gtk_flow_box_new ();
   gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (cv->tags_flow), GTK_SELECTION_NONE);
   gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (cv->tags_flow), 30);
   gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (cv->tags_flow), 0);
   gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (cv->tags_flow), 0);
   gtk_widget_set_hexpand (cv->tags_flow, TRUE);
   gtk_box_append (GTK_BOX (cv->tags_row), cv->tags_flow);

   gtk_widget_set_visible (cv->tags_row, FALSE);
   gtk_box_append (GTK_BOX (vbox), cv->tags_row);
   cv->tags_watch = gimv_index_add_watch (cb_index_changed, cv);
}

#endif /* ENABLE_INDEX */


static GtkWidget *
create_data_page (GimvCommentView *cv)
{
   GtkWidget *vbox, *vbox1, *hbox, *hbox1;
   GtkWidget *scrolledwin, *clist, *combo, *entry;
   GtkWidget *label;
   gchar *titles[] = {_("Key"), _("Value")};

   label = gtk_label_new (_("Data"));
   gtk_widget_set_name (label, "TabLabel");
   gtk_widget_show (label);
   cv->data_page = vbox = gimv_vbox_new (FALSE, 0);
   gtk_widget_show (vbox);

   gtk_notebook_append_page (GTK_NOTEBOOK(cv->notebook),
                             vbox, label);

#ifdef ENABLE_INDEX
   create_tags_row (cv, vbox);
#endif

   /* scrolled window & clist */
   scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gimv_box_pack_start(GTK_BOX(vbox), scrolledwin, TRUE, TRUE, 0);

{
   GtkListStore *store;
   GtkTreeViewColumn *col;
   GtkCellRenderer *render;

   store = gtk_list_store_new (N_COLUMN,
                               G_TYPE_STRING,
                               G_TYPE_STRING,
                               G_TYPE_POINTER,
                               G_TYPE_BOOLEAN,
                               G_TYPE_INT,
                               G_TYPE_STRING);
   clist =  gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
   gimv_tree_view_widen_column_resize (GTK_TREE_VIEW (clist));
   cv->comment_clist = clist;

   /* GTK4: gtk_tree_view_set_rules_hint () removed */

   /* set column for key */
   col = gtk_tree_view_column_new ();
   gtk_tree_view_column_set_resizable (col, TRUE);
   gtk_tree_view_column_set_title (col, titles[0]);
   render = gtk_cell_renderer_text_new ();
   gtk_tree_view_column_pack_start (col, render, TRUE);
   gtk_tree_view_column_add_attribute (col, render, "text", COLUMN_KEY);
   gtk_tree_view_column_add_attribute (col, render, "style", COLUMN_STYLE);
   gtk_tree_view_append_column (GTK_TREE_VIEW (clist), col);

   /* set column for value */
   col = gtk_tree_view_column_new ();
   gtk_tree_view_column_set_resizable (col, TRUE);
   gtk_tree_view_column_set_title (col, titles[1]);
   render = gtk_cell_renderer_text_new ();
   gtk_tree_view_column_pack_start (col, render, TRUE);
   gtk_tree_view_column_add_attribute (col, render, "text", COLUMN_VALUE);
   gtk_tree_view_column_add_attribute (col, render, "style", COLUMN_STYLE);
   gtk_tree_view_append_column (GTK_TREE_VIEW (clist), col);
   gtk_tree_view_set_tooltip_column (GTK_TREE_VIEW (clist), COLUMN_TIP);

   g_signal_connect (G_OBJECT (clist),"cursor-changed",
                       G_CALLBACK (cb_tree_view_cursor_changed), cv);

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

   cv->key_combo = combo = gimv_combo_new ();
   gimv_box_pack_start (GTK_BOX (vbox1), combo, TRUE, TRUE, 0);
   gtk_editable_set_editable (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo))), FALSE);
   g_signal_connect (G_OBJECT (combo), "changed",
                     G_CALLBACK (cb_combo_changed), cv);
   g_signal_connect (G_OBJECT (gimv_combo_get_entry (combo)), "changed",
                     G_CALLBACK (cb_combo_entry_changed), cv);

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
   gtk_widget_show (label);

   cv->note_page = scrolledwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW(scrolledwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrolledwin), TRUE);
   gtk_widget_show (scrolledwin);

   {
      GtkTextBuffer *textbuf;

      cv->note_box = gtk_text_view_new ();
      textbuf = gtk_text_view_get_buffer (GTK_TEXT_VIEW (cv->note_box));
      g_signal_connect (G_OBJECT (textbuf), "changed",
                        G_CALLBACK (cb_text_buffer_changed), cv);
   }
   gimv_container_add (GTK_WIDGET (scrolledwin), cv->note_box);
   gtk_widget_show (cv->note_box);

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

   key_str   = gtk_editable_get_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo))));
   value_str = gtk_editable_get_text (GTK_EDITABLE (cv->value_entry));

   if (!cv->comment || !gtk_widget_get_visible (GTK_WIDGET (cv->button_area))) {
      gimv_comment_view_set_sensitive_all (cv, FALSE);
      return;
   } else {
      gimv_comment_view_set_sensitive_all (cv, TRUE);
   }

   {
      GtkTreeView *treeview = GTK_TREE_VIEW (cv->comment_clist);
      GtkTreeSelection *selection = gtk_tree_view_get_selection (treeview);
      GtkTreeModel *model;
      GtkTreeIter iter;

      selected = gtk_tree_selection_get_selected (selection, &model, &iter);
   }

   /* a comment of the image file is only shown */
   {
      gboolean file = selected && selected_is_file (cv);
      gtk_editable_set_editable (GTK_EDITABLE (cv->value_entry), !file);
      gtk_widget_set_sensitive (cv->key_combo, !file);
      gtk_widget_set_sensitive (cv->delete_button, !file);
   }
}


static void
gimv_comment_view_set_combo_list (GimvCommentView *cv)
{
   GList *list;
   GPtrArray *keys, *names;
   GtkComboBox *combo;

   g_return_if_fail (cv);
   g_return_if_fail (cv->key_combo);

   combo = GTK_COMBO_BOX (cv->key_combo);

   g_signal_handlers_block_by_func (G_OBJECT (combo),
                                    G_CALLBACK (cb_combo_changed), cv);
   g_signal_handlers_block_by_func (G_OBJECT (gimv_combo_get_entry (GTK_WIDGET (combo))),
                                    G_CALLBACK (cb_combo_entry_changed), cv);

   gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
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

      if (!strcmp ("X-IMG-File-Path-In-Archive", data_entry->key)
          && info && !gimv_image_info_is_in_archive (info))
      {
         continue;
      }

      g_ptr_array_add (keys,  g_strdup (data_entry->key));
      g_ptr_array_add (names, g_strdup (_(data_entry->display_name)));
      gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo),
                                      _(data_entry->display_name));
   }

   g_signal_handlers_unblock_by_func (G_OBJECT (combo),
                                      G_CALLBACK (cb_combo_changed), cv);
   g_signal_handlers_unblock_by_func (G_OBJECT (gimv_combo_get_entry (GTK_WIDGET (combo))),
                                      G_CALLBACK (cb_combo_entry_changed), cv);

   /* select first item */
   if (keys->len > 0)
      gtk_combo_box_set_active (combo, 0);
}


static void
gimv_comment_view_data_enter (GimvCommentView *cv)
{
   GimvCommentDataEntry *entry;
   const gchar *key, *dname, *value;
   gchar *text[16];

   g_return_if_fail (cv);
   if (!cv->selected_item) return;
   if (selected_is_file (cv)) return;

   key = g_object_get_data (G_OBJECT (cv->selected_item), "key");
   g_return_if_fail (key);

   dname = gtk_editable_get_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo))));
   value = gtk_editable_get_text (GTK_EDITABLE (cv->value_entry));
   g_return_if_fail (dname && *dname);

   entry = gimv_comment_append_data (cv->comment, key, value);

   g_return_if_fail (entry);

   {
      GtkTreeView *treeview = GTK_TREE_VIEW (cv->comment_clist);
      GtkTreeModel *model = gtk_tree_view_get_model (treeview);
      GtkTreeIter iter;
      gboolean go_next;
      GimvCommentDataEntry *src_entry;

      go_next = gtk_tree_model_get_iter_first (model, &iter);
      for (; go_next; go_next = gtk_tree_model_iter_next (model, &iter)) {
         gtk_tree_model_get (model, &iter,
                             COLUMN_RAW_ENTRY, &src_entry,
                             COLUMN_TERMINATOR);
         if (src_entry == entry) break;
      }

      text[0] = entry->display_name;
      text[1] = entry->value;
      if (!entry->userdef) text[0] = _(text[0]);

      if (!go_next)
         gtk_list_store_append (GTK_LIST_STORE (model), &iter);
      gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                          COLUMN_KEY,       text[0],
                          COLUMN_VALUE,     text[1],
                          COLUMN_RAW_ENTRY, entry,
                          COLUMN_TERMINATOR);
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

         if (!strcmp ("X-IMG-File-Path-In-Archive", entry->key)
             && info && !gimv_image_info_is_in_archive (info))
         {
            continue;
         }

         text[0] = _(entry->display_name);
         text[1] = entry->value;

         {
            GtkTreeModel *model;
            GtkTreeIter iter;
            model = gtk_tree_view_get_model (GTK_TREE_VIEW (cv->comment_clist));
            gtk_list_store_append (GTK_LIST_STORE (model), &iter);
            gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                                COLUMN_KEY,       text[0],
                                COLUMN_VALUE,     text[1],
                                COLUMN_RAW_ENTRY, entry,
                                COLUMN_TERMINATOR);
         }
      }

      /* the comments written in the file, after the data (in italics:
       * shown only, not saved with the data) */
      if (info && !gimv_image_info_is_in_archive (info)) {
         GPtrArray *list = gimv_file_comment_read (gimv_image_info_get_path (info));
         GtkTreeModel *model = gtk_tree_view_get_model (GTK_TREE_VIEW (cv->comment_clist));
         guint i;

         for (i = 0; i < list->len; i++) {
            GimvFileComment *c = g_ptr_array_index (list, i);
            const gchar *nl = strchr (c->text, '\n');
            gchar *name = g_strdup_printf (_("In File: %s"), c->name);
            gchar *value = nl ? g_strdup_printf ("%.*s …", (gint) (nl - c->text), c->text)
                              : g_strdup (c->text);
            gchar *tip = g_markup_escape_text (c->text, -1);
            GtkTreeIter iter;

            gtk_list_store_append (GTK_LIST_STORE (model), &iter);
            gtk_list_store_set (GTK_LIST_STORE (model), &iter,
                                COLUMN_KEY,       name,
                                COLUMN_VALUE,     value,
                                COLUMN_RAW_ENTRY, NULL,
                                COLUMN_FILE,      TRUE,
                                COLUMN_STYLE,     PANGO_STYLE_ITALIC,
                                COLUMN_TIP,       tip,
                                COLUMN_TERMINATOR);
            g_free (name);
            g_free (value);
            g_free (tip);
         }
         g_ptr_array_unref (list);
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

   {
      GtkTreeModel *model
         = gtk_tree_view_get_model (GTK_TREE_VIEW (cv->comment_clist));
      gtk_list_store_clear (GTK_LIST_STORE (model));
   }

   gtk_editable_set_text (GTK_EDITABLE (gimv_combo_get_entry (GTK_WIDGET (cv->key_combo))), "\0");
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
#ifdef ENABLE_INDEX
   tags_refresh (cv);
#endif

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
   gtk_widget_show (cv->notebook);

   g_signal_connect (G_OBJECT (cv->notebook), "switch-page",
                       G_CALLBACK(cb_switch_page), cv);
   g_object_add_weak_pointer (G_OBJECT (cv->notebook), (gpointer *) &cv->notebook);

   create_data_page (cv);
   create_note_page (cv);

   /* button area */
   hbox = cv->button_area = gimv_hbox_new (FALSE, 0);
   gimv_box_pack_start(GTK_BOX(cv->main_vbox), cv->button_area, FALSE, FALSE, 2);
   gtk_widget_show (cv->main_vbox);

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
