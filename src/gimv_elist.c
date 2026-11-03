/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * gimv_elist.c - a set of clist and edit area widget.
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
 * $Id: gimv_elist.c,v 1.6 2004/09/22 15:37:11 makeinu Exp $
 */

#include "gimv_elist.h"

#include <stdlib.h>
#include <string.h>

#include "intl.h"
#include "gimv_marshal.h"
#include "gimv_list_view.h"
enum {
   LIST_UPDATED_SIGNAL,
   EDIT_AREA_SET_DATA_SIGNAL,
   ACTION_CONFIRM_SIGNAL,
   LAST_SIGNAL
};



/* GTK4: GtkTreeView (deprecated) -> GtkColumnView of GimvRow */
#define list_widget_get_row_num(widget) \
   g_list_model_get_n_items (gimv_list_view_get_model (widget))

#define list_widget_get_store(widget) \
   G_LIST_STORE (gimv_list_view_get_model (widget))

#define list_widget_get_row(widget, row) \
   gimv_row_store_peek (gimv_list_view_get_model (widget), (row))
#define ROWDATA(columns)     columns
#define ROWDESTROY(columns)  columns + 1
#define ALL_COLUMNS(columns) columns + 2



static void gimv_elist_init       (GimvEList *editlist);
static void gimv_elist_class_init (GimvEListClass *klass);
static void gimv_elist_finalize   (GObject *object);

/* private */
static void     gimv_elist_set_move_button_sensitive   (GimvEList *editlist);
static void     gimv_elist_set_sensitive               (GimvEList *editlist);
static void     gimv_elist_edit_area_set_data          (GimvEList *editlist,
                                                        gint          row);
static gchar  **gimv_elist_edit_area_get_data          (GimvEList *editlist,
                                                        GimvEListActionType type);
static void     gimv_elist_edit_area_reset             (GimvEList *editlist);


static GtkBoxClass *parent_class = NULL;
static guint gimv_elist_signals[LAST_SIGNAL] = {0};


GType
gimv_elist_get_type (void)
{
   static GType gimv_elist_type = 0;

   if (!gimv_elist_type) {
      static const GTypeInfo gimv_elist_info = {
         sizeof (GimvEListClass),
         NULL,               /* base_init */
         NULL,               /* base_finalize */
         (GClassInitFunc)    gimv_elist_class_init,
         NULL,               /* class_finalize */
         NULL,               /* class_data */
         sizeof (GimvEList),
         0,                  /* n_preallocs */
         (GInstanceInitFunc) gimv_elist_init,
      };

      gimv_elist_type = g_type_register_static (GTK_TYPE_BOX,
                                                   "GimvEList",
                                                   &gimv_elist_info,
                                                   0);
   }

   return gimv_elist_type;
}


static void
gimv_elist_init (GimvEList *editlist)
{
   editlist->clist              = NULL;
   editlist->new_button         = NULL;
   editlist->add_button         = NULL;
   editlist->change_button      = NULL;
   editlist->del_button         = NULL;
   editlist->up_button          = NULL;
   editlist->down_button        = NULL;
   editlist->edit_area          = NULL;
   editlist->move_button_area   = NULL;
   editlist->action_button_area = NULL;

   editlist->max_row            = -1;
   editlist->columns            = 0;
   editlist->rows               = 0;
   editlist->selected           = -1;
   editlist->dest_row           = -1;

   editlist->edit_area_flags    = GIMV_ELIST_EDIT_AREA_VALUE_INITIALIZED;
   editlist->column_func_tables = NULL;
   editlist->get_rowdata_fn     = NULL;

   gtk_orientable_set_orientation (GTK_ORIENTABLE (editlist),
                                   GTK_ORIENTATION_VERTICAL);

   editlist->rowdata_table
      = g_hash_table_new (g_direct_hash, g_direct_equal);
   editlist->rowdata_destroy_fn_table
      = g_hash_table_new (g_direct_hash, g_direct_equal);
}


static void
gimv_elist_class_init (GimvEListClass *klass)
{
   GObjectClass *gobject_class;

   gobject_class = (GObjectClass *) klass;
   parent_class = g_type_class_peek_parent (klass);

   gimv_elist_signals[LIST_UPDATED_SIGNAL]
      = g_signal_new ("list-updated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvEListClass, list_updated),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_elist_signals[EDIT_AREA_SET_DATA_SIGNAL]
      = g_signal_new ("edit-area-set-data",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvEListClass, edit_area_set_data),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);

   gimv_elist_signals[ACTION_CONFIRM_SIGNAL]
      = g_signal_new ("action-confirm",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (GimvEListClass, action_confirm),
                      NULL, NULL,
                      g_cclosure_marshal_generic,
                      G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_INT, G_TYPE_POINTER);

   gobject_class->finalize = gimv_elist_finalize;
}


static void
gimv_elist_updated (GimvEList *editlist)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   g_signal_emit (G_OBJECT (editlist),
                  gimv_elist_signals[LIST_UPDATED_SIGNAL], 0);
}



/*******************************************************************************
 *
 *  Object class functions.
 *
 *******************************************************************************/
static void
free_rowdata (gpointer key, gpointer value, gpointer data)
{
   GimvEList *editlist = data;
   GDestroyNotify destroy;

   destroy = g_hash_table_lookup (editlist->rowdata_destroy_fn_table, value);
   if (destroy)
      destroy (value);
}


