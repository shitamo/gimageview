/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * gimv_dlist.c - Pair of reorderable stack list widget.
 * Copyright (C) 2001-2002 Takuro Ashie
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
 * Foundation, Inc., 59 Temple Place - Suite 3S30, Boston, MA 02111-1307, USA.
 *
 * $Id: gimv_dlist.c,v 1.6 2004/09/22 15:37:11 makeinu Exp $
 */

#include "gimv_dlist.h"

#include <string.h>
#include <stdlib.h>

#include "intl.h" /* FIXME */
#include "gimv_list_view.h"


enum {
   ENABLED_LIST_UPDATED_SIGNAL,
   LAST_SIGNAL
};


/* GTK4: GtkTreeView (deprecated) -> GtkListView of GimvRow
   (columns: 0 = translated label, 1 = label, 2 = index) */
#define list_widget_get_row_num(widget) \
   g_list_model_get_n_items (gimv_list_view_get_model (widget))

#define list_widget_get_store(widget) \
   G_LIST_STORE (gimv_list_view_get_model (widget))


static void       gimv_dlist_init                     (GimvDList *dslist);
static void       gimv_dlist_class_init               (GimvDListClass *klass);

/* object class functions */
static void       gimv_dlist_finalize                 (GObject *object);

/* private functions */
static void       gimv_dlist_enabled_list_updated     (GimvDList *dslist);
static void       gimv_dlist_set_sensitive            (GimvDList *dslist);
static GtkWidget *gimv_dlist_create_list_widget       (GimvDList *dslist,
                                                       gboolean    reorderble);


static GtkBoxClass *parent_class = NULL;
static guint gimv_dlist_signals[LAST_SIGNAL] = {0};


GType
gimv_dlist_get_type (void)
{
   static GType gimv_dlist_type = 0;

   if (!gimv_dlist_type) {
      static const GTypeInfo gimv_dlist_info = {
         sizeof (GimvDListClass),
         NULL,               /* base_init */
         NULL,               /* base_finalize */
         (GClassInitFunc)    gimv_dlist_class_init,
         NULL,               /* class_finalize */
         NULL,               /* class_data */
         sizeof (GimvDList),
         0,                  /* n_preallocs */
         (GInstanceInitFunc) gimv_dlist_init,
      };

      gimv_dlist_type = g_type_register_static (GTK_TYPE_BOX,
                                                 "GimvDList",
                                                 &gimv_dlist_info,
                                                 0);
   }

   return gimv_dlist_type;
}


static void
gimv_dlist_init (GimvDList *dslist)
{
   dslist->clist1          = NULL;
   dslist->clist2          = NULL;
   dslist->add_button      = NULL;
   dslist->del_button      = NULL;
   dslist->up_button       = NULL;
   dslist->down_button     = NULL;

   dslist->clist1_rows     = 0;
   dslist->clist1_selected = -1;
   dslist->clist1_dest_row = -1;
   dslist->clist2_rows     = 0;
   dslist->clist2_selected = -1;
   dslist->clist2_dest_row = -1;
   dslist->available_list  = NULL;

   gtk_orientable_set_orientation (GTK_ORIENTABLE (dslist),
                                   GTK_ORIENTATION_HORIZONTAL);
}


