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

#include "gimv_wrap_box.h"
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
#include "gimv_list_view.h"
#include "prefs.h"


typedef enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_KEY,
   COLUMN_VALUE,
   COLUMN_RAW_ENTRY,
   COLUMN_FILE,       /* a comment written in the image file: shown only */
   COLUMN_TIP,        /* their whole text (markup) */
   N_COLUMN
} ListStoreColumn;


static void gimv_comment_view_set_sensitive  (GimvCommentView *cv);
static void gimv_comment_view_set_combo_list (GimvCommentView *cv);
static void gimv_comment_view_data_enter     (GimvCommentView *cv);
static void gimv_comment_view_reset_data     (GimvCommentView *cv);
static void combo_remove_extra                (GimvCommentView *cv);
static void combo_select_name                 (GimvCommentView *cv,
                                               const gchar     *name,
                                               const gchar     *key);


static GimvRow *
comment_row_new (void)
{
   return gimv_row_new (N_COLUMN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER,
                        G_TYPE_BOOLEAN, G_TYPE_STRING);
}


/* the row selected is a comment of the image file (not to edit) */
static gboolean
selected_is_file (GimvCommentView *cv)
{
   GimvRow *row;

   if (!cv->comment_clist) return FALSE;
   row = gimv_list_view_get_selected_row (cv->comment_clist);
   return row && gimv_row_peek_boolean (row, COLUMN_FILE);
}


/* a cell of the data list: the comments of the file in italics, with
 * their whole text as the tooltip */
static GtkWidget *
data_cell_setup (gpointer data)
{
   GtkWidget *label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   return label;
}

