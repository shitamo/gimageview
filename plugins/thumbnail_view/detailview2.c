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
 *
 * $Id: detailview2.c,v 1.13 2004/09/21 08:44:29 makeinu Exp $
 */

#include "detailview.h"

/*
 *   For Gtk+-2.0
 */


#include <time.h>
#include <string.h>

#include "charset.h"
#include "dnd.h"
#include "detailview_prefs.h"
#include "detailview_priv.h"
#include "fileutil.h"
#include "gimv_list_view.h"
#include "gimv_image.h"
#include "gimv_thumb.h"
#include "gimv_thumb_win.h"
#include "prefs.h"


enum {
   COLUMN_TERMINATOR = -1,
   COLUMN_THUMB_DATA,
   COLUMN_PIXMAP,
   N_COLUMN
};


typedef struct DetailViewData_Tag
{
   GtkWidget *treeview;      /* GtkColumnView */
   GListStore *store;        /* rows (GimvRow) */
   gboolean   dragging;
   gint       press_button;
   gint       press_mask;
   gint       press_x, press_y;
   gboolean   set_selection;
   /* GTK4: the list widgets change the selection when the button is
      released, after cb_treeview_button_release () (capture phase) */
   gboolean   releasing;
   guint      releasing_id;
} DetailViewData;


/* callback functions */
static gboolean cb_treeview_button_press       (GtkWidget         *widget,
                                                GimvEventButton    *event,
                                                GimvThumbView     *tv);
static gboolean cb_treeview_motion_notify      (GtkWidget         *widget,
                                                GimvEventMotion    *event,
                                                gpointer           data);
static gboolean cb_treeview_button_release     (GtkWidget         *widget,
                                                GimvEventButton    *event,
                                                GimvThumbView     *tv);
static void     cb_treeview_drag_data_received (GtkWidget         *widget,
                                                GimvDragContext    *context,
                                                gint               x,
                                                gint               y,
                                                GimvSelectionData  *seldata,
                                                guint              info,
                                                guint              time,
                                                gpointer           data);
static void     cb_sorter_changed              (GtkSorter         *sorter,
                                                GtkSorterChange    change,
                                                GimvThumbView     *tv);

/* other private function */
static DetailViewData *detailview_new          (GimvThumbView  *tv);
static void            set_column_types        (GimvThumbView  *tv,
                                                DetailViewData *tv_data,
                                                const gchar    *dest_mode);
static void            detailview_set_pixmaps  (GimvThumbView  *tv,
                                                const gchar    *dest_mode);



/* the thumbnail of the row at widget coordinates x, y (NULL if none) */
static GimvThumb *
thumb_at_widget_pos (DetailViewData *tv_data, gdouble x, gdouble y,
                     guint *position)
{
   GimvRow *row = NULL;

   if (!gimv_list_view_get_row_at (tv_data->treeview, x, y, position, &row)
       || !row)
   {
      return NULL;
   }

   return gimv_row_peek_pointer (row, COLUMN_THUMB_DATA);
}


static GimvThumb *
thumb_at_position (DetailViewData *tv_data, guint position)
{
   GimvRow *row = gimv_row_store_peek (G_LIST_MODEL (tv_data->store), position);
   return row ? gimv_row_peek_pointer (row, COLUMN_THUMB_DATA) : NULL;
}



/******************************************************************************
 *
 *   selection model
 *
 *   GTK4: GtkTreeView and its select function are deprecated.  The thumbnail
 *   view keeps the selection in GimvThumb::selected, so the selection model
 *   of the column view reads it from there, and it accepts the changes
 *   requested by the view (clicks, keyboard) under the conditions the select
 *   function of the GtkTreeView based version used.
 *
 ******************************************************************************/
#define DETAIL_TYPE_SELECTION (detail_selection_get_type ())
G_DECLARE_FINAL_TYPE (DetailSelection, detail_selection,
                      DETAIL, SELECTION, GObject)

struct _DetailSelection
{
   GObject        parent;
   GListModel    *model;   /* of GimvRow */
   GimvThumbView *tv;      /* weak */
};

