/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2001-2004 Takuro Ashie
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
 */

#include <string.h>
#include <gobject/gvaluecollector.h>

#include "gimv_list_view.h"
#include "gimv_gtk4_compat.h"


/******************************************************************************
 *
 *   GimvRow
 *
 ******************************************************************************/
struct _GimvRow
{
   GObject     parent;

   gint        n_columns;
   GValue     *values;

   GListStore *children;
   GimvRow    *parent_row;   /* weak pointer */
};

enum {
   ROW_CHANGED,
   ROW_LAST_SIGNAL
};

static guint row_signals[ROW_LAST_SIGNAL] = {0};

G_DEFINE_FINAL_TYPE (GimvRow, gimv_row, G_TYPE_OBJECT)


static void
gimv_row_init (GimvRow *row)
{
}


static void
row_set_parent (GimvRow *row, GimvRow *parent)
{
   if (row->parent_row)
      g_object_remove_weak_pointer (G_OBJECT (row->parent_row),
                                    (gpointer *) &row->parent_row);
   row->parent_row = parent;
   if (parent)
      g_object_add_weak_pointer (G_OBJECT (parent),
                                 (gpointer *) &row->parent_row);
}


static void
gimv_row_dispose (GObject *object)
{
   GimvRow *row = GIMV_ROW (object);

   row_set_parent (row, NULL);
   g_clear_object (&row->children);

   G_OBJECT_CLASS (gimv_row_parent_class)->dispose (object);
}


static void
gimv_row_finalize (GObject *object)
{
   GimvRow *row = GIMV_ROW (object);
   gint i;

   for (i = 0; i < row->n_columns; i++)
      g_value_unset (&row->values[i]);
   g_free (row->values);

   G_OBJECT_CLASS (gimv_row_parent_class)->finalize (object);
}