static void
data_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   GtkWidget *label = gimv_cell_get_content (cell);
   gboolean file = gimv_row_peek_boolean (row, COLUMN_FILE);
   const gchar *text = gimv_row_peek_string (row, GPOINTER_TO_INT (data));
   PangoAttrList *attrs = NULL;

   gtk_label_set_text (GTK_LABEL (label), text ? text : "");
   if (file) {
      attrs = pango_attr_list_new ();
      pango_attr_list_insert (attrs, pango_attr_style_new (PANGO_STYLE_ITALIC));
   }
   gtk_label_set_attributes (GTK_LABEL (label), attrs);
   if (attrs) pango_attr_list_unref (attrs);
   gtk_widget_set_tooltip_markup (label, file ? gimv_row_peek_string (row, COLUMN_TIP) : NULL);
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
   if (selected_is_file (cv)) return;

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
   combo_remove_extra (cv);
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
cb_data_list_selection_changed (GtkSelectionModel *selection,
                                guint position, guint n_items,
                                GimvCommentView *cv)
{
   GimvRow *row;
   const gchar *key = NULL, *value = NULL, *raw_key = NULL;
   gchar *whole = NULL;

   if (!cv->comment_clist) return;

   row = gimv_list_view_get_selected_row (cv->comment_clist);
   if (row) {
      GimvCommentDataEntry *entry = gimv_row_peek_pointer (row, COLUMN_RAW_ENTRY);
      if (entry) raw_key = entry->key;
      key   = gimv_row_peek_string (row, COLUMN_KEY);
      value = gimv_row_peek_string (row, COLUMN_VALUE);
      /* a comment of the file: its whole text (the list shows its first line) */
      if (gimv_row_peek_boolean (row, COLUMN_FILE)) {
         const gchar *tip = gimv_row_peek_string (row, COLUMN_TIP);
         if (tip && pango_parse_markup (tip, -1, 0, NULL, &whole, NULL, NULL))
            value = g_strdelimit (whole, "\n", ' ');
      }
   }

   combo_select_name (cv, key, raw_key);
   gtk_editable_set_text (GTK_EDITABLE (cv->value_entry), value ? value : "");
   g_free (whole);

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


/* the key of a row that is not one of the keys of the combo (a comment of
 * the file, X-IMG-Tags, a line of another program) is put at the end of
 * the combo while it is selected, so the combo shows the key of the row
 * and not the one selected before; such a row is only shown */
static void
combo_remove_extra (GimvCommentView *cv)
{
   GPtrArray *keys  = g_object_get_data (G_OBJECT (cv->key_combo), "keys");
   GPtrArray *names = g_object_get_data (G_OBJECT (cv->key_combo), "names");
   GListModel *model = gtk_drop_down_get_model (GTK_DROP_DOWN (cv->key_combo));

   if (!g_object_get_data (G_OBJECT (cv->key_combo), "extra")) return;
   g_object_set_data (G_OBJECT (cv->key_combo), "extra", NULL);
   if (!keys || !names || !keys->len || !GTK_IS_STRING_LIST (model)) return;
   g_ptr_array_remove_index (keys, keys->len - 1);
   g_ptr_array_remove_index (names, names->len - 1);
   gtk_string_list_remove (GTK_STRING_LIST (model), g_list_model_get_n_items (model) - 1);
}


static gboolean
combo_has_extra (GimvCommentView *cv)
{
   return g_object_get_data (G_OBJECT (cv->key_combo), "extra") != NULL;
}


/* select the item whose display name is "name"; not one of them: an item
 * for it at the end (key: the key of the row) */
static void
combo_select_name (GimvCommentView *cv, const gchar *name, const gchar *key)
{
   GPtrArray *keys  = g_object_get_data (G_OBJECT (cv->key_combo), "keys");
   GPtrArray *names = g_object_get_data (G_OBJECT (cv->key_combo), "names");
   GListModel *model;
   guint i, idx = GTK_INVALID_LIST_POSITION;

   combo_remove_extra (cv);
   for (i = 0; name && names && i < names->len; i++) {
      if (!strcmp (name, g_ptr_array_index (names, i))) {
         idx = i;
         break;
      }
   }
   model = gtk_drop_down_get_model (GTK_DROP_DOWN (cv->key_combo));
   if (idx == GTK_INVALID_LIST_POSITION && name && *name && keys && names
       && GTK_IS_STRING_LIST (model))
   {
      g_ptr_array_add (keys,  g_strdup (key ? key : name));
      g_ptr_array_add (names, g_strdup (name));
      gtk_string_list_append (GTK_STRING_LIST (model), name);
      g_object_set_data (G_OBJECT (cv->key_combo), "extra", GINT_TO_POINTER (1));
      idx = keys->len - 1;
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

   if (!cv->tags_flow) return;
   gimv_wrap_box_clear (GIMV_WRAP_BOX (cv->tags_flow));
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
      gimv_wrap_box_append (GIMV_WRAP_BOX (cv->tags_flow), none);
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
      gimv_wrap_box_append (GIMV_WRAP_BOX (cv->tags_flow), button);
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

   /* as words, wrapping when the line is full */
   cv->tags_flow = gimv_wrap_box_new (2, 0);
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
   gtk_widget_set_visible (label, TRUE);
   cv->data_page = vbox = gimv_vbox_new (FALSE, 0);
   gtk_widget_set_visible (vbox, TRUE);

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
   GtkColumnViewColumn *col;

   /* GTK4: GtkTreeView (deprecated) -> GtkColumnView */
   clist = gimv_column_view_new (G_LIST_MODEL (gimv_row_store_new ()),
                                 GTK_SELECTION_SINGLE);
   cv->comment_clist = clist;
   gimv_list_view_enable_search (clist, COLUMN_KEY, NULL, NULL);

   /* set column for key */
   col = gimv_column_view_append (GTK_COLUMN_VIEW (clist), titles[0],
                                  gimv_cell_factory_new (data_cell_setup, data_cell_bind,
                                                         GINT_TO_POINTER (COLUMN_KEY),
                                                         NULL, FALSE));
   gtk_column_view_column_set_resizable (col, TRUE);

   /* set column for value */
   col = gimv_column_view_append (GTK_COLUMN_VIEW (clist), titles[1],
                                  gimv_cell_factory_new (data_cell_setup, data_cell_bind,
                                                         GINT_TO_POINTER (COLUMN_VALUE),
                                                         NULL, FALSE));
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

   /* a comment of the image file is only shown; so is a key that is not
      one of the keys to edit here (X-IMG-Tags: Edit Tags...) */
   {
      gboolean file = selected && selected_is_file (cv);
      gboolean other = selected && combo_has_extra (cv);
      gtk_editable_set_editable (GTK_EDITABLE (cv->value_entry), !file && !other);
      gtk_widget_set_sensitive (cv->key_combo, !file && !other);
      gtk_widget_set_sensitive (cv->save_button, !file && !other);
      gtk_widget_set_sensitive (cv->delete_button, !file);
      gtk_widget_set_tooltip_text (cv->value_entry,
         other && !g_strcmp0 (g_object_get_data (G_OBJECT (cv->key_combo), "key"), "X-IMG-Tags")
         ? _("The tags are edited with Edit Tags... (the right button menu or the Edit menu)")
         : NULL);
   }
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
   g_object_set_data (G_OBJECT (combo), "extra", NULL);

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
   if (selected_is_file (cv)) return;
   if (combo_has_extra (cv)) return;

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

         if (!strcmp ("X-IMG-File-Path-In-Archive", entry->key)
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

      /* the comments written in the file, after the data (in italics:
       * shown only, not saved with the data) */
      if (info && !gimv_image_info_is_in_archive (info)) {
         GPtrArray *list = gimv_file_comment_read (gimv_image_info_get_path (info));
         GListModel *model = gimv_list_view_get_model (cv->comment_clist);
         guint i;

         for (i = 0; i < list->len; i++) {
            GimvFileComment *c = g_ptr_array_index (list, i);
            const gchar *nl = strchr (c->text, '\n');
            gchar *name = g_strdup_printf (_("In File: %s"), c->name);
            gchar *value = nl ? g_strdup_printf ("%.*s …", (gint) (nl - c->text), c->text)
                              : g_strdup (c->text);
            gchar *tip = g_markup_escape_text (c->text, -1);
            GimvRow *row = comment_row_new ();

            gimv_row_set (row,
                          COLUMN_KEY,       name,
                          COLUMN_VALUE,     value,
                          COLUMN_RAW_ENTRY, NULL,
                          COLUMN_FILE,      TRUE,
                          COLUMN_TIP,       tip,
                          COLUMN_TERMINATOR);
            g_list_store_append (G_LIST_STORE (model), row);
            g_object_unref (row);
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

   g_list_store_remove_all (G_LIST_STORE (gimv_list_view_get_model (cv->comment_clist)));

   gtk_drop_down_set_selected (GTK_DROP_DOWN (cv->key_combo),
                               GTK_INVALID_LIST_POSITION);
   combo_remove_extra (cv);
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