static void detail_selection_list_model_init      (GListModelInterface        *iface);
static void detail_selection_selection_model_init (GtkSelectionModelInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (DetailSelection, detail_selection, G_TYPE_OBJECT,
   G_IMPLEMENT_INTERFACE (G_TYPE_LIST_MODEL,
                          detail_selection_list_model_init)
   G_IMPLEMENT_INTERFACE (GTK_TYPE_SELECTION_MODEL,
                          detail_selection_selection_model_init))


static GType
detail_selection_get_item_type (GListModel *list)
{
   return GIMV_TYPE_ROW;
}


static guint
detail_selection_get_n_items (GListModel *list)
{
   DetailSelection *self = DETAIL_SELECTION (list);
   return self->model ? g_list_model_get_n_items (self->model) : 0;
}


static gpointer
detail_selection_get_item (GListModel *list, guint position)
{
   DetailSelection *self = DETAIL_SELECTION (list);
   return self->model ? g_list_model_get_item (self->model, position) : NULL;
}


static void
detail_selection_list_model_init (GListModelInterface *iface)
{
   iface->get_item_type = detail_selection_get_item_type;
   iface->get_n_items   = detail_selection_get_n_items;
   iface->get_item      = detail_selection_get_item;
}


static gboolean
detail_selection_is_selected (GtkSelectionModel *model, guint position)
{
   DetailSelection *self = DETAIL_SELECTION (model);
   GimvRow *row;
   GimvThumb *thumb;

   if (!self->model) return FALSE;

   row = gimv_row_store_peek (self->model, position);
   thumb = row ? gimv_row_peek_pointer (row, COLUMN_THUMB_DATA) : NULL;

   return thumb ? thumb->selected : FALSE;
}


/* the conditions of the select function of the GtkTreeView based version */
static gboolean
detail_selection_change_allowed (DetailSelection *self)
{
   DetailViewData *tv_data;

   if (!self->tv) return FALSE;

   tv_data = g_object_get_data (G_OBJECT (self->tv), DETAIL_VIEW_LABEL);
   if (!tv_data) return FALSE;

   /* only by a mouse button (keyboard navigation moves the focus only) */
   if (tv_data->press_button <= 0 && !tv_data->set_selection
       && !tv_data->releasing)
   {
      return FALSE;
   }

   /*
    *  For DnD and popup, should not change selection when selected path was
    *  pressed.
    *  Selection should be changed when mouse button was released.
    */
   if (tv_data->press_button > 0
       && !(tv_data->press_mask & GDK_CONTROL_MASK)
       && !(tv_data->press_mask & GDK_SHIFT_MASK)
       && !tv_data->dragging
       && tv_data->treeview
       && gtk_widget_get_mapped (tv_data->treeview))
   {
      GimvThumb *thumb = thumb_at_widget_pos (tv_data,
                                              tv_data->press_x,
                                              tv_data->press_y, NULL);
      if (thumb && thumb->selected)
         return FALSE;
   }

   return TRUE;
}


static gboolean
detail_selection_set_selection (GtkSelectionModel *model,
                                GtkBitset *selected, GtkBitset *mask)
{
   DetailSelection *self = DETAIL_SELECTION (model);
   DetailViewData *tv_data;
   GtkBitsetIter iter;
   guint position, min = G_MAXUINT, max = 0;
   gboolean valid;

   if (!self->model) return FALSE;
   if (!detail_selection_change_allowed (self)) return FALSE;

   for (valid = gtk_bitset_iter_init_first (&iter, mask, &position);
        valid;
        valid = gtk_bitset_iter_next (&iter, &position))
   {
      GimvRow *row = gimv_row_store_peek (self->model, position);
      GimvThumb *thumb = row ? gimv_row_peek_pointer (row, COLUMN_THUMB_DATA) : NULL;
      gboolean select = gtk_bitset_contains (selected, position);

      if (!thumb || thumb->selected == select) continue;

      thumb->selected = select;
      min = MIN (min, position);
      max = MAX (max, position);
   }

   tv_data = g_object_get_data (G_OBJECT (self->tv), DETAIL_VIEW_LABEL);
   if (tv_data)
      tv_data->set_selection = FALSE;

   if (min <= max)
      gtk_selection_model_selection_changed (model, min, max - min + 1);

   return TRUE;
}


static void
detail_selection_selection_model_init (GtkSelectionModelInterface *iface)
{
   iface->is_selected   = detail_selection_is_selected;
   iface->set_selection = detail_selection_set_selection;
}


static void
cb_detail_selection_items_changed (GListModel *model, guint position,
                                   guint removed, guint added,
                                   DetailSelection *self)
{
   g_list_model_items_changed (G_LIST_MODEL (self), position, removed, added);
}


static void
detail_selection_dispose (GObject *object)
{
   DetailSelection *self = DETAIL_SELECTION (object);

   if (self->model) {
      g_signal_handlers_disconnect_by_func (self->model,
                                            cb_detail_selection_items_changed,
                                            self);
      g_clear_object (&self->model);
   }
   if (self->tv) {
      g_object_remove_weak_pointer (G_OBJECT (self->tv), (gpointer *) &self->tv);
      self->tv = NULL;
   }

   G_OBJECT_CLASS (detail_selection_parent_class)->dispose (object);
}


static void
detail_selection_class_init (DetailSelectionClass *klass)
{
   G_OBJECT_CLASS (klass)->dispose = detail_selection_dispose;
}


static void
detail_selection_init (DetailSelection *self)
{
}


static GtkSelectionModel *
detail_selection_new (GListModel *model, GimvThumbView *tv)
{
   DetailSelection *self = g_object_new (DETAIL_TYPE_SELECTION, NULL);

   self->model = g_object_ref (model);
   g_signal_connect (model, "items-changed",
                     G_CALLBACK (cb_detail_selection_items_changed), self);
   self->tv = tv;
   g_object_add_weak_pointer (G_OBJECT (tv), (gpointer *) &self->tv);

   return GTK_SELECTION_MODEL (self);
}



/******************************************************************************
 *
 *   Callback functions.
 *
 ******************************************************************************/
static gint
cb_treeview_key_press (GtkWidget *widget,
                       GimvEventKey *event,
                       GimvThumbView *tv)
{
   DetailViewData *tv_data;
   GimvThumb *thumb = NULL;
   guint position;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data, FALSE);

   position = gimv_list_view_get_focus_position (tv_data->treeview);
   if (position != GTK_INVALID_LIST_POSITION)
      thumb = thumb_at_position (tv_data, position);

   if (gimv_thumb_view_thumb_key_press_cb(widget, event, thumb))
      return FALSE;

   {
      switch (event->keyval) {
      case GDK_KEY_Left:
      case GDK_KEY_Right:
      case GDK_KEY_Up:
      case GDK_KEY_Down:
         return FALSE;
         break;
      case GDK_KEY_Return:
         if (!thumb) break;
         if (event->state & GDK_SHIFT_MASK || event->state & GDK_CONTROL_MASK) {
            /* is there somteing to do? */
         } else {
            gimv_thumb_view_set_selection_all (tv, FALSE);
         }
         gimv_thumb_view_set_selection (thumb, TRUE);
         gimv_thumb_view_open_image (tv, thumb, 0);
         break;
      case GDK_KEY_space:
         if (!thumb) break;
         gimv_thumb_view_set_selection (thumb, !thumb->selected);
         break;
      case GDK_KEY_Delete:
         gimv_thumb_view_delete_files (tv);
         break;
      default:
         break;
      }
   }

   return FALSE;
}


