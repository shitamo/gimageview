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

/*
 *  Helpers for the list widgets of GTK 4 (GtkListView, GtkColumnView).
 *
 *  GtkTreeView, GtkTreeModel, GtkListStore, GtkTreeStore and the cell
 *  renderers are deprecated since GTK 4.10.  The replacement is a GListModel
 *  of items shown by list item factories.  GImageView's lists hold rows of
 *  a few typed values, so the items are GimvRow objects which keep their
 *  values the way GtkListStore did (and an optional list of child rows for
 *  trees, used with GtkTreeListModel).  The cell helpers below bind the
 *  values of a row to the widgets of a list item and update them when the
 *  row changes.
 */

#ifndef __GIMV_LIST_VIEW_H__
#define __GIMV_LIST_VIEW_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

/******************************************************************************
 *  GimvRow
 ******************************************************************************/
#define GIMV_TYPE_ROW (gimv_row_get_type ())
G_DECLARE_FINAL_TYPE (GimvRow, gimv_row, GIMV, ROW, GObject)

/* the types of the columns follow n_columns (like gtk_list_store_new ()) */
GimvRow      *gimv_row_new                (gint          n_columns,
                                           ...);
GimvRow      *gimv_row_newv               (gint          n_columns,
                                           const GType  *types);
gint          gimv_row_get_n_columns      (GimvRow      *row);
/* column, value, ..., -1 (like gtk_list_store_set ()); emits "changed" */
void          gimv_row_set                (GimvRow      *row,
                                           ...);
void          gimv_row_set_value          (GimvRow      *row,
                                           gint          column,
                                           const GValue *value);
/* column, &location, ..., -1 (like gtk_tree_model_get (): strings are
   copies, objects are referenced, boxed values are copies) */
void          gimv_row_get                (GimvRow      *row,
                                           ...);
const GValue *gimv_row_peek_value         (GimvRow      *row,
                                           gint          column);
const gchar  *gimv_row_peek_string        (GimvRow      *row,
                                           gint          column);
gpointer      gimv_row_peek_pointer       (GimvRow      *row,
                                           gint          column);
gpointer      gimv_row_peek_object        (GimvRow      *row,
                                           gint          column);
gint          gimv_row_peek_int           (GimvRow      *row,
                                           gint          column);
gboolean      gimv_row_peek_boolean       (GimvRow      *row,
                                           gint          column);
/* notify the cells showing the row (gimv_row_set () does it) */
void          gimv_row_changed            (GimvRow      *row);

/* child rows (trees).  With create, the (empty) list is created when the
   row has none, which makes the row expandable. */
GListStore   *gimv_row_get_children       (GimvRow      *row,
                                           gboolean      create);
void          gimv_row_remove_children    (GimvRow      *row);
GimvRow      *gimv_row_get_parent         (GimvRow      *row);
/* appends child (the store keeps a reference) */
void          gimv_row_append_child       (GimvRow      *parent,
                                           GimvRow      *child);
void          gimv_row_insert_child       (GimvRow      *parent,
                                           guint         position,
                                           GimvRow      *child);
/* TRUE if row is a descendant of ancestor */
gboolean      gimv_row_is_ancestor        (GimvRow      *ancestor,
                                           GimvRow      *row);

/* a GListStore of GimvRow */
GListStore   *gimv_row_store_new          (void);
/* the row at position (not referenced), NULL if out of range.  store may
   also be a GtkTreeListModel or a selection model of GimvRow items. */
GimvRow      *gimv_row_store_peek         (GListModel   *store,
                                           guint         position);
gboolean      gimv_row_store_find         (GListModel   *store,
                                           GimvRow      *row,
                                           guint        *position);

/* the GimvRow of an item of a list model: either a GimvRow, or a
   GtkTreeListRow of a GtkTreeListModel made with gimv_tree_list_model_new () */
GimvRow      *gimv_row_from_item          (gpointer      item);

/* a GtkTreeListModel over root (a GListModel of GimvRow) whose children are
   the children lists of the rows */
GtkTreeListModel *gimv_tree_list_model_new (GListModel  *root);
/* the GtkTreeListRow of row (NULL if one of its ancestors is collapsed) */
GtkTreeListRow   *gimv_tree_list_model_get_row_for (GtkTreeListModel *model,
                                                    GimvRow          *row,
                                                    guint            *position);
/* expand row and its ancestors */
void              gimv_tree_list_model_expand_to (GtkTreeListModel *model,
                                                  GimvRow          *row);

/******************************************************************************
 *  list item factories
 ******************************************************************************/
/* the GtkTreeListRow bound to a list item (NULL for flat lists) */
GtkTreeListRow     *gimv_list_item_get_tree_row  (GtkListItem      *item);