static void
gimv_row_class_init (GimvRowClass *klass)
{
   GObjectClass *object_class = G_OBJECT_CLASS (klass);

   object_class->dispose  = gimv_row_dispose;
   object_class->finalize = gimv_row_finalize;

   row_signals[ROW_CHANGED]
      = g_signal_new ("changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}


GimvRow *
gimv_row_newv (gint n_columns, const GType *types)
{
   GimvRow *row;
   gint i;

   g_return_val_if_fail (n_columns > 0, NULL);
   g_return_val_if_fail (types, NULL);

   row = g_object_new (GIMV_TYPE_ROW, NULL);
   row->n_columns = n_columns;
   row->values = g_new0 (GValue, n_columns);
   for (i = 0; i < n_columns; i++)
      g_value_init (&row->values[i], types[i]);

   return row;
}


GimvRow *
gimv_row_new (gint n_columns, ...)
{
   GimvRow *row;
   GType *types;
   va_list args;
   gint i;

   g_return_val_if_fail (n_columns > 0, NULL);

   types = g_new (GType, n_columns);
   va_start (args, n_columns);
   for (i = 0; i < n_columns; i++)
      types[i] = va_arg (args, GType);
   va_end (args);

   row = gimv_row_newv (n_columns, types);
   g_free (types);

   return row;
}


gint
gimv_row_get_n_columns (GimvRow *row)
{
   g_return_val_if_fail (GIMV_IS_ROW (row), 0);
   return row->n_columns;
}


void
gimv_row_changed (GimvRow *row)
{
   g_return_if_fail (GIMV_IS_ROW (row));
   g_signal_emit (row, row_signals[ROW_CHANGED], 0);
}


void
gimv_row_set_value (GimvRow *row, gint column, const GValue *value)
{
   g_return_if_fail (GIMV_IS_ROW (row));
   g_return_if_fail (column >= 0 && column < row->n_columns);
   g_return_if_fail (value);

   g_value_copy (value, &row->values[column]);
   gimv_row_changed (row);
}


void
gimv_row_set (GimvRow *row, ...)
{
   va_list args;
   gint column;

   g_return_if_fail (GIMV_IS_ROW (row));

   va_start (args, row);
   for (column = va_arg (args, gint); column != -1; column = va_arg (args, gint)) {
      GValue value = G_VALUE_INIT;
      gchar *error = NULL;

      if (column < 0 || column >= row->n_columns) {
         g_warning ("%s: invalid column number %d", G_STRLOC, column);
         break;
      }

      G_VALUE_COLLECT_INIT (&value, G_VALUE_TYPE (&row->values[column]),
                            args, 0, &error);
      if (error) {
         g_warning ("%s: %s", G_STRLOC, error);
         g_free (error);
         break;
      }

      g_value_unset (&row->values[column]);
      row->values[column] = value;   /* takes over the collected value */
   }
   va_end (args);

   gimv_row_changed (row);
}


void
gimv_row_get (GimvRow *row, ...)
{
   va_list args;
   gint column;

   g_return_if_fail (GIMV_IS_ROW (row));

   va_start (args, row);
   for (column = va_arg (args, gint); column != -1; column = va_arg (args, gint)) {
      gchar *error = NULL;

      if (column < 0 || column >= row->n_columns) {
         g_warning ("%s: invalid column number %d", G_STRLOC, column);
         break;
      }

      G_VALUE_LCOPY (&row->values[column], args, 0, &error);
      if (error) {
         g_warning ("%s: %s", G_STRLOC, error);
         g_free (error);
         break;
      }
   }
   va_end (args);
}


const GValue *
gimv_row_peek_value (GimvRow *row, gint column)
{
   g_return_val_if_fail (GIMV_IS_ROW (row), NULL);
   g_return_val_if_fail (column >= 0 && column < row->n_columns, NULL);
   return &row->values[column];
}


const gchar *
gimv_row_peek_string (GimvRow *row, gint column)
{
   const GValue *value = gimv_row_peek_value (row, column);
   g_return_val_if_fail (value && G_VALUE_HOLDS_STRING (value), NULL);
   return g_value_get_string (value);
}


gpointer
gimv_row_peek_pointer (GimvRow *row, gint column)
{
   const GValue *value = gimv_row_peek_value (row, column);
   g_return_val_if_fail (value && G_VALUE_HOLDS_POINTER (value), NULL);
   return g_value_get_pointer (value);
}


gpointer
gimv_row_peek_object (GimvRow *row, gint column)
{
   const GValue *value = gimv_row_peek_value (row, column);
   g_return_val_if_fail (value && G_VALUE_HOLDS_OBJECT (value), NULL);
   return g_value_get_object (value);
}


gint
gimv_row_peek_int (GimvRow *row, gint column)
{
   const GValue *value = gimv_row_peek_value (row, column);
   g_return_val_if_fail (value && G_VALUE_HOLDS_INT (value), 0);
   return g_value_get_int (value);
}


gboolean
gimv_row_peek_boolean (GimvRow *row, gint column)
{
   const GValue *value = gimv_row_peek_value (row, column);
   g_return_val_if_fail (value && G_VALUE_HOLDS_BOOLEAN (value), FALSE);
   return g_value_get_boolean (value);
}


GListStore *
gimv_row_get_children (GimvRow *row, gboolean create)
{
   g_return_val_if_fail (GIMV_IS_ROW (row), NULL);

   if (!row->children && create)
      row->children = g_list_store_new (GIMV_TYPE_ROW);

   return row->children;
}


void
gimv_row_remove_children (GimvRow *row)
{
   g_return_if_fail (GIMV_IS_ROW (row));
   if (row->children)
      g_list_store_remove_all (row->children);
}


GimvRow *
gimv_row_get_parent (GimvRow *row)
{
   g_return_val_if_fail (GIMV_IS_ROW (row), NULL);
   return row->parent_row;
}


void
gimv_row_insert_child (GimvRow *parent, guint position, GimvRow *child)
{
   GListStore *children;

   g_return_if_fail (GIMV_IS_ROW (parent));
   g_return_if_fail (GIMV_IS_ROW (child));

   children = gimv_row_get_children (parent, TRUE);
   row_set_parent (child, parent);
   if (position > g_list_model_get_n_items (G_LIST_MODEL (children)))
      position = g_list_model_get_n_items (G_LIST_MODEL (children));
   g_list_store_insert (children, position, child);
}


void
gimv_row_append_child (GimvRow *parent, GimvRow *child)
{
   gimv_row_insert_child (parent, G_MAXUINT, child);
}


gboolean
gimv_row_is_ancestor (GimvRow *ancestor, GimvRow *row)
{
   g_return_val_if_fail (GIMV_IS_ROW (ancestor), FALSE);
   g_return_val_if_fail (GIMV_IS_ROW (row), FALSE);

   for (row = row->parent_row; row; row = row->parent_row)
      if (row == ancestor) return TRUE;

   return FALSE;
}


GListStore *
gimv_row_store_new (void)
{
   return g_list_store_new (GIMV_TYPE_ROW);
}


GimvRow *
gimv_row_store_peek (GListModel *store, guint position)
{
   gpointer item;
   GimvRow *row;

   g_return_val_if_fail (G_IS_LIST_MODEL (store), NULL);

   item = g_list_model_get_item (store, position);
   if (!item) return NULL;

   /* a GtkTreeListRow is not kept by its model, but the row is kept by its
      store (gimv_row_from_item () does not reference it) */
   row = gimv_row_from_item (item);
   g_object_unref (item);

   return row;
}


gboolean
gimv_row_store_find (GListModel *store, GimvRow *row, guint *position)
{
   guint i, n;

   g_return_val_if_fail (G_IS_LIST_MODEL (store), FALSE);

   if (G_IS_LIST_STORE (store))
      return g_list_store_find (G_LIST_STORE (store), row, position);

   n = g_list_model_get_n_items (store);
   for (i = 0; i < n; i++) {
      if (gimv_row_store_peek (store, i) == row) {
         if (position) *position = i;
         return TRUE;
      }
   }

   return FALSE;
}


GimvRow *
gimv_row_from_item (gpointer item)
{
   if (!item) return NULL;
   if (GTK_IS_TREE_LIST_ROW (item)) {
      GimvRow *row = gtk_tree_list_row_get_item (GTK_TREE_LIST_ROW (item));
      if (row) g_object_unref (row);   /* the model keeps a reference */
      return GIMV_IS_ROW (row) ? row : NULL;
   }
   return GIMV_IS_ROW (item) ? GIMV_ROW (item) : NULL;
}


static GListModel *
tree_list_create_children (gpointer item, gpointer data)
{
   GimvRow *row = GIMV_ROW (item);

   if (!row->children) return NULL;
   return G_LIST_MODEL (g_object_ref (row->children));
}


GtkTreeListModel *
gimv_tree_list_model_new (GListModel *root)
{
   g_return_val_if_fail (G_IS_LIST_MODEL (root), NULL);

   return gtk_tree_list_model_new (g_object_ref (root), FALSE, FALSE,
                                   tree_list_create_children, NULL, NULL);
}


GtkTreeListRow *
gimv_tree_list_model_get_row_for (GtkTreeListModel *model, GimvRow *row,
                                  guint *position)
{
   GimvRow *parent;
   GtkTreeListRow *parent_row = NULL, *tree_row = NULL;
   guint pos;

   g_return_val_if_fail (GTK_IS_TREE_LIST_MODEL (model), NULL);
   g_return_val_if_fail (GIMV_IS_ROW (row), NULL);

   parent = gimv_row_get_parent (row);

   if (!parent) {
      GListModel *root = gtk_tree_list_model_get_model (model);
      if (!gimv_row_store_find (root, row, &pos)) return NULL;
      tree_row = gtk_tree_list_model_get_child_row (model, pos);
   } else {
      parent_row = gimv_tree_list_model_get_row_for (model, parent, NULL);
      if (!parent_row) return NULL;
      if (gtk_tree_list_row_get_expanded (parent_row)
          && parent->children
          && g_list_store_find (parent->children, row, &pos))
      {
         tree_row = gtk_tree_list_row_get_child_row (parent_row, pos);
      }
      g_object_unref (parent_row);
   }

   if (tree_row && position)
      *position = gtk_tree_list_row_get_position (tree_row);

   return tree_row;
}


void
gimv_tree_list_model_expand_to (GtkTreeListModel *model, GimvRow *row)
{
   GtkTreeListRow *tree_row;
   GimvRow *parent;

   g_return_if_fail (GTK_IS_TREE_LIST_MODEL (model));
   g_return_if_fail (GIMV_IS_ROW (row));

   parent = gimv_row_get_parent (row);
   if (parent)
      gimv_tree_list_model_expand_to (model, parent);

   tree_row = gimv_tree_list_model_get_row_for (model, row, NULL);
   if (tree_row) {
      gtk_tree_list_row_set_expanded (tree_row, TRUE);
      g_object_unref (tree_row);
   }
}



/******************************************************************************
 *
 *   list item factories
 *
 ******************************************************************************/
#define LIST_ITEM_KEY      "gimv-list-item"
#define CELL_CONTENT_KEY   "gimv-cell-content"

typedef struct {
   GimvCellSetupFunc setup;
   GimvCellBindFunc  bind;
   gpointer          data;
   GDestroyNotify    destroy;
   gboolean          expander;
} CellFactory;

/* per list item */
typedef struct {
   CellFactory *factory;
   GtkListItem *item;      /* not referenced: the data is owned by it */
   GtkWidget   *content;
   GimvRow     *row;       /* bound row (referenced) */
   gulong       changed_id;
   GtkTreeListRow *tree_row; /* bound tree row (referenced) */
   gulong       expanded_id;
} CellItem;


static void
cell_factory_free (gpointer data)
{
   CellFactory *factory = data;

   if (factory->destroy)
      factory->destroy (factory->data);
   g_free (factory);
}


static void
cell_item_free (gpointer data)
{
   CellItem *ci = data;

   if (ci->row) {
      g_signal_handler_disconnect (ci->row, ci->changed_id);
      g_object_unref (ci->row);
   }
   if (ci->tree_row) {
      g_signal_handler_disconnect (ci->tree_row, ci->expanded_id);
      g_object_unref (ci->tree_row);
   }
   g_free (ci);
}


static void
cb_cell_setup (GtkSignalListItemFactory *f, GObject *object, CellFactory *factory)
{
   GtkListItem *item = GTK_LIST_ITEM (object);
   CellItem *ci;
   GtkWidget *child;

   ci = g_new0 (CellItem, 1);
   ci->factory = factory;
   ci->item    = item;
   ci->content = factory->setup (factory->data);

   if (factory->expander) {
      child = gtk_tree_expander_new ();
      gtk_tree_expander_set_child (GTK_TREE_EXPANDER (child), ci->content);
   } else {
      child = ci->content;
   }

   g_object_set_data (G_OBJECT (child), LIST_ITEM_KEY, item);
   g_object_set_data (G_OBJECT (child), CELL_CONTENT_KEY, ci->content);
   gtk_list_item_set_child (item, child);
   g_object_set_data_full (object, "gimv-cell-item", ci, cell_item_free);
}


static void
cell_item_update (CellItem *ci)
{
   if (ci->factory->bind)
      ci->factory->bind (ci->content, ci->row, ci->item, ci->factory->data);
}


static void
cb_row_changed (GimvRow *row, CellItem *ci)
{
   cell_item_update (ci);
}


/* the cell may depend on the expanded state (open folder icons) */
static void
cb_row_expanded (GtkTreeListRow *tree_row, GParamSpec *pspec, CellItem *ci)
{
   cell_item_update (ci);
}


static void
cb_cell_bind (GtkSignalListItemFactory *f, GObject *object, CellFactory *factory)
{
   GtkListItem *item = GTK_LIST_ITEM (object);
   CellItem *ci = g_object_get_data (object, "gimv-cell-item");
   gpointer model_item = gtk_list_item_get_item (item);
   GimvRow *row;

   g_return_if_fail (ci);

   if (factory->expander) {
      GtkWidget *expander = gtk_list_item_get_child (item);
      gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander),
                                      GTK_IS_TREE_LIST_ROW (model_item)
                                      ? GTK_TREE_LIST_ROW (model_item) : NULL);
   }

   row = gimv_row_from_item (model_item);
   if (!row) return;

   ci->row = g_object_ref (row);
   ci->changed_id = g_signal_connect (row, "changed",
                                      G_CALLBACK (cb_row_changed), ci);
   if (GTK_IS_TREE_LIST_ROW (model_item)) {
      ci->tree_row = g_object_ref (model_item);
      ci->expanded_id = g_signal_connect (model_item, "notify::expanded",
                                          G_CALLBACK (cb_row_expanded), ci);
   }
   cell_item_update (ci);
}