static gboolean
cb_treeview_button_press (GtkWidget *widget,
                          GimvEventButton *event,
                          GimvThumbView *tv)
{
   DetailViewData *tv_data;
   GimvThumb *thumb = NULL;
   gboolean retval2 = FALSE;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data, FALSE);

   tv_data->dragging     = FALSE;
   tv_data->press_button = event->button;
   tv_data->press_mask   = event->state;
   tv_data->press_x      = event->x;
   tv_data->press_y      = event->y;

   /* get state of row under mouse cursor */
   thumb = thumb_at_widget_pos (tv_data, event->x, event->y, NULL);

   if (thumb) {
      retval2 = gimv_thumb_view_thumb_button_press_cb (widget, event, thumb);
   }
   /*
   if (retval1) {
      return TRUE;
   } else {
      return retval2;
   }
   */
   return retval2;
}


static gboolean
cb_treeview_motion_notify (GtkWidget *widget,
                           GimvEventMotion *event,
                           gpointer data)
{
   DetailViewData *tv_data;
   GimvThumbView *tv = data;
   GimvThumb *thumb = NULL;
   gint dx, dy;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data, FALSE);

   if (tv_data->dragging) return FALSE;

   dx = tv_data->press_x - event->x;
   dy = tv_data->press_y - event->y;
   if (abs (dx) > 2 || abs (dy) > 2)
      tv_data->dragging = TRUE;

   thumb = thumb_at_widget_pos (tv_data, event->x, event->y, NULL);

   return gimv_thumb_view_motion_notify_cb (widget, event, thumb);
}


static gboolean
idle_releasing_done (gpointer data)
{
   DetailViewData *tv_data = data;

   tv_data->releasing    = FALSE;
   tv_data->releasing_id = 0;

   return G_SOURCE_REMOVE;
}


static gboolean
cb_treeview_button_release (GtkWidget *widget,
                            GimvEventButton *event,
                            GimvThumbView *tv)
{
   DetailViewData *tv_data;
   GimvThumb *thumb = NULL;
   gboolean retval2 = FALSE;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data, FALSE);

   tv_data->press_button = -1;
   tv_data->press_mask   = -1;
   tv_data->press_x      = -1;
   tv_data->press_y      = -1;

   /* the selection may be changed by the view while this release event is
      handled (see detail_selection_change_allowed ()) */
   if (!tv_data->dragging && event->type == GDK_BUTTON_RELEASE) {
      tv_data->releasing = TRUE;
      if (!tv_data->releasing_id)
         tv_data->releasing_id = g_idle_add_full (G_PRIORITY_HIGH,
                                                  idle_releasing_done,
                                                  tv_data, NULL);
   }

   if (tv_data->dragging) {
      tv_data->dragging = FALSE;
      return FALSE;
   }

   /* get state of row under mouse cursor */
   thumb = thumb_at_widget_pos (tv_data, event->x, event->y, NULL);
   if (thumb) {
      if (event->type == GDK_BUTTON_RELEASE
          && (event->button == 1)
          && !(event->state & GDK_SHIFT_MASK)
          && !(event->state & GDK_CONTROL_MASK)
          && !tv_data->dragging)
      {
         gimv_thumb_view_set_selection_all (tv, FALSE);
         gimv_thumb_view_set_selection (thumb, TRUE);
      }
   }

   if (thumb) {
      retval2 = gimv_thumb_view_thumb_button_release_cb (widget, event, thumb);
   }
   /*
   if (retval1) {
      return TRUE;
   } else {
      return retval2;
   }
   */
   return retval2;
}


static gboolean
cb_treeview_scroll (GtkWidget *widget, GimvEventScroll *se, GimvThumbView *tv)
{
   GimvEventButton be;
   gboolean retval = FALSE;

   g_return_val_if_fail (GTK_IS_WIDGET(widget), FALSE);

   memset (&be, 0, sizeof (be));
   be.type       = GDK_BUTTON_PRESS;
   be.time       = se->time;
   be.x          = se->x;
   be.y          = se->y;
   be.state      = se->state;
   be.x_root     = se->x;   /* GTK4: no root coordinates */
   be.y_root     = se->y;
   be.event      = se->event;
   be.controller = se->controller;
   switch ((se)->direction) {
   case GDK_SCROLL_UP:
      be.button = 4;
      break;
   case GDK_SCROLL_DOWN:
      be.button = 5;
      break;
   case GDK_SCROLL_LEFT:
      be.button = 6;
      break;
   case GDK_SCROLL_RIGHT:
      be.button = 7;
      break;
   default:
      g_warning ("invalid scroll direction!");
      be.button = 0;
      break;
   }

   retval = cb_treeview_button_press (widget, &be, tv);

   be.type = GDK_BUTTON_RELEASE;
   retval = cb_treeview_button_release (widget, &be, tv);

   return retval;
}