static void
gimv_dlist_class_init (GimvDListClass *klass)
{
   GObjectClass *gobject_class;

   gobject_class = (GObjectClass *) klass;
   parent_class = g_type_class_peek_parent (klass);

   gimv_dlist_signals[ENABLED_LIST_UPDATED_SIGNAL]
      = g_signal_new ("enabled-list-updated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvDListClass, enabled_list_updated),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gobject_class->finalize = gimv_dlist_finalize;
}



/*******************************************************************************
 *
 *  Object Class functions.
 *
 *******************************************************************************/
static void
gimv_dlist_finalize (GObject *object)
{
   GimvDList *dslist = GIMV_DLIST (object);

   g_list_foreach (dslist->available_list, (GFunc) g_free, NULL);
   g_list_free (dslist->available_list);
   dslist->available_list = NULL;

   if (G_OBJECT_CLASS (parent_class)->finalize)
      G_OBJECT_CLASS (parent_class)->finalize (object);
}



/*******************************************************************************
 *
 *  Callback functions for child widget.
 *
 *******************************************************************************/


static void
gimv_dlist_update_selected (GimvDList *dslist, GtkWidget *view)
{
   guint position = gimv_list_view_get_selected (view);
   gint selected = position == GTK_INVALID_LIST_POSITION ? -1 : (gint) position;

   if (view == dslist->clist1)
      dslist->clist1_selected = selected;
   else if  (view == dslist->clist2)
      dslist->clist2_selected = selected;

   gimv_dlist_set_sensitive (dslist);
}


static void
cb_gimv_dlist_selection_changed (GtkSelectionModel *selection,
                                 guint position, guint n_items, gpointer data)
{
   GimvDList *dslist = data;
   GtkWidget *view = g_object_get_data (G_OBJECT (selection), "gimv-dlist-view");

   g_return_if_fail (dslist);
   if (!view) return;

   gimv_dlist_update_selected (dslist, view);
}


/* "row_changed" and "row_deleted" of the GtkListStore */
static void
cb_gimv_dlist_items_changed (GListModel *model, guint position,
                             guint removed, guint added, gpointer data)
{
   GimvDList *dslist = data;

   gimv_dlist_enabled_list_updated (dslist);

   if (removed > 0) {
      if (dslist->clist2_dest_row < 0 && dslist->clist2) {
         guint selected = gimv_list_view_get_selected (dslist->clist2);
         dslist->clist2_selected = selected == GTK_INVALID_LIST_POSITION
            ? -1 : (gint) selected;
         gimv_dlist_set_sensitive (dslist);
      }
      dslist->clist2_dest_row = -1;
   }
}


/* a row was moved by drag and drop */
static void
cb_gimv_dlist_reordered (GtkWidget *view, guint from, guint to, gpointer data)
{
   GimvDList *dslist = data;

   gimv_dlist_update_selected (dslist, view);
   gimv_dlist_enabled_list_updated (dslist);
}



static void
cb_gimv_dlist_add_button_pressed (GtkButton *button, gpointer data)
{
   GimvDList *dslist = data;
   gint idx;

   if (dslist->clist1_selected < 0) return;

   {
      GimvRow *row = gimv_list_view_get_selected_row (dslist->clist1);

      if (!row) return;

      idx = gimv_row_peek_int (row, 2);
   }

   gimv_dlist_column_add (dslist, idx);

   gimv_dlist_enabled_list_updated (dslist);
   gimv_dlist_set_sensitive (dslist);
}


static void
cb_gimv_dlist_del_button_pressed (GtkButton *button, gpointer data)
{
   GimvDList *dslist = data;
   gint idx;

   if (dslist->clist2_selected < 0) return;

   {
      GimvRow *row = gimv_list_view_get_selected_row (dslist->clist2);

      if (!row) return;

      idx = gimv_row_peek_int (row, 2);
   }

   gimv_dlist_column_del (dslist, idx);

   gimv_dlist_enabled_list_updated (dslist);
   gimv_dlist_set_sensitive (dslist);
}


static void
cb_gimv_dlist_up_button_pressed (GtkButton *button, gpointer data)
{
   GimvDList *dslist = data;
   gint selected = dslist->clist2_selected;
   gint rows = dslist->clist2_rows;

   g_return_if_fail (button && dslist);

   if (selected < 1 || selected > rows - 1) return;

   dslist->clist2_dest_row = dslist->clist2_selected - 1;
   gimv_list_view_move_row (dslist->clist2, selected, selected - 1);
}


static void
cb_gimv_dlist_down_button_pressed (GtkButton *button, gpointer data)
{
   GimvDList *dslist = data;
   gint selected = dslist->clist2_selected;
   gint rows = dslist->clist2_rows;

   g_return_if_fail (button && dslist);

   if (selected < 0 || selected > rows - 2) return;

   dslist->clist2_dest_row = dslist->clist2_selected + 1;
   gimv_list_view_move_row (dslist->clist2, selected, selected + 1);
}



/*******************************************************************************
 *
 *  Private functions.
 *
 *******************************************************************************/
static void
gimv_dlist_enabled_list_updated (GimvDList *dslist)
{
   g_return_if_fail (GIMV_IS_DLIST (dslist));

   g_signal_emit (G_OBJECT (dslist),
                  gimv_dlist_signals[ENABLED_LIST_UPDATED_SIGNAL], 0);
}


static void
gimv_dlist_set_sensitive (GimvDList *dslist)
{
   gint selected1 = dslist->clist1_selected;
   gint selected2 = dslist->clist2_selected;
   gint rows1 = dslist->clist1_rows;
   gint rows2 = dslist->clist2_rows;

   /* g_return_if_fail (dslist); */

   /* add button */
   if (selected1 < 0 || selected1 >= rows1) {
      gtk_widget_set_sensitive (dslist->add_button, FALSE);
   } else {
      gtk_widget_set_sensitive (dslist->add_button, TRUE);
   }

   /* delete button */
   if (selected2 < 0 || selected2 >= rows2) {
      gtk_widget_set_sensitive (dslist->del_button, FALSE);
   } else {
      gtk_widget_set_sensitive (dslist->del_button, TRUE);
   }

   /* up button */
   if (selected2 > 0 && selected2 < rows2) {
      gtk_widget_set_sensitive (dslist->up_button, TRUE);
   } else {
      gtk_widget_set_sensitive (dslist->up_button, FALSE);
   }

   /* down button */
   if (selected2 >= 0 && selected2 < rows2 - 1) {
      gtk_widget_set_sensitive (dslist->down_button, TRUE);
   } else {
      gtk_widget_set_sensitive (dslist->down_button, FALSE);
   }
}


static GimvRow *
dlist_row_new (const gchar *i18n_text, const gchar *text, gint idx)
{
   GimvRow *row = gimv_row_new (3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
   gimv_row_set (row, 0, i18n_text, 1, text, 2, idx, -1);
   return row;
}


static GtkWidget *
gimv_dlist_create_list_widget (GimvDList *dslist, gboolean reorderble)
{
   GtkWidget *clist;
   GListStore *store;

   store = gimv_row_store_new ();
   clist = gimv_list_view_new (G_LIST_MODEL (store), GTK_SELECTION_SINGLE,
                               gimv_text_factory_new (0, FALSE));
   dslist->clist2 = clist;

   if (reorderble)
      gimv_list_view_set_reorderable (clist, TRUE,
                                      cb_gimv_dlist_reordered, dslist);

   g_signal_connect (G_OBJECT (store), "items-changed",
                     G_CALLBACK (cb_gimv_dlist_items_changed),
                     dslist);
   g_object_set_data (G_OBJECT (gimv_list_view_get_selection (clist)),
                      "gimv-dlist-view", clist);
   g_signal_connect (gimv_list_view_get_selection (clist), "selection-changed",
                     G_CALLBACK (cb_gimv_dlist_selection_changed),
                     dslist);

   return clist;
}



/*******************************************************************************
 *
 *  Public functions.
 *
 *******************************************************************************/
GtkWidget *
gimv_dlist_new (const gchar *clist1_title,
                 const gchar *clist2_title)
{
   GimvDList *dslist = g_object_new (gimv_dlist_get_type (), NULL);

   GtkWidget *hbox = GTK_WIDGET (dslist);
   GtkWidget *vbox, *vbox1, *vbox2, *vbox3, *hseparator;
   GtkWidget *label, *scrollwin1, *scrollwin2, *clist, *button, *arrow;

   /* possible columns */
   vbox1 = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox1, TRUE, TRUE, 0);
   gtk_widget_set_visible (vbox1, TRUE);

   label = gtk_label_new (clist1_title);
   gimv_box_pack_start (GTK_BOX (vbox1), label, FALSE, FALSE, 0);
   gtk_widget_set_visible (label, TRUE);

   scrollwin1 = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollwin1),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrollwin1), TRUE);
   gimv_container_set_border_width (GTK_WIDGET (scrollwin1), 5);
   gimv_box_pack_start (GTK_BOX (vbox1), scrollwin1, TRUE, TRUE, 0);
   gtk_widget_set_visible (scrollwin1, TRUE);

   clist = dslist->clist1 = gimv_dlist_create_list_widget (dslist, FALSE);
   gimv_container_add (GTK_WIDGET (scrollwin1), clist);
   gtk_widget_set_visible (clist, TRUE);


   /* add/delete buttons */
   vbox3 = gimv_vbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox3, FALSE, FALSE, 0);
   gtk_widget_set_visible (vbox3, TRUE);

   vbox = gimv_vbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (vbox3), vbox, FALSE, FALSE, 0);
   gtk_widget_set_visible (vbox, TRUE);

   button = dslist->add_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_RIGHT);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Add"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = dslist->del_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_LEFT);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Delete"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);

   hseparator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
   gimv_box_pack_start (GTK_BOX (vbox), hseparator, FALSE, FALSE, 2);
   gtk_widget_set_visible (hseparator, TRUE);


   /* move buttons */
   button = dslist->up_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_UP);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Up"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = dslist->down_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_DOWN);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Down"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);


   /* Use list */
   vbox2 = gimv_vbox_new (FALSE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox2, TRUE, TRUE, 0);
   gtk_widget_set_visible (vbox2, TRUE);

   label = gtk_label_new (clist2_title);
   gimv_box_pack_start (GTK_BOX (vbox2), label, FALSE, FALSE, 0);
   gtk_widget_set_visible (label, TRUE);

   scrollwin2 = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollwin2),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrollwin2), TRUE);
   gimv_container_set_border_width (GTK_WIDGET (scrollwin2), 5);
   gimv_box_pack_start (GTK_BOX (vbox2), scrollwin2, TRUE, TRUE, 0);
   gtk_widget_set_visible (scrollwin2, TRUE);

   clist = dslist->clist2 = gimv_dlist_create_list_widget (dslist, TRUE);
   gimv_container_add (GTK_WIDGET (scrollwin2), clist);
   dslist->clist2_rows = list_widget_get_row_num (clist);
   gtk_widget_set_visible (clist, TRUE);



   gtk_widget_set_size_request (scrollwin1, -1, 200);
   gtk_widget_set_size_request (scrollwin2, -1, 200);