static void
gimv_elist_finalize (GObject *object)
{
   GimvEList *editlist = GIMV_ELIST (object);
   gint i;

   /* remove column funcs */
   for (i = 0; i < editlist->columns; i++) {
      GimvEListColumnFuncTable *table = editlist->column_func_tables[i];
      if (table->destroy_fn)
         table->destroy_fn (table->coldata);
      g_free (table);
      editlist->column_func_tables[i] = NULL;
   }
   g_free (editlist->column_func_tables);
   editlist->column_func_tables = NULL;

   g_hash_table_foreach (editlist->rowdata_table, free_rowdata, editlist);
   g_hash_table_destroy (editlist->rowdata_table);
   g_hash_table_destroy (editlist->rowdata_destroy_fn_table);

   if (G_OBJECT_CLASS (parent_class)->finalize)
      G_OBJECT_CLASS (parent_class)->finalize (object);
}



/*******************************************************************************
 *
 *  Callback functions for child widget.
 *
 *******************************************************************************/

static void
cb_editlist_selection_changed (GtkSelectionModel *selection,
                               guint position, guint n_items, gpointer data)
{
   GimvEList *editlist = data;
   guint selected;

   g_return_if_fail (editlist);
   if (!editlist->clist) return;

   selected = gimv_list_view_get_selected (editlist->clist);
   if (selected == GTK_INVALID_LIST_POSITION)
      editlist->selected = -1;
   else
      editlist->selected = selected;

   gimv_elist_edit_area_set_data (editlist, editlist->selected);

   gimv_elist_set_sensitive (editlist);
}


/* "row_changed" and "row_deleted" of the GtkListStore */
static void
cb_editlist_items_changed (GListModel *model, guint position,
                           guint removed, guint added, gpointer data)
{
   GimvEList *editlist = data;

   editlist->rows = g_list_model_get_n_items (model);

   gimv_elist_updated (editlist);

   if (removed > 0) {
      /* the selection model keeps the selected row selected if it was only
         moved (up/down buttons, sorting) */
      if (editlist->dest_row < 0) {
         guint selected = gimv_list_view_get_selected (editlist->clist);
         editlist->selected = selected == GTK_INVALID_LIST_POSITION
            ? -1 : (gint) selected;
         gimv_elist_set_sensitive (editlist);
      }
      editlist->dest_row = -1;

      if (editlist->selected >= editlist->rows)
         editlist->selected = -1;
      gimv_elist_edit_area_set_data (editlist, editlist->selected);
   }

   gimv_elist_set_sensitive (editlist);
}


/* a row was moved by drag and drop */
static void
cb_editlist_reordered (GtkWidget *view, guint from, guint to, gpointer data)
{
   GimvEList *editlist = data;

   gimv_elist_updated (editlist);
   gimv_elist_set_sensitive (editlist);
}



static void
cb_editlist_up_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;

   gint selected = editlist->selected;
   gint rows = editlist->rows;

   g_return_if_fail (button && editlist);

   if (selected < 1 || selected > rows - 1) return;

   editlist->dest_row = editlist->selected - 1;
   gimv_list_view_move_row (editlist->clist, selected, selected - 1);
}


static void
cb_editlist_down_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;
   gint selected = editlist->selected;
   gint rows = editlist->rows;

   g_return_if_fail (button && editlist);

   if (selected < 0 || selected > rows - 2) return;

   editlist->dest_row = editlist->selected + 1;
   gimv_list_view_move_row (editlist->clist, selected, selected + 1);
}


static void
cb_editlist_new_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;
   GimvEListConfirmFlags flags;

   flags = gimv_elist_action_confirm (editlist, GIMV_ELIST_ACTION_RESET);
   if (flags & GIMV_ELIST_CONFIRM_CANNOT_NEW) return;

   gimv_elist_unselect_all (editlist);
   gimv_elist_edit_area_reset (editlist);
   gimv_elist_set_sensitive (editlist);
}


static void
cb_editlist_add_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;
   GimvEListConfirmFlags flags;
   gint row;
   gchar **text;
   gpointer rowdata = NULL;
   GDestroyNotify destroy_fn = NULL;
   gboolean set_rowdata = FALSE;

   g_return_if_fail (GIMV_IS_ELIST (editlist));

   flags = gimv_elist_action_confirm (editlist, GIMV_ELIST_ACTION_ADD);
   if (flags & GIMV_ELIST_CONFIRM_CANNOT_ADD) return;

   text = gimv_elist_edit_area_get_data (editlist, GIMV_ELIST_ACTION_ADD);
   g_return_if_fail (text);

   row = gimv_elist_append_row (editlist, text);
   g_strfreev (text);

   if (editlist->get_rowdata_fn)
      set_rowdata = editlist->get_rowdata_fn (editlist,
                                              GIMV_ELIST_ACTION_ADD,
                                              &rowdata, &destroy_fn);
   if (set_rowdata) {
      gimv_elist_set_row_data_full (editlist, row, rowdata, destroy_fn);
   }

   gimv_elist_updated (editlist);

   gimv_elist_edit_area_reset (editlist);
   gimv_elist_set_sensitive (editlist);
}