static void
cb_treeview_drag_data_received (GtkWidget *widget,
                                GimvDragContext *context,
                                gint x, gint y,
                                GimvSelectionData *seldata,
                                guint info,
                                guint time,
                                gpointer data)
{
   gimv_thumb_view_drag_data_received_cb (widget, context, x, y,
                                          seldata, info, time, data);

   /* GTK4: GtkTreeView has no default "drag_data_received" handler any
      more, so there is no need to stop the emission */
}


/* GTK4: GtkColumnView has no "clicked" signal for the columns.  The
   columns get a sorter, which makes their titles clickable; the view sorter
   changes when a title is clicked.  The rows are sorted by the thumbnail
   window like before (the sorters themselves do not sort anything). */
static gint
dummy_sorter_compare (gconstpointer a, gconstpointer b, gpointer data)
{
   return 0;
}


static void
cb_sorter_changed (GtkSorter *sorter, GtkSorterChange change, GimvThumbView *tv)
{
   GtkColumnViewColumn *column;
   GimvThumbWin *tw;
   DetailViewData *tv_data;
   GList *node;
   gint col, idx;
   GimvSortItem sort_item, current_item;
   GimvSortFlag sort_flag = 0, current_flag;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   if (tv->progress) return;

   tw = tv->tw;
   g_return_if_fail (GIMV_IS_THUMB_WIN (tw));

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data && tv_data->treeview);

   column = gtk_column_view_sorter_get_primary_sort_column (GTK_COLUMN_VIEW_SORTER (sorter));
   if (!column) return;
   {
      GListModel *columns = gtk_column_view_get_columns (GTK_COLUMN_VIEW (tv_data->treeview));
      guint i, n = g_list_model_get_n_items (columns);

      col = -1;
      for (i = 0; i < n; i++) {
         GtkColumnViewColumn *c = g_list_model_get_item (columns, i);
         g_object_unref (c);
         if (c == column) {
            col = i;
            break;
         }
      }
      if (col < 0) return;
   }

   node = g_list_nth (detailview_title_idx_list, col - 1);
   if (!node) return;

   idx = GPOINTER_TO_INT (node->data);

   switch (idx) {
   case 1:
      sort_item = GIMV_SORT_NAME;
      break;
   case 2:
      sort_item = GIMV_SORT_SIZE;
      break;
   case 3:
      sort_item = GIMV_SORT_TYPE;
      break;
   case 5:
      sort_item = GIMV_SORT_ATIME;
      break;
   case 6:
      sort_item = GIMV_SORT_MTIME;
      break;
   case 7:
      sort_item = GIMV_SORT_CTIME;
      break;
   default:
      return;
   }

   current_item = gimv_thumb_win_get_sort_type (tw, &current_flag);

   if (current_item == sort_item) {
      if (current_flag & GIMV_SORT_REVERSE)
         sort_flag &= ~GIMV_SORT_REVERSE;
      else
         sort_flag |= GIMV_SORT_REVERSE;
   }

   gimv_thumb_win_sort_thumbnail (tw, sort_item, sort_flag,
                                  GIMV_THUMB_WIN_CURRENT_PAGE);
}


/******************************************************************************
 *
 *   private functions.
 *
 ******************************************************************************/
static void
detailview_data_free (DetailViewData *tv_data)
{
   if (tv_data->releasing_id)
      g_source_remove (tv_data->releasing_id);
   if (tv_data->store)
      g_object_unref (tv_data->store);
   g_free (tv_data);
}


static DetailViewData *
detailview_new (GimvThumbView *tv)
{
   DetailViewData *tv_data;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), NULL);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   if (!tv_data) {
      tv_data = g_new0 (DetailViewData, 1);
      tv_data->treeview      = NULL;
      tv_data->dragging      = FALSE;
      tv_data->press_button  = -1;
      tv_data->press_mask    = -1;
      tv_data->press_x       = -1;
      tv_data->press_y       = -1;
      tv_data->set_selection = FALSE;
      g_object_set_data_full (G_OBJECT (tv), DETAIL_VIEW_LABEL, tv_data,
                              (GDestroyNotify) detailview_data_free);
   }

   return tv_data;
}


/* the thumbnail (or icon) column */
static GtkWidget *
pixmap_cell_setup (gpointer data)
{
   GtkWidget *picture = gtk_picture_new ();
   gint size = GPOINTER_TO_INT (data);

   gtk_picture_set_can_shrink (GTK_PICTURE (picture), FALSE);
   gtk_widget_set_halign (picture, GTK_ALIGN_CENTER);
   gtk_widget_set_valign (picture, GTK_ALIGN_CENTER);
   if (size > 0)
      gtk_widget_set_size_request (picture, size, size);

   return picture;
}


static void
pixmap_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   GdkTexture *texture = gimv_row_peek_object (row, COLUMN_PIXMAP);
   gtk_picture_set_paintable (GTK_PICTURE (cell), GDK_PAINTABLE (texture));
}