static void
cb_cell_unbind (GtkSignalListItemFactory *f, GObject *object, CellFactory *factory)
{
   GtkListItem *item = GTK_LIST_ITEM (object);
   CellItem *ci = g_object_get_data (object, "gimv-cell-item");

   g_return_if_fail (ci);

   if (factory->expander) {
      GtkWidget *expander = gtk_list_item_get_child (item);
      gtk_tree_expander_set_list_row (GTK_TREE_EXPANDER (expander), NULL);
   }

   if (ci->row) {
      g_signal_handler_disconnect (ci->row, ci->changed_id);
      g_clear_object (&ci->row);
      ci->changed_id = 0;
   }
   if (ci->tree_row) {
      g_signal_handler_disconnect (ci->tree_row, ci->expanded_id);
      g_clear_object (&ci->tree_row);
      ci->expanded_id = 0;
   }
}


GtkListItemFactory *
gimv_cell_factory_new (GimvCellSetupFunc setup, GimvCellBindFunc bind,
                       gpointer data, GDestroyNotify destroy,
                       gboolean expander)
{
   GtkListItemFactory *f;
   CellFactory *factory;

   g_return_val_if_fail (setup, NULL);

   factory = g_new0 (CellFactory, 1);
   factory->setup    = setup;
   factory->bind     = bind;
   factory->data     = data;
   factory->destroy  = destroy;
   factory->expander = expander;

   f = gtk_signal_list_item_factory_new ();
   g_object_set_data_full (G_OBJECT (f), "gimv-cell-factory",
                           factory, cell_factory_free);
   g_signal_connect (f, "setup",  G_CALLBACK (cb_cell_setup),  factory);
   g_signal_connect (f, "bind",   G_CALLBACK (cb_cell_bind),   factory);
   g_signal_connect (f, "unbind", G_CALLBACK (cb_cell_unbind), factory);

   return f;
}