static void
cb_editlist_change_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;
   GimvEListConfirmFlags flags;
   gchar **text;
   gint i;
   gpointer rowdata = NULL;
   GDestroyNotify destroy_fn = NULL;
   gboolean set_rowdata = FALSE;

   g_return_if_fail (GIMV_IS_ELIST (editlist));

   if (editlist->selected < 0 || editlist->selected >= editlist->rows) return;

   flags = gimv_elist_action_confirm (editlist, GIMV_ELIST_ACTION_CHANGE);
   if (flags & GIMV_ELIST_CONFIRM_CANNOT_CHANGE) return;

   text = gimv_elist_edit_area_get_data (editlist,
                                            GIMV_ELIST_ACTION_CHANGE);
   g_return_if_fail (text);

   {
      GimvRow *row = gimv_list_view_get_selected_row (editlist->clist);

      if (!row) {
         g_strfreev (text);
         return;
      }

      for (i = 0; i < editlist->columns; i++) {
         gimv_row_set (row, i, text[i], -1);
      }
   }

   g_strfreev (text);

   if (editlist->get_rowdata_fn)
      set_rowdata = editlist->get_rowdata_fn (editlist,
                                              GIMV_ELIST_ACTION_CHANGE,
                                              &rowdata, &destroy_fn);
   if (set_rowdata) {
      gimv_elist_set_row_data_full (editlist, editlist->selected,
                                       rowdata, destroy_fn);
   }

   gimv_elist_updated (editlist);

   editlist->edit_area_flags &= ~GIMV_ELIST_EDIT_AREA_VALUE_CHANGED;

   gimv_elist_set_sensitive (editlist);
}


static void
cb_editlist_delete_button (GtkButton *button, gpointer data)
{
   GimvEList *editlist = data;
   GimvEListConfirmFlags flags;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (editlist->selected >= 0);

   flags = gimv_elist_action_confirm (editlist, GIMV_ELIST_ACTION_DELETE);
   if (flags & GIMV_ELIST_CONFIRM_CANNOT_DELETE) return;

   gimv_elist_remove_row (editlist, editlist->selected);

   gimv_elist_updated (editlist);

   gimv_elist_set_sensitive (editlist);
}


/*******************************************************************************
 *
 *  Private functions.
 *
 *******************************************************************************/
static void
gimv_elist_set_move_button_sensitive (GimvEList *editlist)
{
   gint rownum = editlist->rows;
   gint selected = editlist->selected;
   gboolean reorderble = gimv_elist_get_reorderable (editlist);

   if (!reorderble || selected < 0) {
      gtk_widget_set_sensitive (editlist->up_button, FALSE);
      gtk_widget_set_sensitive (editlist->down_button, FALSE);

   } else {
      /* up button */
      if (selected == 0)
         gtk_widget_set_sensitive (editlist->up_button, FALSE);
      else
         gtk_widget_set_sensitive (editlist->up_button, TRUE);

      /* down button */
      if (selected < rownum - 1)
         gtk_widget_set_sensitive (editlist->down_button, TRUE);
      else
         gtk_widget_set_sensitive (editlist->down_button, FALSE);
   }
}


static void
gimv_elist_set_sensitive (GimvEList *editlist)
{
   gimv_elist_set_move_button_sensitive (editlist);
   gimv_elist_set_action_button_sensitive (editlist);
}


static GtkWidget *
gimv_elist_create_list_widget (GimvEList *editlist, gint colnum)
{
   GtkWidget *clist;
   GListStore *store;
   GtkColumnViewColumn *col = NULL;
   gint i;

   /*
    *  columns: [0, colnum) are strings,
    *  ROWDATA(colnum) is row data,
    *  ROWDESTROY(colnum) is GDestroyNotify
    */
   store = gimv_row_store_new ();
   clist = gimv_column_view_new (G_LIST_MODEL (store), GTK_SELECTION_SINGLE);
   editlist->clist = clist;
   gimv_list_view_set_reorderable (clist, TRUE,
                                   cb_editlist_reordered, editlist);

   g_signal_connect (G_OBJECT (store), "items-changed",
                     G_CALLBACK (cb_editlist_items_changed),
                     editlist);
   g_signal_connect (gimv_list_view_get_selection (clist), "selection-changed",
                     G_CALLBACK (cb_editlist_selection_changed),
                     editlist);

   /* set column */
   for (i = 0; i < colnum; i++) {
      col = gimv_column_view_append_text (GTK_COLUMN_VIEW (clist), NULL, i);
      gtk_column_view_column_set_resizable (col, TRUE);
   }
   /* GtkTreeView let the last column take the remaining width */
   if (col)
      gtk_column_view_column_set_expand (col, TRUE);

   editlist->columns = colnum;

   return clist;
}


static GimvEListColumnFuncTable *
gimv_elist_column_func_table_new (void)
{
   GimvEListColumnFuncTable *table;
   table = g_new0 (GimvEListColumnFuncTable, 1);
   table->widget            = NULL;
   table->coldata           = NULL;
   table->destroy_fn        = NULL;
   table->set_data_fn       = NULL;
   table->get_data_fn       = NULL;
   table->reset_fn          = NULL;

   return table;
}