/* text columns */
typedef struct {
   gint   column;
   gfloat xalign;
} TextCell;


static GtkWidget *
text_cell_setup (gpointer data)
{
   TextCell *tc = data;
   GtkWidget *label = gtk_label_new (NULL);

   gtk_label_set_xalign (GTK_LABEL (label), tc->xalign);
   gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
   gtk_label_set_single_line_mode (GTK_LABEL (label), TRUE);
   gtk_widget_set_hexpand (label, TRUE);

   return label;
}


static void
text_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   TextCell *tc = data;
   const gchar *text = gimv_row_peek_string (row, tc->column);

   gtk_label_set_text (GTK_LABEL (cell), text ? text : "");
}


static void
set_column_types (GimvThumbView *tv, DetailViewData *tv_data,
                  const gchar *dest_mode)
{
   GtkColumnView *view = GTK_COLUMN_VIEW (tv_data->treeview);
   GtkColumnViewColumn *col;
   GList *node;
   gint i, size = 0;
   /* the padding of the GtkCellRenderer used for the pixmap */
   const gint pad = 2;

   /* pixmap column */
   if (!strcmp (DETAIL_ICON_LABEL, dest_mode))
      size = ICON_SIZE + pad * 2;
   if (!strcmp (DETAIL_THUMB_LABEL, dest_mode))
      size = tv->thumb_size + pad * 2;

   col = gimv_column_view_append (view, "",
                                  gimv_cell_factory_new (pixmap_cell_setup,
                                                         pixmap_cell_bind,
                                                         GINT_TO_POINTER (size),
                                                         NULL, FALSE));
   /* "Detail" mode: no pixmaps */
   if (size <= 0)
      gtk_column_view_column_set_fixed_width (col, 1);

   /* other column */
   for (node = detailview_title_idx_list, i = N_COLUMN;
        node;
        node = g_list_next (node), i++)
   {
      gint idx = GPOINTER_TO_INT (node->data);
      TextCell *tc = g_new0 (TextCell, 1);
      GtkSorter *sorter;

      tc->column = i;
      switch (detailview_columns[idx].justification) {
      case GTK_JUSTIFY_CENTER:
         tc->xalign = 0.5;
         break;
      case GTK_JUSTIFY_RIGHT:
         tc->xalign = 1.0;
         break;
      case GTK_JUSTIFY_LEFT:
      default:
         tc->xalign = 0.0;
         break;
      }

      col = gimv_column_view_append (view, _(detailview_columns[idx].title),
                                     gimv_cell_factory_new (text_cell_setup,
                                                            text_cell_bind,
                                                            tc, g_free,
                                                            FALSE));
      gtk_column_view_column_set_fixed_width (col, detailview_columns[idx].width);
      gtk_column_view_column_set_resizable (col, TRUE);

      /* clicking the title sorts the thumbnails (see cb_sorter_changed ()) */
      sorter = GTK_SORTER (gtk_custom_sorter_new (dummy_sorter_compare,
                                                  NULL, NULL));
      gtk_column_view_column_set_sorter (col, sorter);
      g_object_unref (sorter);
   }

   g_signal_connect (gtk_column_view_get_sorter (view), "changed",
                     G_CALLBACK (cb_sorter_changed), tv);
}


static void
detailview_set_pixmaps (GimvThumbView *tv, const gchar *dest_mode)
{
   GimvThumb *thumb;
   GList *node;
   gint i, num;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   node = g_list_first (tv->thumblist);
   num = g_list_length (node);

   /* set images */
   for (i = 0 ; i < num ; i++) {
      thumb = node->data;
      detailview_update_thumbnail (tv, thumb, dest_mode);
      node = g_list_next (node);
   }
}



/******************************************************************************
 *
 *   public functions.
 *
 ******************************************************************************/
void
detailview_freeze (GimvThumbView *tv)
{
}


void
detailview_thaw (GimvThumbView *tv)
{
}


/* the row of a thumbnail */
static GimvRow *
detailview_new_row (GimvThumb *thumb)
{
   GList *data_node;
   gint j, colnum = detailview_title_idx_list_num + N_COLUMN;
   GimvRow *row;
   GType *types;

   types = g_new0 (GType, colnum);
   for (j = 0; j < colnum; j++)
      types[j] = G_TYPE_STRING;
   types[COLUMN_THUMB_DATA] = G_TYPE_POINTER;
   types[COLUMN_PIXMAP]     = GDK_TYPE_TEXTURE;
   row = gimv_row_newv (colnum, types);
   g_free (types);

   gimv_row_set (row,
                 COLUMN_THUMB_DATA, thumb,
                 COLUMN_TERMINATOR);
   data_node = detailview_title_idx_list;
   for (j = N_COLUMN;
        j < colnum && data_node;
        j++, data_node = g_list_next (data_node))
   {
      gint idx = GPOINTER_TO_INT (data_node->data);
      gchar *text = NULL;

      if (detailview_columns[idx].func)
         text = detailview_columns[idx].func (thumb);

      gimv_row_set (row,
                    j, text,
                    COLUMN_TERMINATOR);

      if (detailview_columns[idx].free && text)
         g_free (text);
   }

   return row;
}