#ifdef USE_ARROW
   gtk_widget_set_size_request (dslist->add_button,  20, 20);
   gtk_widget_set_size_request (dslist->del_button,  20, 20);
   gtk_widget_set_size_request (dslist->up_button,   20, 20);
   gtk_widget_set_size_request (dslist->down_button, 20, 20);
#endif /* USE_ARROW */

   g_signal_connect (G_OBJECT (dslist->add_button), "clicked",
                     G_CALLBACK (cb_gimv_dlist_add_button_pressed),
                     dslist);
   g_signal_connect (G_OBJECT (dslist->del_button), "clicked",
                     G_CALLBACK (cb_gimv_dlist_del_button_pressed),
                     dslist);
   g_signal_connect (G_OBJECT (dslist->up_button), "clicked",
                     G_CALLBACK (cb_gimv_dlist_up_button_pressed),
                     dslist);
   g_signal_connect (G_OBJECT (dslist->down_button), "clicked",
                     G_CALLBACK (cb_gimv_dlist_down_button_pressed),
                     dslist);


   return hbox;
}


gint
gimv_dlist_append_available_item (GimvDList *dslist, const gchar *item)
{
   GtkWidget *clist = dslist->clist1;
   gchar *text = g_strdup (item);
   gchar *i18n_text = _(text);
   gint idx;

   g_return_val_if_fail (GIMV_IS_DLIST (dslist), -1);

   dslist->available_list = g_list_append (dslist->available_list, text);
   idx = g_list_index (dslist->available_list, text);

   {
      GimvRow *row = dlist_row_new (i18n_text, text, idx);
      g_list_store_append (list_widget_get_store (clist), row);
      g_object_unref (row);
   }

   dslist->clist1_rows = list_widget_get_row_num (dslist->clist1);
   gimv_dlist_set_sensitive (dslist);

   return idx;
}