static void
gimv_elist_edit_area_set_data (GimvEList *editlist, gint row)
{
   gint i;

   g_return_if_fail (row < editlist->rows);

   g_signal_emit (G_OBJECT (editlist),
                  gimv_elist_signals[EDIT_AREA_SET_DATA_SIGNAL], 0);

   for (i = 0; i < editlist->columns; i++) {
      GimvEListColumnFuncTable *table = editlist->column_func_tables[i];
      gchar *text = NULL;

      if (!table) continue;
      if (!table->set_data_fn) continue;

      if (row >= 0) {
         GimvRow *r = list_widget_get_row (editlist->clist, row);

         if (r)
            gimv_row_get (r, i, &text, -1);
      } 

      table->set_data_fn (editlist, table->widget, row, i,
                          text, table->coldata);

      g_free (text);
   }

   editlist->edit_area_flags &= ~GIMV_ELIST_EDIT_AREA_VALUE_INITIALIZED;
   editlist->edit_area_flags &= ~GIMV_ELIST_EDIT_AREA_VALUE_CHANGED;

   gimv_elist_set_sensitive (editlist);
}


static gchar **
gimv_elist_edit_area_get_data (GimvEList *editlist, GimvEListActionType type)
{
   gint i;
   gchar **text;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (editlist->columns > 0, NULL);

   text = g_new0 (gchar *, editlist->columns + 1);

   for (i = 0; i < editlist->columns; i++) {
      GimvEListColumnFuncTable *table = editlist->column_func_tables[i];

      text[i] = NULL;

      if (!table) {
         g_warning ("GimvEList: function table for column %d is not found", i);
         continue;
      }
      if (!table->get_data_fn) {
         g_warning ("GimvEList: get_data_fn method for column %d is notimplemented", i);
         continue;
      }

      text[i] = table->get_data_fn (editlist, type, table->widget, table->coldata);
   }
   text[editlist->columns] = NULL;

   return text;
}


static void
gimv_elist_edit_area_reset (GimvEList *editlist)
{
   gint i;

   for (i = 0; i < editlist->columns; i++) {
      GimvEListColumnFuncTable *table = editlist->column_func_tables[i];

      if (!table) continue;
      if (!table->reset_fn) continue;

      table->reset_fn (editlist, table->widget, table->coldata);
   }

   editlist->edit_area_flags |= GIMV_ELIST_EDIT_AREA_VALUE_INITIALIZED;
   editlist->edit_area_flags &= ~GIMV_ELIST_EDIT_AREA_VALUE_CHANGED;

   gimv_elist_set_sensitive (editlist);
}



/*******************************************************************************
 *
 *  Public functions.
 *
 *******************************************************************************/
GtkWidget *
gimv_elist_new (gint colnum)
{
   GimvEList *editlist;
   GtkWidget *main_vbox;
   GtkWidget *vbox, *vbox1, *hbox, *hbox1, *scrollwin;
   GtkWidget *clist, *button, *arrow;
   gint i;

   g_return_val_if_fail (colnum > 0, NULL);

   editlist = g_object_new (gimv_elist_get_type (), NULL);
   main_vbox = GTK_WIDGET (editlist);

   /* clist */
   hbox = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 0);
   gimv_box_pack_start (GTK_BOX (main_vbox), hbox, TRUE, TRUE, 0);
   gtk_widget_set_visible (hbox, TRUE);

   scrollwin = gimv_scrolled_window_new (NULL, NULL);
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollwin),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scrollwin), TRUE);
   gimv_container_set_border_width (GTK_WIDGET (scrollwin), 5);
   gimv_box_pack_start (GTK_BOX (hbox), scrollwin, TRUE, TRUE, 0);
   gimv_widget_set_size (scrollwin, -1, 120);
   gtk_widget_set_visible (scrollwin, TRUE);

   clist = gimv_elist_create_list_widget (editlist, colnum);
   gimv_container_add (GTK_WIDGET (scrollwin), clist);
   gtk_widget_set_visible (clist, TRUE);


   /* move buttons */
   vbox = editlist->move_button_area = gimv_vbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (hbox), vbox, FALSE, FALSE, 2);
   gtk_widget_set_visible (vbox, TRUE);

   vbox1 = gimv_vbox_new (TRUE, 0);
   gimv_box_pack_start (GTK_BOX (vbox), vbox1, FALSE, FALSE, 2);
   gtk_widget_set_visible (vbox1, TRUE);

   button = editlist->up_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_UP);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Up"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox1), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = editlist->down_button = gtk_button_new ();
#ifdef USE_ARROW
   arrow = gimv_arrow_new (GTK_ARROW_DOWN);
#else /* USE_ARROW */
   arrow = gtk_label_new (_("Down"));
   gimv_misc_set_alignment (arrow, 0.5, 0.5);
#endif /* USE_ARROW */
   gimv_container_add (GTK_WIDGET (button), arrow);
   gtk_widget_set_visible (arrow, TRUE);
   gimv_box_pack_start (GTK_BOX (vbox1), button, FALSE, FALSE, 2);
   gtk_widget_set_visible (button, TRUE);


   /* edit area */
   editlist->edit_area = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (editlist->edit_area), 5);
   gimv_box_pack_start (GTK_BOX (main_vbox), editlist->edit_area,
                       FALSE, TRUE, 0);
   gtk_widget_set_visible (editlist->edit_area, TRUE);


   /* edit buttons */
   hbox = editlist->action_button_area = gimv_hbox_new (FALSE, 0);
   gimv_container_set_border_width (GTK_WIDGET (hbox), 5);
   gimv_box_pack_start (GTK_BOX (main_vbox), hbox, FALSE, TRUE, 0);
   gtk_widget_set_visible (hbox, TRUE);

   hbox1 = gimv_hbox_new (TRUE, 0);
   gimv_box_pack_end (GTK_BOX (hbox), hbox1, FALSE, TRUE, 0);
   gtk_widget_set_visible (hbox1, TRUE);

   button = editlist->new_button = gtk_button_new_with_label (_("New"));
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = editlist->add_button = gtk_button_new_with_label (_("Add"));
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = editlist->change_button = gtk_button_new_with_label (_("Change"));
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);
   gtk_widget_set_visible (button, TRUE);

   button = editlist->del_button = gtk_button_new_with_label (_("Delete"));
   gimv_box_pack_start (GTK_BOX (hbox1), button, FALSE, TRUE, 2);
   gtk_widget_set_visible (button, TRUE);