void
detailview_append_thumb_frame (GimvThumbView *tv, GimvThumb *thumb,
                               const gchar *dest_mode)
{
   DetailViewData *tv_data;
   gint pos;
   GimvRow *row;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);

   /* image sizes from the file headers, in the background */
   detailview_size_job_start (tv, pos == 0);

   tv_data = g_object_get_data (G_OBJECT(tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data && tv_data->treeview);

   row = detailview_new_row (thumb);
   if (pos < 0 || (guint) pos > g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)))
      pos = g_list_model_get_n_items (G_LIST_MODEL (tv_data->store));
   g_list_store_insert (tv_data->store, pos, row);
   g_object_unref (row);

   detailview_set_selection (tv, thumb, thumb->selected);
}


/* all the thumbnails of the list into an empty view, in one change of the
 * model: one by one, 80,000 rows took minutes */
static void
detailview_append_all (GimvThumbView *tv)
{
   DetailViewData *tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   GPtrArray *rows;
   GList *node;

   g_return_if_fail (tv_data && tv_data->store);

   detailview_size_job_start (tv, TRUE);
   rows = g_ptr_array_new_with_free_func (g_object_unref);
   for (node = tv->thumblist; node; node = g_list_next (node))
      g_ptr_array_add (rows, detailview_new_row (node->data));
   g_list_store_splice (tv_data->store,
                        0, g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)),
                        rows->pdata, rows->len);
   g_ptr_array_unref (rows);

   for (node = tv->thumblist; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      if (thumb->selected)
         detailview_set_selection (tv, thumb, TRUE);
   }
}


void
detailview_update_thumbnail (GimvThumbView  *tv, GimvThumb *thumb,
                             const gchar *dest_mode)
{
   DetailViewData *tv_data;
   GdkTexture *pixmap = NULL;
   GdkTexture *mask = NULL;
   GList *node;
   gint pos, i;
   GimvRow *row;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));
   g_return_if_fail (GIMV_IS_THUMB (thumb));

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data && tv_data->treeview);

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);

   /* set thumbnail */
   if (!strcmp (DETAIL_ICON_LABEL, dest_mode)) {
      gimv_thumb_get_icon (thumb, &pixmap, &mask);
   } else if (!strcmp (DETAIL_THUMB_LABEL, dest_mode)) {
      gimv_thumb_get_thumb (thumb, &pixmap, &mask);
   }

   if (pos < 0) return;
   row = gimv_row_store_peek (G_LIST_MODEL (tv_data->store), pos);
   if (!row) return;

   /* GTK4 port: the text columns are also synced without a pixmap (plain
      "Detail" mode), e.g. after a comment was saved */
   if (pixmap
       || !strcmp (DETAIL_ICON_LABEL, dest_mode)
       || !strcmp (DETAIL_THUMB_LABEL, dest_mode))
      /* NULL: the thumbnail was dropped (large collection) */
      gimv_row_set (row,
                    COLUMN_PIXMAP,   pixmap,
                    COLUMN_TERMINATOR);

   /* reset column data */
   node = detailview_title_idx_list;
   for (i = N_COLUMN, node = detailview_title_idx_list;
        node;
        i++, node = g_list_next (node))
   {
      gint idx = GPOINTER_TO_INT (node->data);
      gchar *str;

      if (!detailview_columns[idx].need_sync) continue;

      str = detailview_columns[idx].func (thumb);

      gimv_row_set (row,
                    i, str,
                    COLUMN_TERMINATOR);

      if (detailview_columns[idx].free)
         g_free (str);
   }

   return;
}


GList *
detailview_get_load_list (GimvThumbView *tv)
{
   GList *loadlist = NULL, *node;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), NULL);

   if (!strcmp (DETAIL_VIEW_LABEL, tv->summary_mode)) return NULL;

   for (node = tv->thumblist; node; node = g_list_next (node)) {
      GimvThumb *thumb = node->data;
      GdkTexture *pixmap = NULL;
      GdkTexture *mask = NULL;

      gimv_thumb_get_thumb (thumb, &pixmap, &mask);
      if (!pixmap)
         loadlist = g_list_prepend (loadlist, thumb);
   }

   return g_list_reverse (loadlist);
}


void
detailview_remove_thumbnail (GimvThumbView *tv, GimvThumb *thumb)
{
   DetailViewData *tv_data;
   gint pos;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));
   g_return_if_fail (GIMV_IS_THUMB (thumb));

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data);

   /* all of them: at once (removing 80,000 rows one by one takes minutes) */
   if (gimv_thumb_view_is_clearing (tv)) {
      if (g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)) > 0)
         g_list_store_remove_all (tv_data->store);
      return;
   }

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);
   if (pos < 0) return;

   if ((guint) pos < g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)))
      g_list_store_remove (tv_data->store, pos);
}


void
detailview_adjust (GimvThumbView *tv, GimvThumb *thumb)
{
   DetailViewData *tv_data;
   GList *node;
   gint pos;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));

   node = g_list_find (gimv_thumb_view_get_list(), tv);
   if (!node) return;

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data);

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);
   if (pos < 0) return;

   if ((guint) pos < g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)))
      gimv_list_view_scroll_to (tv_data->treeview, pos, FALSE, FALSE);
}