GtkTreeListRow *
gimv_list_item_get_tree_row (GtkListItem *item)
{
   gpointer model_item;

   g_return_val_if_fail (GTK_IS_LIST_ITEM (item), NULL);

   model_item = gtk_list_item_get_item (item);
   return GTK_IS_TREE_LIST_ROW (model_item) ? GTK_TREE_LIST_ROW (model_item) : NULL;
}


GtkWidget *
gimv_cell_get_content (GtkWidget *cell)
{
   GtkWidget *content;

   g_return_val_if_fail (GTK_IS_WIDGET (cell), NULL);

   content = g_object_get_data (G_OBJECT (cell), CELL_CONTENT_KEY);
   return content ? content : cell;
}


/* text */
static gchar *
value_to_string (const GValue *value)
{
   GValue str = G_VALUE_INIT;
   gchar *retval;

   if (!value || !G_IS_VALUE (value)) return NULL;

   if (G_VALUE_HOLDS_STRING (value))
      return g_value_dup_string (value);

   if (!g_value_type_transformable (G_VALUE_TYPE (value), G_TYPE_STRING))
      return NULL;

   g_value_init (&str, G_TYPE_STRING);
   g_value_transform (value, &str);
   retval = g_value_dup_string (&str);
   g_value_unset (&str);

   return retval;
}