#ifdef USE_ARROW
   gtk_widget_set_size_request (editlist->up_button, 20, 20);
   gtk_widget_set_size_request (editlist->down_button, 20, 20);
#endif /* USE_ARROW */
   gtk_widget_set_size_request (editlist->new_button, 70, -1);

   g_signal_connect (G_OBJECT (editlist->up_button), "clicked",
                     G_CALLBACK (cb_editlist_up_button), editlist);
   g_signal_connect (G_OBJECT (editlist->down_button), "clicked",
                     G_CALLBACK (cb_editlist_down_button), editlist);
   g_signal_connect (G_OBJECT (editlist->new_button), "clicked",
                     G_CALLBACK (cb_editlist_new_button), editlist);
   g_signal_connect (G_OBJECT (editlist->add_button), "clicked",
                     G_CALLBACK (cb_editlist_add_button), editlist);
   g_signal_connect (G_OBJECT (editlist->change_button), "clicked",
                     G_CALLBACK (cb_editlist_change_button), editlist);
   g_signal_connect (G_OBJECT (editlist->del_button), "clicked",
                     G_CALLBACK (cb_editlist_delete_button), editlist);


   /* initialize column func tables */
   editlist->column_func_tables
      = g_new0 (GimvEListColumnFuncTable *, editlist->columns);
   for (i = 0; i < colnum; i++) {
      editlist->column_func_tables[i]
         = gimv_elist_column_func_table_new ();
   }

   gimv_elist_set_sensitive (editlist);

   return main_vbox;
}


GtkWidget *
gimv_elist_new_with_titles (gint colnum, gchar *titles[])
{
   GimvEList *editlist;
   gint i;

   editlist = GIMV_ELIST (gimv_elist_new (colnum));

   for (i = 0; i < editlist->columns; i++) {
      GtkColumnViewColumn *col
         = gimv_column_view_get_column (GTK_COLUMN_VIEW (editlist->clist), i);
      gtk_column_view_column_set_title (col, titles[i]);
   }

   gimv_column_view_set_headers_visible (GTK_COLUMN_VIEW (editlist->clist), TRUE);

   return GTK_WIDGET (editlist);
}


void
gimv_elist_set_column_title_visible (GimvEList *editlist, gboolean visible)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   gimv_column_view_set_headers_visible (GTK_COLUMN_VIEW (editlist->clist),
                                         visible);
}


void
gimv_elist_set_reorderable (GimvEList *editlist, gboolean reorderble)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   gimv_list_view_set_reorderable (editlist->clist, reorderble,
                                   cb_editlist_reordered, editlist);

   if (reorderble)
      gtk_widget_set_visible (editlist->move_button_area, TRUE);
   else
      gtk_widget_set_visible (editlist->move_button_area, FALSE);
}


void
gimv_elist_set_auto_sort (GimvEList *editlist, gint column)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (column < editlist->columns);

   /* clicking the title of the column sorts the list */
   if (column >= 0)
      gimv_column_view_set_sort_column (
         GTK_COLUMN_VIEW (editlist->clist),
         gimv_column_view_get_column (GTK_COLUMN_VIEW (editlist->clist), column),
         column);
   else
      gimv_column_view_set_sort_column (GTK_COLUMN_VIEW (editlist->clist),
                                        NULL, -1);
}


gboolean
gimv_elist_get_reorderable (GimvEList *editlist)
{
   g_return_val_if_fail (GIMV_IS_ELIST (editlist), FALSE);

   return gimv_list_view_get_reorderable (editlist->clist);
}


gint
gimv_elist_append_row (GimvEList *editlist, gchar *data[])
{
   gint retval;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), -1);
   g_return_val_if_fail (editlist->max_row < 0
                         || editlist->rows <= editlist->max_row, -1);


   {
      GimvRow *row;
      GType *types;
      gint i;

      types = g_new0 (GType, ALL_COLUMNS (editlist->columns));
      for (i = 0; i < editlist->columns; i++)
         types[i] = G_TYPE_STRING;
      types[ROWDATA (editlist->columns)]    = G_TYPE_POINTER;
      types[ROWDESTROY (editlist->columns)] = G_TYPE_POINTER;
      row = gimv_row_newv (ALL_COLUMNS (editlist->columns), types);
      g_free (types);

      for (i = 0; i < editlist->columns; i++) {
         gimv_row_set (row, i, data[i], -1);
      }
      g_list_store_append (list_widget_get_store (editlist->clist), row);
      g_object_unref (row);
   }
   editlist->rows = list_widget_get_row_num(editlist->clist);
   retval = editlist->rows - 1;


   gimv_elist_set_sensitive (editlist);

   return retval;
}