void
gimv_dlist_column_add (GimvDList *dslist, gint idx)
{
   gchar *text, *i18n_text;
   GList *list;

   list = g_list_nth (dslist->available_list, idx);
   g_return_if_fail (list);
   text = list->data;
   i18n_text = _(text);
   g_return_if_fail (text);

   {
      GListModel *model1 = gimv_list_view_get_model (dslist->clist1);
      GListModel *model2 = gimv_list_view_get_model (dslist->clist2);
      guint i, n = g_list_model_get_n_items (model1);
      GimvRow *row;

      /* find row from clist1 */
      for (i = 0; i < n; i++) {
         const gchar *str = gimv_row_peek_string (gimv_row_store_peek (model1, i), 1);
         if (str && !strcmp (text, str)) break;
      }
      if (i >= n) return;

      /* append to clist2 */
      row = dlist_row_new (i18n_text, text, idx);
      g_list_store_append (G_LIST_STORE (model2), row);
      g_object_unref (row);

      /* set cursor of clist1 */
      if (i + 1 < n)
         gimv_list_view_select (dslist->clist1, i + 1);
      else if (i > 0)
         gimv_list_view_select (dslist->clist1, i - 1);

      /* remove from clist1 */
      g_list_store_remove (G_LIST_STORE (model1), i);

      /* reset */
      dslist->clist1_rows = g_list_model_get_n_items (model1);
      dslist->clist2_rows = g_list_model_get_n_items (model2);
      gimv_dlist_update_selected (dslist, dslist->clist1);
   }
}