static GtkWidget *
text_cell_setup (gpointer data)
{
   GtkWidget *label = gtk_label_new (NULL);

   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);

   return label;
}


static void
text_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   gint col = GPOINTER_TO_INT (data);
   gchar *text = value_to_string (gimv_row_peek_value (row, col));

   gtk_label_set_text (GTK_LABEL (cell), text ? text : "");
   g_free (text);
}


GtkListItemFactory *
gimv_text_factory_new (gint text_col, gboolean expander)
{
   return gimv_cell_factory_new (text_cell_setup, text_cell_bind,
                                 GINT_TO_POINTER (text_col), NULL, expander);
}


/* icon & text */
typedef struct {
   gint icon_col;
   gint text_col;
} IconTextColumns;


static GtkWidget *
icon_text_cell_setup (gpointer data)
{
   IconTextColumns *cols = data;
   GtkWidget *box, *picture, *label;

   box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

   picture = gtk_picture_new ();
   gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
   gtk_widget_set_valign (picture, GTK_ALIGN_CENTER);
   gtk_box_append (GTK_BOX (box), picture);
   g_object_set_data (G_OBJECT (box), "gimv-cell-image", picture);

   if (cols->text_col >= 0) {
      label = text_cell_setup (NULL);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_box_append (GTK_BOX (box), label);
      g_object_set_data (G_OBJECT (box), "gimv-cell-label", label);
   }

   return box;
}


GtkWidget *
gimv_icon_text_cell_get_image (GtkWidget *cell)
{
   return g_object_get_data (G_OBJECT (gimv_cell_get_content (cell)),
                             "gimv-cell-image");
}


GtkWidget *
gimv_icon_text_cell_get_label (GtkWidget *cell)
{
   return g_object_get_data (G_OBJECT (gimv_cell_get_content (cell)),
                             "gimv-cell-label");
}


void
gimv_icon_text_cell_set_icon (GtkWidget *cell, const GValue *value)
{
   GtkWidget *picture = gimv_icon_text_cell_get_image (cell);
   GdkPaintable *paintable = NULL;

   g_return_if_fail (GTK_IS_PICTURE (picture));

   if (value && G_VALUE_HOLDS_OBJECT (value)) {
      GObject *obj = g_value_get_object (value);

      if (GDK_IS_PAINTABLE (obj)) {
         paintable = g_object_ref (GDK_PAINTABLE (obj));
      } else if (GDK_IS_PIXBUF (obj)) {
         paintable = GDK_PAINTABLE (gimv_texture_new_for_pixbuf (GDK_PIXBUF (obj)));
      }
   } else if (value && G_VALUE_HOLDS_STRING (value) && g_value_get_string (value)) {
      GtkIconTheme *theme
         = gtk_icon_theme_get_for_display (gtk_widget_get_display (picture));
      paintable = GDK_PAINTABLE (
         gtk_icon_theme_lookup_icon (theme, g_value_get_string (value), NULL,
                                     16, gtk_widget_get_scale_factor (picture),
                                     gtk_widget_get_direction (picture), 0));
   }

   gtk_picture_set_paintable (GTK_PICTURE (picture), paintable);
   gtk_widget_set_visible (picture, paintable != NULL);
   if (paintable) g_object_unref (paintable);
}