void
gimv_elist_remove_row (GimvEList *editlist, gint row)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (row >= 0 && row < editlist->rows);

   gimv_elist_set_row_data (editlist, row, NULL);

   if ((guint) row < list_widget_get_row_num (editlist->clist)) {
      g_list_store_remove (list_widget_get_store (editlist->clist), row);
      editlist->selected = -1;
   }

   editlist->rows = list_widget_get_row_num(editlist->clist);

   gimv_elist_updated (editlist);
   gimv_elist_set_sensitive (editlist);
}


gint
gimv_elist_get_n_rows (GimvEList *editlist)
{
   g_return_val_if_fail (GIMV_IS_ELIST (editlist), -1);

   return editlist->rows;
}


gint
gimv_elist_get_selected_row (GimvEList *editlist)
{
   g_return_val_if_fail (GIMV_IS_ELIST (editlist), -1);

   return editlist->selected;
}


gchar **
gimv_elist_get_row_text (GimvEList *editlist, gint row)
{
   gchar **text;
   gint i;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (row >= 0 &&  row < editlist->rows, NULL);
   g_return_val_if_fail (editlist->columns > 0, NULL);

   text = g_new0 (gchar *, editlist->columns + 1);


   {
      GimvRow *r = list_widget_get_row (editlist->clist, row);

      if (!r) {
         g_free (text);
         g_return_val_if_reached (NULL);
      }

      for (i = 0; i < editlist->columns; i++) {
         text[i] = NULL;
         gimv_row_get (r, i, &text[i], -1);
         if (!text[i])
            text[i] = g_strdup ("");
      }
   }


   text[editlist->columns] = NULL;

   return text;
}


gchar *
gimv_elist_get_cell_text (GimvEList *editlist, gint row, gint col)
{
   gchar *text = NULL;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (row >= 0 &&  row < editlist->rows, NULL);
   g_return_val_if_fail (editlist->columns > 0, NULL);
   g_return_val_if_fail (col < editlist->columns, NULL);


   {
      GimvRow *r = list_widget_get_row (editlist->clist, row);

      g_return_val_if_fail (r, NULL);

      gimv_row_get (r, col, &text, -1);
   }


   return text;
}


void
gimv_elist_set_row_data (GimvEList *editlist,
                         gint          row,
                         gpointer      data)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (row >= 0 &&  row < editlist->rows);

   gimv_elist_set_row_data_full (editlist, row, data, NULL);
}


void
gimv_elist_set_row_data_full (GimvEList *editlist,
                              gint          row,
                              gpointer      data,
                              GDestroyNotify destroy_fn)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (row >= 0 &&  row < editlist->rows);

   {
      GimvRow *r;
      GDestroyNotify destroy;
      gpointer rowdata = gimv_elist_get_row_data (editlist, row);

      if (rowdata) {
         destroy = g_hash_table_lookup (editlist->rowdata_destroy_fn_table,
                                        rowdata);
         if (destroy) {
            destroy (rowdata);
            g_hash_table_remove (editlist->rowdata_destroy_fn_table, rowdata);
         }

         g_hash_table_remove (editlist->rowdata_table, rowdata);
      }

      r = list_widget_get_row (editlist->clist, row);
      g_return_if_fail (r);

      gimv_row_set (r,
                    ROWDATA(editlist->columns),    data,
                    ROWDESTROY(editlist->columns), destroy_fn,
                    -1);

      if (data) {
         g_hash_table_insert (editlist->rowdata_table,
                              data, data);
         g_hash_table_insert (editlist->rowdata_destroy_fn_table,
                              data, destroy_fn);
      }
   }

   gimv_elist_updated (editlist);
}


gpointer
gimv_elist_get_row_data (GimvEList *editlist,
                            gint          row)
{
   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (row >= 0 &&  row < editlist->rows, NULL);

   {
      GimvRow *r = list_widget_get_row (editlist->clist, row);

      if (!r) return NULL;

      return gimv_row_peek_pointer (r, ROWDATA (editlist->columns));
   }
}


void
gimv_elist_unselect_all (GimvEList *editlist)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   gimv_list_view_unselect_all (editlist->clist);
   editlist->selected = -1;

   gimv_elist_set_sensitive (editlist);
}


void
gimv_elist_set_max_row (GimvEList *editlist, gint rownum)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   editlist->max_row = rownum;
}


void
gimv_elist_set_column_funcs (GimvEList *editlist,
                             GtkWidget *widget,
                             gint column,
                             GimvEListSetDataFn set_data_fn,
                             GimvEListGetDataFn get_data_fn,
                             GimvEListResetFn   reset_fn,
                             gpointer coldata,
                             GDestroyNotify destroy_fn)
{
   GimvEListColumnFuncTable *table;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (column >= 0 && column < editlist->columns);

   table = editlist->column_func_tables[column];

   table->widget      = widget;
   table->coldata     = coldata;
   table->destroy_fn  = destroy_fn;

   table->set_data_fn = set_data_fn;
   table->get_data_fn = get_data_fn;
   table->reset_fn    = reset_fn;
}


void
gimv_elist_set_get_row_data_func (GimvEList *editlist,
                                     GimvEListGetRowDataFn get_rowdata_func)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   editlist->get_rowdata_fn = get_rowdata_func;
}