/* creates the widget of a cell */
typedef GtkWidget *(*GimvCellSetupFunc) (gpointer          data);
/* sets the widget of a cell from the row; item is a GtkListItem (or a
   GtkColumnViewCell, which is a subclass of it).  It is called again when
   the row changes and, in trees, when it is expanded or collapsed. */
typedef void       (*GimvCellBindFunc)  (GtkWidget        *cell,
                                         GimvRow          *row,
                                         GtkListItem      *item,
                                         gpointer          data);

/* with expander, the cell is shown in a GtkTreeExpander (the model is a
   GtkTreeListModel) */
GtkListItemFactory *gimv_cell_factory_new        (GimvCellSetupFunc setup,
                                                  GimvCellBindFunc  bind,
                                                  gpointer          data,
                                                  GDestroyNotify    destroy,
                                                  gboolean          expander);
/* a label showing the text of column text_col (any type transformable to
   a string) */
GtkListItemFactory *gimv_text_factory_new        (gint              text_col,
                                                  gboolean          expander);
/* an icon (GdkPaintable, GdkPixbuf or icon name in icon_col) followed by
   the text of text_col (-1 for none) */
GtkListItemFactory *gimv_icon_text_factory_new   (gint              icon_col,
                                                  gint              text_col,
                                                  gboolean          expander);

/* the widget made by the setup function of a cell (inside the expander) */
GtkWidget          *gimv_cell_get_content        (GtkWidget        *cell);
/* the parts of an icon & text cell */
GtkWidget          *gimv_icon_text_cell_get_image (GtkWidget       *cell);
GtkWidget          *gimv_icon_text_cell_get_label (GtkWidget       *cell);
void                gimv_icon_text_cell_set_icon  (GtkWidget       *cell,
                                                   const GValue    *value);

/******************************************************************************
 *  views
 ******************************************************************************/
/* a selection model over model (which it takes over) behaving like the
   GtkTreeSelection of mode: nothing is selected initially and a single
   selection can be empty */
GtkSelectionModel   *gimv_selection_model_new      (GListModel         *model,
                                                    GtkSelectionMode    mode);
/* views taking over model and factory */
GtkWidget           *gimv_list_view_new            (GListModel         *model,
                                                    GtkSelectionMode    mode,
                                                    GtkListItemFactory *factory);
GtkWidget           *gimv_column_view_new          (GListModel         *model,
                                                    GtkSelectionMode    mode);

/******************************************************************************
 *  column view
 ******************************************************************************/
GtkColumnViewColumn *gimv_column_view_append       (GtkColumnView      *view,
                                                    const gchar        *title,
                                                    GtkListItemFactory *factory);
GtkColumnViewColumn *gimv_column_view_append_text  (GtkColumnView      *view,
                                                    const gchar        *title,
                                                    gint                text_col);
GtkColumnViewColumn *gimv_column_view_get_column   (GtkColumnView      *view,
                                                    guint               index);
guint                gimv_column_view_get_n_columns (GtkColumnView     *view);
void                 gimv_column_view_set_headers_visible (GtkColumnView *view,
                                                    gboolean            visible);
gboolean             gimv_column_view_get_headers_visible (GtkColumnView *view);

/******************************************************************************
 *  GtkListView / GtkColumnView utilities
 ******************************************************************************/
/* the model of the selection model of a GtkListView or GtkColumnView */
GListModel         *gimv_list_view_get_model          (GtkWidget  *view);
GtkSelectionModel  *gimv_list_view_get_selection      (GtkWidget  *view);
/* position and item (a GimvRow, not referenced) of the row at the widget
   coordinates x, y.  FALSE if there is no row. */
gboolean            gimv_list_view_get_row_at         (GtkWidget  *view,
                                                       gdouble     x,
                                                       gdouble     y,
                                                       guint      *position,
                                                       GimvRow   **row);
/* the bounds of the row widget at position in widget coordinates (FALSE if
   the row is not realized, i.e. not near the visible area) */
gboolean            gimv_list_view_get_row_area       (GtkWidget  *view,
                                                       guint       position,
                                                       graphene_rect_t *area);
/* highlight the row at x, y as drop target (x < 0 : remove the highlight) */
void                gimv_list_view_set_drop_highlight (GtkWidget  *view,
                                                       gdouble     x,
                                                       gdouble     y);
void                gimv_list_view_scroll_to          (GtkWidget  *view,
                                                       guint       position,
                                                       gboolean    select,
                                                       gboolean    focus);
/* the selected position of a single selection (GTK_INVALID_LIST_POSITION if
   none) */
guint               gimv_list_view_get_selected       (GtkWidget  *view);
/* the selected row (not referenced) */
GimvRow            *gimv_list_view_get_selected_row   (GtkWidget  *view);
void                gimv_list_view_select             (GtkWidget  *view,
                                                       guint       position);
void                gimv_list_view_unselect_all       (GtkWidget  *view);

G_END_DECLS

#endif /* __GIMV_LIST_VIEW_H__ */