static void
icon_text_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   IconTextColumns *cols = data;

   if (cols->icon_col >= 0)
      gimv_icon_text_cell_set_icon (cell, gimv_row_peek_value (row, cols->icon_col));

   if (cols->text_col >= 0) {
      GtkWidget *label = gimv_icon_text_cell_get_label (cell);
      gchar *text = value_to_string (gimv_row_peek_value (row, cols->text_col));
      gtk_label_set_text (GTK_LABEL (label), text ? text : "");
      g_free (text);
   }
}


GtkListItemFactory *
gimv_icon_text_factory_new (gint icon_col, gint text_col, gboolean expander)
{
   IconTextColumns *cols = g_new0 (IconTextColumns, 1);

   cols->icon_col = icon_col;
   cols->text_col = text_col;

   return gimv_cell_factory_new (icon_text_cell_setup, icon_text_cell_bind,
                                 cols, g_free, expander);
}



/******************************************************************************
 *
 *   views
 *
 ******************************************************************************/
GtkSelectionModel *
gimv_selection_model_new (GListModel *model, GtkSelectionMode mode)
{
   GtkSingleSelection *single;

   g_return_val_if_fail (G_IS_LIST_MODEL (model), NULL);

   switch (mode) {
   case GTK_SELECTION_MULTIPLE:
      return GTK_SELECTION_MODEL (gtk_multi_selection_new (model));
   case GTK_SELECTION_NONE:
      return GTK_SELECTION_MODEL (gtk_no_selection_new (model));
   case GTK_SELECTION_SINGLE:
   case GTK_SELECTION_BROWSE:
   default:
      single = gtk_single_selection_new (model);
      gtk_single_selection_set_autoselect (single, FALSE);
      gtk_single_selection_set_can_unselect (single, mode != GTK_SELECTION_BROWSE);
      gtk_single_selection_set_selected (single, GTK_INVALID_LIST_POSITION);
      return GTK_SELECTION_MODEL (single);
   }
}


GtkWidget *
gimv_list_view_new (GListModel *model, GtkSelectionMode mode,
                    GtkListItemFactory *factory)
{
   return gtk_list_view_new (gimv_selection_model_new (model, mode), factory);
}


GtkWidget *
gimv_column_view_new (GListModel *model, GtkSelectionMode mode)
{
   return gtk_column_view_new (gimv_selection_model_new (model, mode));
}



/******************************************************************************
 *
 *   column view
 *
 ******************************************************************************/
GtkColumnViewColumn *
gimv_column_view_append (GtkColumnView *view, const gchar *title,
                         GtkListItemFactory *factory)
{
   GtkColumnViewColumn *column;

   g_return_val_if_fail (GTK_IS_COLUMN_VIEW (view), NULL);

   column = gtk_column_view_column_new (title, factory);
   gtk_column_view_append_column (view, column);
   g_object_unref (column);   /* the view keeps a reference */

   return column;
}


GtkColumnViewColumn *
gimv_column_view_append_text (GtkColumnView *view, const gchar *title,
                              gint text_col)
{
   return gimv_column_view_append (view, title,
                                   gimv_text_factory_new (text_col, FALSE));
}


GtkColumnViewColumn *
gimv_column_view_get_column (GtkColumnView *view, guint index)
{
   GListModel *columns;
   GtkColumnViewColumn *column;

   g_return_val_if_fail (GTK_IS_COLUMN_VIEW (view), NULL);

   columns = gtk_column_view_get_columns (view);
   column = g_list_model_get_item (columns, index);
   if (column) g_object_unref (column);   /* the view keeps a reference */

   return column;
}


guint
gimv_column_view_get_n_columns (GtkColumnView *view)
{
   g_return_val_if_fail (GTK_IS_COLUMN_VIEW (view), 0);
   return g_list_model_get_n_items (gtk_column_view_get_columns (view));
}


/* GtkColumnView has no API to hide the column titles: hide its header
   widget (the child whose CSS name is "header") */
static GtkWidget *
column_view_get_header (GtkColumnView *view)
{
   GtkWidget *child;

   for (child = gtk_widget_get_first_child (GTK_WIDGET (view));
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      if (!strcmp (gtk_widget_get_css_name (child), "header"))
         return child;
   }

   return NULL;
}


void
gimv_column_view_set_headers_visible (GtkColumnView *view, gboolean visible)
{
   GtkWidget *header;

   g_return_if_fail (GTK_IS_COLUMN_VIEW (view));

   header = column_view_get_header (view);
   if (header)
      gtk_widget_set_visible (header, visible);
}