void
gimv_elist_edit_area_set_value_changed (GimvEList *editlist)
{
   g_return_if_fail (GIMV_IS_ELIST (editlist));

   editlist->edit_area_flags &= ~GIMV_ELIST_EDIT_AREA_VALUE_INITIALIZED;
   editlist->edit_area_flags |= GIMV_ELIST_EDIT_AREA_VALUE_CHANGED;

   gimv_elist_set_sensitive (editlist);
}


GimvEListConfirmFlags
gimv_elist_action_confirm (GimvEList *editlist,
                           GimvEListActionType type)
{
   GimvEListConfirmFlags retval = 0;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), retval);

   if (editlist->edit_area_flags & GIMV_ELIST_EDIT_AREA_VALUE_INITIALIZED)
      retval |= GIMV_ELIST_CONFIRM_CANNOT_NEW;

   if (editlist->max_row >= 0 && editlist->rows >= editlist->max_row)
      retval |= GIMV_ELIST_CONFIRM_CANNOT_ADD;

   if (!(editlist->edit_area_flags & GIMV_ELIST_EDIT_AREA_VALUE_CHANGED))
      retval |= GIMV_ELIST_CONFIRM_CANNOT_CHANGE;

   if (editlist->selected < 0 || editlist->selected >= editlist->rows) {
      retval |= GIMV_ELIST_CONFIRM_CANNOT_CHANGE;
      retval |= GIMV_ELIST_CONFIRM_CANNOT_DELETE;
   }

   g_signal_emit (G_OBJECT (editlist),
                  gimv_elist_signals[ACTION_CONFIRM_SIGNAL], 0,
                    type,
                    editlist->selected,
                    &retval);

   return retval;
}


void
gimv_elist_set_action_button_sensitive (GimvEList *editlist)
{
   GimvEListConfirmFlags flags;

   g_return_if_fail (GIMV_IS_ELIST (editlist));

   flags = gimv_elist_action_confirm (editlist,
                                         GIMV_ELIST_ACTION_SET_SENSITIVE);

   gtk_widget_set_sensitive (editlist->new_button,
                             !(flags & GIMV_ELIST_CONFIRM_CANNOT_NEW));
   gtk_widget_set_sensitive (editlist->add_button,
                             !(flags & GIMV_ELIST_CONFIRM_CANNOT_ADD));
   gtk_widget_set_sensitive (editlist->change_button,
                             !(flags & GIMV_ELIST_CONFIRM_CANNOT_CHANGE));
   gtk_widget_set_sensitive (editlist->del_button,
                             !(flags & GIMV_ELIST_CONFIRM_CANNOT_DELETE));
}



/*******************************************************************************
 *
 *  convenient function to create entry.
 *
 *******************************************************************************/
typedef struct GimvEListEntryData_Tag
{
   GimvEList *editlist;
   GtkWidget    *entry;
   gchar        *init_string;
   gboolean      allow_empty;
} GimvEListEntryData;


static void
cb_editlist_entry_set_data (GimvEList *editlist, GtkWidget *widget,
                            gint row, gint col,
                            const gchar *text, gpointer coldata)
{
   GimvEListEntryData *entry_data = coldata;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (GTK_IS_ENTRY (widget));
   g_return_if_fail (entry_data);

   if (text)
      gtk_editable_set_text (GTK_EDITABLE (widget), text);
}


static gchar *
cb_editlist_entry_get_data (GimvEList *editlist,
                            GimvEListActionType type,
                            GtkWidget *widget,
                            gpointer coldata)
{
   GimvEListEntryData *entry_data = coldata;
   const gchar *text;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (GTK_IS_ENTRY (widget), NULL);
   g_return_val_if_fail (entry_data, NULL);

   text = gtk_editable_get_text (GTK_EDITABLE (widget));

   if (text)
      return g_strdup (text);
   else
      return NULL;
}


static void
cb_editlist_entry_reset (GimvEList *editlist,
                         GtkWidget *widget,
                         gpointer coldata)
{
   GimvEListEntryData *entry_data = coldata;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (GTK_IS_ENTRY (widget));
   g_return_if_fail (entry_data);

   gtk_editable_set_text (GTK_EDITABLE (widget), entry_data->init_string);
}


static void
cb_editlist_entry_changed (GtkEntry *entry, gpointer data)
{
   GimvEListEntryData *entry_data = data;
   GimvEList *editlist;

   g_return_if_fail (entry_data);
   g_return_if_fail (GIMV_IS_ELIST (entry_data->editlist));

   editlist = entry_data->editlist;

   gimv_elist_edit_area_set_value_changed (editlist);
}


static void
editlist_entry_destroy (gpointer data)
{
   GimvEListEntryData *entry_data = data;

   g_free (entry_data->init_string);
   g_free (entry_data);
}


static void
cb_editlist_entry_confirm (GimvEList *editlist,
                           GimvEListActionType type,
                           gint selected_row,
                           GimvEListConfirmFlags *flags,
                           gpointer data)
{
   GimvEListEntryData *entry_data = data;
   const gchar *text;

   text = gtk_editable_get_text (GTK_EDITABLE (entry_data->entry));
   if (!entry_data->allow_empty && (!text || !*text)) {
      *flags |= GIMV_ELIST_CONFIRM_CANNOT_ADD;
      *flags |= GIMV_ELIST_CONFIRM_CANNOT_CHANGE;
   }

   return;
}