void
gimv_dlist_column_del (GimvDList *dslist, gint idx)
{
   gchar *text, *i18n_text;
   GList *list;

   list = g_list_nth (dslist->available_list, idx);
   g_return_if_fail (list);
   text = list->data;
   i18n_text = _(text);
   g_return_if_fail (text);

   {
      GListModel *model1 = gimv_list_view_get_model (dslist->clist1);
      GListModel *model2 = gimv_list_view_get_model (dslist->clist2);
      guint i, j, n1 = g_list_model_get_n_items (model1);
      guint n2 = g_list_model_get_n_items (model2);
      GimvRow *row;

      /* find row from clist2 */
      for (i = 0; i < n2; i++) {
         const gchar *str = gimv_row_peek_string (gimv_row_store_peek (model2, i), 1);
         if (str && !strcmp (text, str)) break;
      }
      if (i >= n2) return;

      /* insert to clist1 (sorted by index) */
      for (j = 0; j < n1; j++) {
         gint idx1 = gimv_row_peek_int (gimv_row_store_peek (model1, j), 2);
         if (idx < idx1) break;
      }
      row = dlist_row_new (i18n_text, text, idx);
      g_list_store_insert (G_LIST_STORE (model1), j, row);
      g_object_unref (row);

      /* set cursor of clist2 */
      if (i + 1 < n2)
         gimv_list_view_select (dslist->clist2, i + 1);
      else if (i > 0)
         gimv_list_view_select (dslist->clist2, i - 1);

      /* remove from clist2 */
      g_list_store_remove (G_LIST_STORE (model2), i);

      dslist->clist1_rows = g_list_model_get_n_items (model1);
      dslist->clist2_rows = g_list_model_get_n_items (model2);
      gimv_dlist_update_selected (dslist, dslist->clist2);
   }
}


void
gimv_dlist_column_add_by_label (GimvDList *dslist, const gchar *label)
{
   GList *list;
   gint j, idx = -1;

   g_return_if_fail (GIMV_IS_DLIST (dslist));
   g_return_if_fail (label && *label);

   list = dslist->available_list;
   for (j = 0; list; j++, list = g_list_next (list)) {
      if (!strcmp (label, (gchar *)list->data)) {
         idx = j;
         break;
      }
   }
   if (idx >= 0)
      gimv_dlist_column_add (dslist, idx);

   gimv_dlist_set_sensitive (dslist);
}


gint
gimv_dlist_get_n_available_items (GimvDList  *dslist)
{
   g_return_val_if_fail (GIMV_IS_DLIST (dslist), 0);
   return dslist->clist1_rows;
}


gint
gimv_dlist_get_n_enabled_items (GimvDList  *dslist)
{
   g_return_val_if_fail (GIMV_IS_DLIST (dslist), 0);
   return dslist->clist2_rows;
}


gchar *
gimv_dlist_get_enabled_row_text (GimvDList *dslist, gint row)
{
   gchar *text;

   g_return_val_if_fail (GIMV_IS_DLIST (dslist), NULL);
   g_return_val_if_fail (row >= 0 && row < dslist->clist2_rows, NULL);

   {
      GimvRow *r = gimv_row_store_peek (gimv_list_view_get_model (dslist->clist2),
                                        row);
      if (!r) return NULL;

      text = NULL;
      gimv_row_get (r, 1, &text, -1);

      return text;
   }
}