gboolean
gimv_column_view_get_headers_visible (GtkColumnView *view)
{
   GtkWidget *header;

   g_return_val_if_fail (GTK_IS_COLUMN_VIEW (view), FALSE);

   header = column_view_get_header (view);
   return header ? gtk_widget_get_visible (header) : FALSE;
}



/******************************************************************************
 *
 *   GtkListView / GtkColumnView utilities
 *
 ******************************************************************************/
GtkSelectionModel *
gimv_list_view_get_selection (GtkWidget *view)
{
   if (GTK_IS_COLUMN_VIEW (view))
      return gtk_column_view_get_model (GTK_COLUMN_VIEW (view));
   if (GTK_IS_LIST_VIEW (view))
      return gtk_list_view_get_model (GTK_LIST_VIEW (view));

   g_return_val_if_reached (NULL);
}


GListModel *
gimv_list_view_get_model (GtkWidget *view)
{
   GtkSelectionModel *selection = gimv_list_view_get_selection (view);
   GListModel *model = NULL;

   if (!selection) return NULL;

   if (GTK_IS_SINGLE_SELECTION (selection))
      model = gtk_single_selection_get_model (GTK_SINGLE_SELECTION (selection));
   else if (GTK_IS_MULTI_SELECTION (selection))
      model = gtk_multi_selection_get_model (GTK_MULTI_SELECTION (selection));
   else if (GTK_IS_NO_SELECTION (selection))
      model = gtk_no_selection_get_model (GTK_NO_SELECTION (selection));

   return model;
}


/* the GtkListItem of a cell widget made by gimv_cell_factory_new () */
static GtkListItem *
widget_get_list_item (GtkWidget *widget)
{
   return g_object_get_data (G_OBJECT (widget), LIST_ITEM_KEY);
}


static GtkListItem *
find_list_item_in (GtkWidget *widget)
{
   GtkListItem *item = widget_get_list_item (widget);
   GtkWidget *child;

   if (item) return item;

   for (child = gtk_widget_get_first_child (widget);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      item = find_list_item_in (child);
      if (item) return item;
   }

   return NULL;
}


static gboolean
is_row_widget (GtkWidget *widget)
{
   return !strcmp (gtk_widget_get_css_name (widget), "row");
}


/* the row widget (CSS name "row") at x, y */
static GtkWidget *
row_widget_at (GtkWidget *view, gdouble x, gdouble y, GtkListItem **item_ret)
{
   GtkWidget *widget, *row_widget = NULL;
   GtkListItem *item = NULL;

   widget = gtk_widget_pick (view, x, y, GTK_PICK_INSENSITIVE);

   for (; widget && widget != view; widget = gtk_widget_get_parent (widget)) {
      if (!item)
         item = widget_get_list_item (widget);
      if (is_row_widget (widget)) {
         row_widget = widget;
         break;
      }
   }

   if (!row_widget) return NULL;
   if (!item)
      item = find_list_item_in (row_widget);
   if (!item) return NULL;
   if (gtk_list_item_get_position (item) == GTK_INVALID_LIST_POSITION)
      return NULL;

   if (item_ret) *item_ret = item;

   return row_widget;
}


gboolean
gimv_list_view_get_row_at (GtkWidget *view, gdouble x, gdouble y,
                           guint *position, GimvRow **row)
{
   GtkListItem *item = NULL;

   g_return_val_if_fail (GTK_IS_WIDGET (view), FALSE);

   if (!row_widget_at (view, x, y, &item)) return FALSE;

   if (position)
      *position = gtk_list_item_get_position (item);
   if (row)
      *row = gimv_row_from_item (gtk_list_item_get_item (item));

   return TRUE;
}


static GtkWidget *
find_row_widget_for_position (GtkWidget *widget, guint position)
{
   GtkWidget *child, *found;

   if (is_row_widget (widget)) {
      GtkListItem *item = find_list_item_in (widget);
      if (item && gtk_list_item_get_position (item) == position)
         return widget;
      return NULL;
   }

   for (child = gtk_widget_get_first_child (widget);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      found = find_row_widget_for_position (child, position);
      if (found) return found;
   }

   return NULL;
}


gboolean
gimv_list_view_get_row_area (GtkWidget *view, guint position,
                             graphene_rect_t *area)
{
   GtkWidget *row_widget;

   g_return_val_if_fail (GTK_IS_WIDGET (view), FALSE);
   g_return_val_if_fail (area, FALSE);

   row_widget = find_row_widget_for_position (view, position);
   if (!row_widget || !gtk_widget_get_mapped (row_widget)) return FALSE;

   return gtk_widget_compute_bounds (row_widget, view, area);
}