gboolean
detailview_set_selection (GimvThumbView *tv, GimvThumb *thumb, gboolean select)
{
   DetailViewData *tv_data;
   gint pos;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), FALSE);

   if (!tv->thumblist) return FALSE;

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data && tv_data->treeview, FALSE);

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);

   if (pos >= 0) {
      GtkSelectionModel *selection
         = gimv_list_view_get_selection (tv_data->treeview);

      if ((guint) pos >= g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)))
         return TRUE;

      /* the selection model reads GimvThumb::selected */
      thumb->selected = select;
      gtk_selection_model_selection_changed (selection, pos, 1);
   }

   return TRUE;
}


void
detailview_set_focus (GimvThumbView *tv, GimvThumb *thumb)
{
   DetailViewData *tv_data;
   gint pos;

   g_return_if_fail (GIMV_IS_THUMB_VIEW (tv));
   if (!tv->thumblist) return;

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_if_fail (tv_data && tv_data->treeview);

   pos = gimv_thumb_view_get_thumb_index (tv, thumb);

   if (pos >= 0) {
      if ((guint) pos >= g_list_model_get_n_items (G_LIST_MODEL (tv_data->store)))
         return;
      gimv_list_view_set_focus_position (tv_data->treeview, pos, FALSE);
   } else {
      gtk_widget_grab_focus (tv_data->treeview);
   }
}


GimvThumb *
detailview_get_focus (GimvThumbView *tv)
{
   GimvThumb *thumb = NULL;
   DetailViewData *tv_data;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), NULL);
   if (!tv->thumblist) return NULL;

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data && tv_data->treeview, NULL);

   {
      guint position = gimv_list_view_get_focus_position (tv_data->treeview);

      if (position != GTK_INVALID_LIST_POSITION)
         thumb = thumb_at_position (tv_data, position);
   }

   return thumb;
}


gboolean
detailview_thumbnail_is_in_viewport (GimvThumbView *tv, GimvThumb *thumb)
{
   DetailViewData *tv_data;
   gint row;
   graphene_rect_t area;
   gint height;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), FALSE);
   g_return_val_if_fail (GIMV_IS_THUMB (thumb), FALSE);

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   g_return_val_if_fail (tv_data, FALSE);

   row = gimv_thumb_view_get_thumb_index (tv, thumb);
   if (row < 0) return FALSE;

   if (!gtk_widget_get_mapped (GTK_WIDGET (tv_data->treeview))) return FALSE;

   /* rows far from the visible area have no widgets */
   if (!gimv_list_view_get_row_area (tv_data->treeview, row, &area))
      return FALSE;

   /* intersect? */
   height = gtk_widget_get_height (tv_data->treeview);
   return area.origin.y + area.size.height > 0 && area.origin.y < height;
}


/* typing on the list: a file by its name */
static gchar *
detail_search_text (GimvRow *row, gpointer data)
{
   GimvThumb *thumb = gimv_row_peek_pointer (row, COLUMN_THUMB_DATA);
   const gchar *path;

   if (!GIMV_IS_THUMB (thumb) || !thumb->info
       || !(path = gimv_image_info_get_path (thumb->info)))
      return NULL;
   return charset_to_internal (fileutil_basename (path), conf.charset_filename,
                               conf.charset_auto_detect_fn, conf.charset_filename_mode);
}


/* the file found: selected alone and focused, as a click does */
static void
detail_search_select (GtkWidget *view, guint position, gpointer data)
{
   GimvThumbView *tv = data;
   DetailViewData *tv_data;
   GimvRow *row;
   GimvThumb *thumb;

   if (!GIMV_IS_THUMB_VIEW (tv)) return;
   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   if (!tv_data) return;
   row = gimv_row_store_peek (G_LIST_MODEL (tv_data->store), position);
   thumb = row ? gimv_row_peek_pointer (row, COLUMN_THUMB_DATA) : NULL;
   if (!GIMV_IS_THUMB (thumb)) return;
   gimv_thumb_view_set_selection_all (tv, FALSE);
   gimv_thumb_view_set_selection (thumb, TRUE);
   gimv_thumb_view_set_focus (tv, thumb);
}


static gboolean
cb_treeview_query_tooltip (GtkWidget *widget, gint x, gint y, gboolean keyboard,
                           GtkTooltip *tooltip, GimvThumbView *tv)
{
   GimvRow *row = NULL;
   GimvThumb *thumb;
   const gchar *file;
   gchar *text;

   if (keyboard || !GIMV_IS_THUMB_VIEW (tv)
       || !g_object_get_data (G_OBJECT (tv), "gimv-name-only"))
      return FALSE;
   if (!gimv_list_view_get_row_at (widget, x, y, NULL, &row) || !row)
      return FALSE;
   thumb = gimv_row_peek_pointer (row, COLUMN_THUMB_DATA);
   if (!GIMV_IS_THUMB (thumb) || !thumb->info
       || !(file = gimv_image_info_get_path (thumb->info)))
      return FALSE;
   text = charset_to_internal (file, conf.charset_filename,
                               conf.charset_auto_detect_fn,
                               conf.charset_filename_mode);
   gtk_tooltip_set_text (tooltip, text);
   g_free (text);
   return TRUE;
}