GtkWidget *
gimv_elist_create_entry (GimvEList *editlist, gint column,
                         const gchar *init_string,
                         gboolean allow_empty)
{
   GimvEListEntryData *entry_data;
   GtkWidget *entry;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (column >= 0 && column < editlist->columns, NULL);

   entry_data = g_new0 (GimvEListEntryData, 1);
   entry_data->editlist = editlist;
   if (!init_string || !*init_string)
      entry_data->init_string = g_strdup ("\0");
   else
      entry_data->init_string = g_strdup (init_string);
   entry_data->allow_empty = allow_empty;

   entry_data->entry = entry = gtk_entry_new ();

   gtk_editable_set_text (GTK_EDITABLE (entry_data->entry), entry_data->init_string);

   gimv_elist_set_column_funcs (editlist,
                                entry, column,
                                cb_editlist_entry_set_data,
                                cb_editlist_entry_get_data,
                                cb_editlist_entry_reset,
                                entry_data,
                                editlist_entry_destroy);

   g_signal_connect (G_OBJECT (editlist), "action_confirm",
                     G_CALLBACK (cb_editlist_entry_confirm),
                     entry_data);
   g_signal_connect (G_OBJECT (entry),"changed",
                     G_CALLBACK (cb_editlist_entry_changed),
                     entry_data);

   gimv_elist_set_sensitive (editlist);

   return entry;
}



/*******************************************************************************
 *
 *  convenient function to create check button.
 *
 *******************************************************************************/
typedef struct GimvEListCheckButtonData_Tag
{
   GimvEList *editlist;
   gboolean   init_value;
   gchar     *true_string;
   gchar     *false_string;
} GimvEListCheckButtonData;

static void
cb_editlist_check_button_set_data (GimvEList *editlist, GtkWidget *widget,
                                   gint row, gint col,
                                   const gchar *text, gpointer coldata)
{
   GimvEListCheckButtonData *button_data = coldata;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (GTK_IS_CHECK_BUTTON (widget));
   g_return_if_fail (button_data);

   if (row < 0) return;

   if (text && *text && !strcmp (text, button_data->true_string)) {
      gimv_toggle_set_active (GTK_WIDGET (widget), TRUE);
   } else {
      gimv_toggle_set_active (GTK_WIDGET (widget), FALSE);
   }
}


static gchar *
cb_editlist_check_button_get_data (GimvEList *editlist,
                                   GimvEListActionType type,
                                   GtkWidget *widget,
                                   gpointer coldata)
{
   GimvEListCheckButtonData *button_data = coldata;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (GTK_IS_CHECK_BUTTON (widget), NULL);
   g_return_val_if_fail (button_data, NULL);

   if (gimv_toggle_get_active (GTK_WIDGET (widget)))
      return g_strdup (button_data->true_string);
   else
      return g_strdup (button_data->false_string);
}


static void
cb_editlist_check_button_reset (GimvEList *editlist,
                                GtkWidget *widget,
                                gpointer coldata)
{
   GimvEListCheckButtonData *button_data = coldata;

   g_return_if_fail (GIMV_IS_ELIST (editlist));
   g_return_if_fail (GTK_IS_CHECK_BUTTON (widget));

   gimv_toggle_set_active (GTK_WIDGET (widget), button_data->init_value);
}


static void
cb_editlist_check_button_toggled (GtkCheckButton *check_button, gpointer data)
{
   GimvEListCheckButtonData *button_data = data;
   gimv_elist_edit_area_set_value_changed (button_data->editlist);
}


static void
editlist_check_button_destroy (gpointer data)
{
   GimvEListCheckButtonData *button_data = data;

   g_free (button_data->true_string);
   g_free (button_data->false_string);
   g_free (button_data);
}


GtkWidget *
gimv_elist_create_check_button (GimvEList *editlist, gint column,
                                const gchar *label,
                                gboolean init_value,
                                const gchar *true_string,
                                const gchar *false_string)
{
   GimvEListCheckButtonData *button_data;
   GtkWidget *check_button;

   g_return_val_if_fail (GIMV_IS_ELIST (editlist), NULL);
   g_return_val_if_fail (column >= 0 && column < editlist->columns, NULL);
   g_return_val_if_fail (true_string && *true_string, NULL);
   g_return_val_if_fail (false_string && *false_string, NULL);

   button_data = g_new0 (GimvEListCheckButtonData, 1);
   button_data->editlist     = editlist;
   button_data->init_value   = init_value;
   button_data->true_string  = g_strdup (true_string);
   button_data->false_string = g_strdup (false_string);

   if (label)
      check_button = gtk_check_button_new_with_label (label);
   else
      check_button = gtk_check_button_new ();

   gimv_toggle_set_active (GTK_WIDGET (check_button), button_data->init_value);

   gimv_elist_set_column_funcs (GIMV_ELIST (editlist),
                                   check_button, column,
                                   cb_editlist_check_button_set_data,
                                   cb_editlist_check_button_get_data,
                                   cb_editlist_check_button_reset,
                                   button_data,
                                   editlist_check_button_destroy);

   g_signal_connect (G_OBJECT (check_button),"toggled",
                     G_CALLBACK (cb_editlist_check_button_toggled),
                     button_data);

   gimv_elist_set_sensitive (editlist);

   return check_button;
}