#define DROP_HIGHLIGHT_CLASS "gimv-drop-highlight"
#define DROP_HIGHLIGHT_KEY   "gimv-drop-highlight-row"

static void
ensure_drop_highlight_css (void)
{
   static gboolean done = FALSE;
   GtkCssProvider *provider;
   GdkDisplay *display = gdk_display_get_default ();

   if (done || !display) return;
   done = TRUE;

   provider = gtk_css_provider_new ();
   gtk_css_provider_load_from_string (provider,
      "row." DROP_HIGHLIGHT_CLASS " { box-shadow: inset 0 0 0 1px currentColor; }");
   gtk_style_context_add_provider_for_display (
      display, GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
   g_object_unref (provider);
}


static void
drop_highlight_clear (GtkWidget *view)
{
   GWeakRef *ref = g_object_get_data (G_OBJECT (view), DROP_HIGHLIGHT_KEY);
   GtkWidget *row_widget;

   if (!ref) return;

   row_widget = g_weak_ref_get (ref);
   if (row_widget) {
      gtk_widget_remove_css_class (row_widget, DROP_HIGHLIGHT_CLASS);
      g_object_unref (row_widget);
   }
   g_weak_ref_set (ref, NULL);
}


static void
weak_ref_free (gpointer data)
{
   g_weak_ref_clear (data);
   g_free (data);
}


void
gimv_list_view_set_drop_highlight (GtkWidget *view, gdouble x, gdouble y)
{
   GWeakRef *ref;
   GtkWidget *row_widget;

   g_return_if_fail (GTK_IS_WIDGET (view));

   drop_highlight_clear (view);
   if (x < 0 || y < 0) return;

   row_widget = row_widget_at (view, x, y, NULL);
   if (!row_widget) return;

   ensure_drop_highlight_css ();
   gtk_widget_add_css_class (row_widget, DROP_HIGHLIGHT_CLASS);

   ref = g_object_get_data (G_OBJECT (view), DROP_HIGHLIGHT_KEY);
   if (!ref) {
      ref = g_new0 (GWeakRef, 1);
      g_weak_ref_init (ref, NULL);
      g_object_set_data_full (G_OBJECT (view), DROP_HIGHLIGHT_KEY,
                              ref, weak_ref_free);
   }
   g_weak_ref_set (ref, row_widget);
}


void
gimv_list_view_scroll_to (GtkWidget *view, guint position,
                          gboolean select, gboolean focus)
{
   GtkListScrollFlags flags = GTK_LIST_SCROLL_NONE;

   g_return_if_fail (GTK_IS_WIDGET (view));

   if (select) flags |= GTK_LIST_SCROLL_SELECT;
   if (focus)  flags |= GTK_LIST_SCROLL_FOCUS;

   if (GTK_IS_COLUMN_VIEW (view))
      gtk_column_view_scroll_to (GTK_COLUMN_VIEW (view), position, NULL,
                                 flags, NULL);
   else if (GTK_IS_LIST_VIEW (view))
      gtk_list_view_scroll_to (GTK_LIST_VIEW (view), position, flags, NULL);
}


guint
gimv_list_view_get_selected (GtkWidget *view)
{
   GtkSelectionModel *selection = gimv_list_view_get_selection (view);
   GtkBitset *set;
   guint position = GTK_INVALID_LIST_POSITION;

   if (!selection) return GTK_INVALID_LIST_POSITION;

   if (GTK_IS_SINGLE_SELECTION (selection))
      return gtk_single_selection_get_selected (GTK_SINGLE_SELECTION (selection));

   set = gtk_selection_model_get_selection (selection);
   if (!gtk_bitset_is_empty (set))
      position = gtk_bitset_get_minimum (set);
   gtk_bitset_unref (set);

   return position;
}


GimvRow *
gimv_list_view_get_selected_row (GtkWidget *view)
{
   guint position = gimv_list_view_get_selected (view);
   GListModel *model;

   if (position == GTK_INVALID_LIST_POSITION) return NULL;

   model = G_LIST_MODEL (gimv_list_view_get_selection (view));
   return gimv_row_store_peek (model, position);
}


void
gimv_list_view_select (GtkWidget *view, guint position)
{
   GtkSelectionModel *selection = gimv_list_view_get_selection (view);

   g_return_if_fail (selection);

   if (position == GTK_INVALID_LIST_POSITION)
      gtk_selection_model_unselect_all (selection);
   else
      gtk_selection_model_select_item (selection, position, TRUE);
}


void
gimv_list_view_unselect_all (GtkWidget *view)
{
   GtkSelectionModel *selection = gimv_list_view_get_selection (view);

   g_return_if_fail (selection);
   gtk_selection_model_unselect_all (selection);
}