GtkWidget *
detailview_create (GimvThumbView *tv, const gchar *dest_mode)
{
   DetailViewData *tv_data;
   const gint ntypes = 128;
   gint ncolumns;
   gboolean show_title;

   g_return_val_if_fail (GIMV_IS_THUMB_VIEW (tv), NULL);

   detailview_create_title_idx_list ();
   ncolumns = detailview_title_idx_list_num + N_COLUMN;
   if (ncolumns > ntypes) {
      g_warning("Too many columns are specified: %d\n", ncolumns - N_COLUMN);
      ncolumns = ntypes;
   }

   tv_data = g_object_get_data (G_OBJECT (tv), DETAIL_VIEW_LABEL);
   if (!tv_data) {
      tv_data = detailview_new (tv);
      g_return_val_if_fail (tv_data, NULL);
   }


   /* create the list widget
      (GTK4: GtkTreeView (deprecated) -> GtkColumnView) */
   if (tv_data->store)
      g_object_unref (tv_data->store);
   tv_data->store = gimv_row_store_new ();
   tv_data->treeview
      = gtk_column_view_new (detail_selection_new (G_LIST_MODEL (tv_data->store),
                                                   tv));
   gtk_column_view_set_enable_rubberband (GTK_COLUMN_VIEW (tv_data->treeview),
                                          FALSE);
   gtk_column_view_set_reorderable (GTK_COLUMN_VIEW (tv_data->treeview), FALSE);
   gtk_scrollable_set_hscroll_policy (GTK_SCROLLABLE (tv_data->treeview),
                                      GTK_SCROLL_NATURAL);

   set_column_types (tv, tv_data, dest_mode);
   detailview_prefs_get_value ("show_title", (gpointer) &show_title);
   gimv_column_view_set_headers_visible (GTK_COLUMN_VIEW (tv_data->treeview),
                                         show_title);
   if (!strcmp (DETAIL_ICON_LABEL, dest_mode)) {
      gtk_widget_set_name (tv_data->treeview, "DetailIconMode");
   }
   if (!strcmp (DETAIL_THUMB_LABEL, dest_mode)) {
      gtk_widget_set_name (tv_data->treeview, "DetailThumbMode");
   }

   gtk_widget_set_visible (tv_data->treeview, TRUE);

   /* the status bar shows the focused file and the files selected (Shift,
    * Ctrl); bound to tv: the view may outlive it, when a tab is closed */
   g_signal_connect_object (gtk_column_view_get_model (GTK_COLUMN_VIEW (tv_data->treeview)),
                            "selection-changed",
                            G_CALLBACK (gimv_thumb_view_focus_changed), tv,
                            G_CONNECT_SWAPPED);
   /* the files of a collection from the index show the name only: the full
      path as the tooltip */
   gtk_widget_set_has_tooltip (tv_data->treeview, TRUE);
   gimv_list_view_enable_search (tv_data->treeview, -1, detail_search_text, NULL);
   gimv_list_view_set_search_select (tv_data->treeview, detail_search_select, tv);
   g_signal_connect_object (tv_data->treeview, "query-tooltip",
                            G_CALLBACK (cb_treeview_query_tooltip), tv, 0);

   gimv_event_connect (GTK_WIDGET (tv_data->treeview), GIMV_EVENT_KEY_PRESS, G_CALLBACK (cb_treeview_key_press), tv);
   gimv_event_connect (GTK_WIDGET (tv_data->treeview), GIMV_EVENT_BUTTON_PRESS, G_CALLBACK (cb_treeview_button_press), tv);
   gimv_event_connect (GTK_WIDGET (tv_data->treeview), GIMV_EVENT_SCROLL, G_CALLBACK (cb_treeview_scroll), tv);
   /* SIGNAL_CONNECT_TRANSRATE_SCROLL (tv_data->treeview); */
   gimv_event_connect (GTK_WIDGET (tv_data->treeview), GIMV_EVENT_BUTTON_RELEASE, G_CALLBACK (cb_treeview_button_release), tv);
   gimv_event_connect (GTK_WIDGET (tv_data->treeview), GIMV_EVENT_MOTION_NOTIFY, G_CALLBACK (cb_treeview_motion_notify), tv);

   /* for drop file list */
   dnd_src_set  (tv_data->treeview, detailview_dnd_targets, detailview_dnd_targets_num);
   dnd_dest_set (tv_data->treeview, detailview_dnd_targets, detailview_dnd_targets_num);

   gimv_dnd_connect (GTK_WIDGET (tv_data->treeview), GIMV_DND_DRAG_BEGIN, G_CALLBACK (gimv_thumb_view_drag_begin_cb), tv);
   gimv_dnd_connect (GTK_WIDGET (tv_data->treeview), GIMV_DND_DRAG_DATA_GET, G_CALLBACK (gimv_thumb_view_drag_data_get_cb), tv);
   gimv_dnd_connect (GTK_WIDGET (tv_data->treeview), GIMV_DND_DRAG_DATA_RECEIVED, G_CALLBACK (cb_treeview_drag_data_received), tv);
   gimv_dnd_connect (GTK_WIDGET (tv_data->treeview), GIMV_DND_DRAG_DATA_DELETE, G_CALLBACK (gimv_thumb_view_drag_data_delete_cb), tv);
   gimv_dnd_connect (GTK_WIDGET (tv_data->treeview), GIMV_DND_DRAG_END, G_CALLBACK (gimv_thumb_view_drag_end_cb), tv);
   g_object_set_data (G_OBJECT (tv_data->treeview), "gimv-tab", tv);


   /* set data */
   if (tv->thumblist) {
      detailview_append_all (tv);
      detailview_set_pixmaps (tv, dest_mode);
   }

   return tv_data->treeview;
}

