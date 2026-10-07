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
 * $Id: gimv_zlist.c,v 1.5 2004/09/21 08:44:32 makeinu Exp $
 */

/*
 *  These codes are mostly taken from Another X image viewer.
 *
 *  Another X image viewer Author:
 *     David Ramboz <dramboz@users.sourceforge.net>
 */

/*
 *  GTK4 port:
 *
 *  GimvZList has no GdkWindow of its own any more.  The whole visible part
 *  of the list is painted by the "draw" method of GimvScrolled (cairo, widget
 *  coordinates); functions which painted a cell immediately in GTK2 queue a
 *  redraw instead.  The rubber band of the region selection is painted as an
 *  overlay at the end of each draw instead of with an XOR GC.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <gtk/gtk.h>

#include "gimv_zlist.h"

/* GTK4: widgets have no border width any more */
#define bw(widget) 0

#define CELL_COL_FROM_X(list, x) \
   (GIMV_SCROLLED_VX (list, (x) - (list)->x_pad - bw (list)) / (list)->cell_width)
#define CELL_ROW_FROM_Y(list, y) \
   (GIMV_SCROLLED_VY (list, (y) - (list)->y_pad - bw (list)) / (list)->cell_height)

#define CELL_X_FROM_COL(list, col) \
   (GIMV_SCROLLED_X (list, (col) * (list)->cell_width  + (list)->x_pad))
#define CELL_Y_FROM_ROW(list, row) \
   (GIMV_SCROLLED_Y (list, (row) * (list)->cell_height + (list)->y_pad))

#define LIST_WIDTH(list)  ((list)->columns * (list)->cell_width /* + (list)->cell_x_pad */)
#define LIST_HEIGHT(list) ((list)->rows * (list)->cell_height /* + (list)->cell_y_pad */)

#define HIGHLIGHT_SIZE 2

#define WIDGET_DRAW(widget) gtk_widget_queue_draw (widget)

static void     gimv_zlist_finalize             (GObject          *object);
static void     gimv_zlist_measure              (GtkWidget        *widget,
                                                 GtkOrientation    orientation,
                                                 gint              for_size,
                                                 gint             *minimum,
                                                 gint             *natural,
                                                 gint             *minimum_baseline,
                                                 gint             *natural_baseline);
static void     gimv_zlist_size_allocate        (GtkWidget        *widget,
                                                 gint              width,
                                                 gint              height,
                                                 gint              baseline);
static void     gimv_zlist_map                  (GtkWidget        *widget);
static void     gimv_zlist_update               (GimvZList        *list);
static void     gimv_zlist_draw_list            (GimvZList        *list,
                                                 cairo_t          *cr,
                                                 GdkRectangle     *area);
static void     gimv_zlist_draw_selection_region(GimvZList        *list,
                                                 cairo_t          *cr);
static void     gimv_zlist_draw                 (GimvScrolled     *scrolled,
                                                 cairo_t          *cr,
                                                 GdkRectangle     *area);

static gboolean gimv_zlist_button_press         (GimvScrolled     *scrolled,
                                                 GimvEventButton  *event);
static gboolean gimv_zlist_button_release       (GimvScrolled     *scrolled,
                                                 GimvEventButton  *event);
static gboolean gimv_zlist_motion_notify        (GimvScrolled     *scrolled,
                                                 GimvEventMotion  *event);
static gboolean gimv_zlist_key_press            (GimvScrolled     *scrolled,
                                                 GimvEventKey     *event);
static void     gimv_zlist_drag_motion          (GimvScrolled     *scrolled,
                                                 gint              x,
                                                 gint              y);
static void     gimv_zlist_drag_leave           (GimvScrolled     *scrolled);
static void     gimv_zlist_highlight            (GimvZList        *list,
                                                 cairo_t          *cr);
static void     gimv_zlist_unhighlight          (GimvZList        *list);
static gboolean gimv_zlist_focus                (GimvZList        *list,
                                                 GtkDirectionType  dir);
static void     gimv_zlist_cell_draw_focus      (GimvZList        *list,
                                                 gint              index);
static void     gimv_zlist_cell_draw_default    (GimvZList        *list,
                                                 gint              index);

static void     gimv_zlist_adjust_adjustments   (GimvScrolled     *scrolled);
static void     gimv_zlist_cell_pos             (GimvZList        *list,
                                                 gint              index,
                                                 gint             *row,
                                                 gint             *col);
static void     gimv_zlist_cell_area            (GimvZList        *list,
                                                 gint              index,
                                                 GdkRectangle     *cell_area);

enum {
   CLEAR,
   CELL_DRAW,
   CELL_SIZE_REQUEST,
   CELL_DRAW_FOCUS,
   CELL_DRAW_DEFAULT,
   CELL_SELECT,
   CELL_UNSELECT,
   LAST_SIGNAL
};

static guint gimv_zlist_signals [LAST_SIGNAL] = { 0 };


G_DEFINE_TYPE (GimvZList, gimv_zlist, GIMV_TYPE_SCROLLED)

#define parent_class_scrolled GIMV_SCROLLED_CLASS (gimv_zlist_parent_class)


static void
gimv_zlist_class_init (GimvZListClass *klass)
{
   GObjectClass *gobject_class;
   GtkWidgetClass *widget_class;
   GimvScrolledClass *scrolled_class;

   gobject_class   = G_OBJECT_CLASS (klass);
   widget_class    = GTK_WIDGET_CLASS (klass);
   scrolled_class  = GIMV_SCROLLED_CLASS (klass);

   gimv_zlist_signals [CLEAR] =
      g_signal_new ("clear",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, clear),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

   /* (cairo_t *cr, gpointer cell, GdkRectangle *cell_area, GdkRectangle *area) */
   gimv_zlist_signals [CELL_DRAW] =
      g_signal_new ("cell_draw",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_draw),
                    NULL, NULL,
                    NULL, /* generic marshaller */
                    G_TYPE_NONE, 4,
                    G_TYPE_POINTER, G_TYPE_POINTER,
                    G_TYPE_POINTER, G_TYPE_POINTER);

   /* (gpointer cell, GtkRequisition *requisition) */
   gimv_zlist_signals [CELL_SIZE_REQUEST] =
      g_signal_new ("cell_size_request",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_size_request),
                    NULL, NULL,
                    NULL, /* generic marshaller */
                    G_TYPE_NONE, 2,
                    G_TYPE_POINTER, G_TYPE_POINTER);

   /* (cairo_t *cr, gpointer cell, GdkRectangle *cell_area) */
   gimv_zlist_signals [CELL_DRAW_FOCUS] =
      g_signal_new ("cell_draw_focus",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_draw_focus),
                    NULL, NULL,
                    NULL, /* generic marshaller */
                    G_TYPE_NONE, 3,
                    G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER);

   /* (cairo_t *cr, gpointer cell, GdkRectangle *cell_area) */
   gimv_zlist_signals [CELL_DRAW_DEFAULT] =
      g_signal_new ("cell_draw_default",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_draw_default),
                    NULL, NULL,
                    NULL, /* generic marshaller */
                    G_TYPE_NONE, 3,
                    G_TYPE_POINTER, G_TYPE_POINTER, G_TYPE_POINTER);

   gimv_zlist_signals [CELL_SELECT] =
      g_signal_new ("cell_select",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_select),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__INT,
                    G_TYPE_NONE, 1, G_TYPE_INT);

   gimv_zlist_signals [CELL_UNSELECT] =
      g_signal_new ("cell_unselect",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvZListClass, cell_unselect),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__INT,
                    G_TYPE_NONE, 1, G_TYPE_INT);

   gobject_class->finalize              = gimv_zlist_finalize;

   widget_class->measure                = gimv_zlist_measure;
   widget_class->size_allocate          = gimv_zlist_size_allocate;
   widget_class->map                    = gimv_zlist_map;

   scrolled_class->adjust_adjustments   = gimv_zlist_adjust_adjustments;
   scrolled_class->draw                 = gimv_zlist_draw;
   scrolled_class->button_press         = gimv_zlist_button_press;
   scrolled_class->button_release       = gimv_zlist_button_release;
   scrolled_class->motion_notify        = gimv_zlist_motion_notify;
   scrolled_class->key_press            = gimv_zlist_key_press;
   scrolled_class->drag_motion          = gimv_zlist_drag_motion;
   scrolled_class->drag_leave           = gimv_zlist_drag_leave;
   /* focus_in / focus_out: GimvScrolled queues a redraw, nothing else to do */

   klass->clear                         = NULL;
   klass->cell_draw                     = NULL;
   klass->cell_size_request             = NULL;
   klass->cell_draw_focus               = NULL;
   klass->cell_draw_default             = NULL;
   klass->cell_select                   = NULL;
   klass->cell_unselect                 = NULL;
}


static void
gimv_zlist_init (GimvZList *list)
{
   gtk_widget_set_focusable (GTK_WIDGET (list), TRUE);

   list->flags            = 0;
   list->cell_width       = 1;
   list->cell_height      = 1;
   list->rows             = 1;
   list->columns          = 1;
   list->cells            = NULL;
   list->cell_count       = 0;
   list->selection_mode   = GTK_SELECTION_SINGLE;
   list->selection        = NULL;
   list->focus            = -1;
   list->anchor           = -1;
   list->cell_x_pad       = 4;
   list->cell_y_pad       = 4;
   list->x_pad            = 0;
   list->y_pad            = 0;
   list->entered_cell     = NULL;

   list->region_select    = GIMV_ZLIST_REGION_SELECT_OFF;
   list->selection_mask   = NULL;
   list->button_pressed   = FALSE;
}


void
gimv_zlist_construct (GimvZList *list, int flags)
{
   g_return_if_fail (list);

   list->flags          = flags;
   if (!list->cells)
      list->cells       = g_array_new (0, 0, sizeof (gpointer));
}


GtkWidget*
gimv_zlist_new (guint flags)
{
   GimvZList *list;

   list = g_object_new (gimv_zlist_get_type (), NULL);
   g_return_val_if_fail (list, NULL);

   gimv_zlist_construct (list, flags);

   return (GtkWidget*) list;
}


void
gimv_zlist_set_to_vertical (GimvZList *zlist)
{
   g_return_if_fail (GIMV_IS_ZLIST (zlist));

   zlist->flags &= ~GIMV_ZLIST_HORIZONTAL;

   gimv_zlist_update (zlist);
   if (gtk_widget_get_visible (GTK_WIDGET (zlist))) {
      WIDGET_DRAW (GTK_WIDGET (zlist));
   }
}


void
gimv_zlist_set_to_horizontal (GimvZList *zlist)
{
   g_return_if_fail (GIMV_IS_ZLIST (zlist));

   zlist->flags |= GIMV_ZLIST_HORIZONTAL;

   gimv_zlist_update (zlist);
   if (gtk_widget_get_visible (GTK_WIDGET (zlist))) {
      WIDGET_DRAW (GTK_WIDGET (zlist));
   }
}


static void
gimv_zlist_finalize (GObject *object)
{
   GimvZList *list = GIMV_ZLIST (object);

   if (list->cells)
      g_array_free (list->cells, TRUE);
   list->cells = NULL;
   list->cell_count = 0;

   g_list_free (list->selection);
   list->selection = NULL;
   g_list_free (list->selection_mask);
   list->selection_mask = NULL;

   G_OBJECT_CLASS (gimv_zlist_parent_class)->finalize (object);
}


static void
gimv_zlist_map (GtkWidget *widget)
{
   GTK_WIDGET_CLASS (gimv_zlist_parent_class)->map (widget);
   gimv_zlist_update (GIMV_ZLIST (widget));
}


static void
gimv_zlist_measure (GtkWidget      *widget,
                    GtkOrientation  orientation,
                    gint            for_size,
                    gint           *minimum,
                    gint           *natural,
                    gint           *minimum_baseline,
                    gint           *natural_baseline)
{
   /* same as the old size_request: the list is always put into a
      scrolled window */
   *minimum = *natural = 50;
}


static void
gimv_zlist_size_allocate (GtkWidget *widget,
                          gint       width,
                          gint       height,
                          gint       baseline)
{
   /* recomputes rows/columns and configures the adjustments */
   gimv_zlist_update (GIMV_ZLIST (widget));
}


static void
gimv_zlist_update (GimvZList *list)
{
   GtkWidget *widget;
   gint width, height;

   widget = GTK_WIDGET(list);

   width  = gtk_widget_get_width  (widget) - 2 * bw (widget);
   height = gtk_widget_get_height (widget) - 2 * bw (widget);

   if (list->flags & GIMV_ZLIST_HORIZONTAL) {
      if (list->flags & GIMV_ZLIST_1)
         list->cell_height = MAX (1, height);

      list->rows    = MAX(1, height / list->cell_height);
      list->columns = list->cell_count % list->rows ?
         list->cell_count / list->rows + 1 :
         list->cell_count / list->rows;

      list->y_pad = (height - LIST_HEIGHT(list)) / 2;
      if (list->y_pad < 0) list->y_pad = 0;
      list->x_pad = 0;

   } else {
      if (list->flags & GIMV_ZLIST_1)
         list->cell_width = MAX (1, width);

      list->columns = MAX(1, width / list->cell_width);
      list->rows    = list->cell_count % list->columns ?
         list->cell_count / list->columns + 1 :
         list->cell_count / list->columns;

      list->x_pad = (width - LIST_WIDTH(list)) / 2;
      if (list->x_pad < 0) list->x_pad = 0;
      list->y_pad = 0;
   }

   /* gtk_adjustment_configure () also clamps the current value */
   gimv_zlist_adjust_adjustments (GIMV_SCROLLED(list));
}


gboolean
gimv_zlist_get_cell_area (GimvZList *list, gint index, GdkRectangle *area)
{
   gint col, row;

   g_return_val_if_fail (GIMV_IS_ZLIST (list), FALSE);
   g_return_val_if_fail (area, FALSE);
   g_return_val_if_fail (index >= 0 && index < list->cell_count, FALSE);

   if (list->flags & GIMV_ZLIST_HORIZONTAL){
      col = index / list->rows;
      row = index % list->rows;
   } else {
      col = index % list->columns;
      row = index / list->columns;
   }

   area->x = CELL_X_FROM_COL(list, col) + list->cell_x_pad;
   area->y = CELL_Y_FROM_ROW(list, row) + list->cell_y_pad;
   area->width  = list->cell_width - list->cell_x_pad;
   area->height = list->cell_height - list->cell_y_pad;

   return TRUE;
}


static void
gimv_zlist_draw_list (GimvZList *list, cairo_t *cr, GdkRectangle *area)
{
   gpointer cell;
   GdkRectangle cell_area, draw_cell_area, intersect_area;
   gint first_row, last_row;
   gint first_column, last_column;
   gint i, j, idx;

   first_column = CELL_COL_FROM_X(list, area->x);
   first_column = CLAMP(first_column, 0, list->columns);
   last_column  = CELL_COL_FROM_X(list, area->x + area->width) + 1;
   last_column  = CLAMP(last_column, 0, list->columns);

   first_row    = CELL_ROW_FROM_Y(list, area->y);
   first_row    = CLAMP(first_row, 0, list->rows);
   last_row     = CELL_ROW_FROM_Y(list, area->y + area->height) + 1;
   last_row     = CLAMP(last_row, 0, list->rows);

   /* the background (padding, empty cells) is already cleared */

   for (i = first_row; i < last_row; i++) {
      for (j = first_column; j < last_column; j++) {
         if (list->flags & GIMV_ZLIST_HORIZONTAL)
            idx = j * list->rows + i;
         else
            idx = i * list->columns + j;

         if (idx < 0 || idx >= list->cell_count) continue;

         cell_area.x = CELL_X_FROM_COL(list, j) + list->cell_x_pad;
         cell_area.y = CELL_Y_FROM_ROW(list, i) + list->cell_y_pad;
         cell_area.width  = list->cell_width - list->cell_x_pad;
         cell_area.height = list->cell_height - list->cell_y_pad;

         if (!gdk_rectangle_intersect (area, &cell_area, &intersect_area))
            continue;

         cell = GIMV_ZLIST_CELL_FROM_INDEX (list, idx);

         /* the cell_draw method may modify the rectangle */
         draw_cell_area = cell_area;
         cairo_save (cr);
         g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_DRAW], 0,
                        cr, cell, &draw_cell_area, &intersect_area);
         cairo_restore (cr);

         /* decoration of a cell without focus (the focused one is
            decorated by gimv_zlist_draw ()) */
         if (idx != list->focus) {
            draw_cell_area = cell_area;
            cairo_save (cr);
            g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_DRAW_DEFAULT], 0,
                           cr, cell, &draw_cell_area);
            cairo_restore (cr);
         }
      } /* columns loop */
   } /* rows loop */
}


/*
 *  GTK4: the rubber band was drawn with an XOR GC in GTK2.  It is an overlay
 *  painted at the end of each draw now.
 */
static void
gimv_zlist_draw_selection_region (GimvZList *list, cairo_t *cr)
{
   GtkWidget *widget;
   GimvScrolled *scrolled;
   GdkRectangle widget_area, region_area, draw_area;
   gint ds_vx, ds_vy, de_vx, de_vy;
   GdkRGBA color;
   const double dash[] = {2.0, 1.0};

   widget = GTK_WIDGET (list);
   scrolled = GIMV_SCROLLED (list);

   if (!scrolled->pressed) return;
   if (scrolled->drag_start_vx < 0 || scrolled->drag_start_vy < 0) return;
   if (scrolled->drag_motion_x < 0 || scrolled->drag_motion_y < 0) return;

   ds_vx = scrolled->drag_start_vx;
   ds_vy = scrolled->drag_start_vy;
   de_vx = GIMV_SCROLLED_VX (scrolled, scrolled->drag_motion_x);
   de_vy = GIMV_SCROLLED_VY (scrolled, scrolled->drag_motion_y);

   region_area.x = GIMV_SCROLLED_X (list, MIN (ds_vx, de_vx));
   region_area.y = GIMV_SCROLLED_Y (list, MIN (ds_vy, de_vy));
   region_area.width  = abs (de_vx - ds_vx);
   region_area.height = abs (de_vy - ds_vy);

   widget_area.x = widget_area.y = 0;
   widget_area.width  = gtk_widget_get_width (widget);
   widget_area.height = gtk_widget_get_height (widget);

   /* a zero sized rectangle doesn't intersect, but the GTK2 version drew a
      line in that case */
   region_area.width  = MAX (region_area.width,  1);
   region_area.height = MAX (region_area.height, 1);

   if (!gdk_rectangle_intersect (&widget_area, &region_area, &draw_area))
      return;

   gimv_widget_get_fg_color (widget, &color);

   cairo_save (cr);
   gdk_cairo_set_source_rgba (cr, &color);
   cairo_set_line_width (cr, 1.0);
   cairo_set_dash (cr, dash, 2, 0.0);
   cairo_rectangle (cr,
                    draw_area.x + 0.5, draw_area.y + 0.5,
                    MAX (draw_area.width  - 1, 0),
                    MAX (draw_area.height - 1, 0));
   cairo_stroke (cr);
   cairo_restore (cr);
}


static void
gimv_zlist_draw (GimvScrolled *scrolled, cairo_t *cr, GdkRectangle *area)
{
   GimvZList *list;
   GtkWidget *widget;
   GdkRGBA base;

   list = GIMV_ZLIST(scrolled);
   widget = GTK_WIDGET(scrolled);

   /* clear the background (was the background of the GdkWindow) */
   gimv_widget_get_base_color (widget, &base);
   cairo_save (cr);
   gdk_cairo_set_source_rgba (cr, &base);
   gdk_cairo_rectangle (cr, area);
   cairo_fill (cr);
   cairo_restore (cr);

   if (list->cell_count > 0 && list->cells) {
      gimv_zlist_draw_list (list, cr, area);

      if (list->focus > -1 && list->focus < list->cell_count) {
         GdkRectangle cell_area;

         gimv_zlist_cell_area (list, list->focus, &cell_area);
         cairo_save (cr);
         g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_DRAW_FOCUS], 0,
                        cr, GIMV_ZLIST_CELL_FROM_INDEX (list, list->focus),
                        &cell_area);
         cairo_restore (cr);
      }
   }

   if (list->region_select)
      gimv_zlist_draw_selection_region (list, cr);

   if (list->flags & GIMV_ZLIST_HIGHLIGHTED)
      gimv_zlist_highlight (list, cr);
}


static gboolean
gimv_zlist_button_press (GimvScrolled *scrolled, GimvEventButton *event)
{
   GtkWidget *widget;
   GimvZList *list;
   gboolean retval = FALSE;
   gint idx;

   widget = GTK_WIDGET (scrolled);
   list = GIMV_ZLIST (scrolled);

   /* grab focus */
   if (!gtk_widget_has_focus (widget))
      gtk_widget_grab_focus (widget);

   /* call parent method */
   if (parent_class_scrolled->button_press)
      retval = parent_class_scrolled->button_press (scrolled, event);

   if (list->region_select) {
      list->region_select = GIMV_ZLIST_REGION_SELECT_OFF;
      WIDGET_DRAW (widget);
   }

   if (event->type != GIMV_BUTTON_PRESS || event->button != 1)
      return retval;

   /* get the selected cell's index */
   idx = gimv_zlist_cell_index_from_xy (list, event->x, event->y);

   /* set to region select mode */
   if (idx < 0) {
      if (event->state & GDK_CONTROL_MASK) {          /* toggle mode */
         list->region_select = GIMV_ZLIST_REGION_SELECT_TOGGLE;
         gimv_zlist_set_selection_mask (list, NULL);
      } else if (event->state & GDK_SHIFT_MASK) {   /* expand mode */
         list->region_select = GIMV_ZLIST_REGION_SELECT_EXPAND;
         gimv_zlist_set_selection_mask (list, NULL);
      } else {
         list->region_select = GIMV_ZLIST_REGION_SELECT_NORMAL;
         gimv_zlist_unselect_all (list);
      }
      WIDGET_DRAW (widget);

   } else {
      /* set focus */
      if (list->focus != idx) {
         if (list->focus > -1)
            gimv_zlist_cell_draw_default (list, list->focus);
         list->focus = idx;
      }

      /* set to the DnD mode */
      if (list->flags & GIMV_ZLIST_USES_DND
          && g_list_find (list->selection, GUINT_TO_POINTER(idx)))
      {
         list->anchor = idx;
         gimv_zlist_cell_draw_focus (list, idx);
         return retval;
      }

      /* set selection */
      switch (list->selection_mode) {
      case GTK_SELECTION_SINGLE:
         list->anchor = idx;
         gimv_zlist_cell_draw_focus (list, idx);
         break;

      case GTK_SELECTION_BROWSE:
         gimv_zlist_unselect_all (list);
         gimv_zlist_cell_select (list, idx);
         break;

      case GTK_SELECTION_MULTIPLE:   /* was GTK_SELECTION_EXTENDED */
         if (event->state & GDK_CONTROL_MASK) {
            list->anchor = idx;
            gimv_zlist_cell_toggle (list, idx);
         } else if (event->state & GDK_SHIFT_MASK) {
            gimv_zlist_extend_selection (list, idx);
         } else {
            list->anchor = idx;

            if (!g_list_find (list->selection, GUINT_TO_POINTER(idx))) {
               gimv_zlist_unselect_all (list);
               gimv_zlist_cell_select (list, idx);
            }
         }
         gimv_zlist_cell_draw_focus (list, idx);
         break;

      default:
         break;
      }
   }

   /* GTK4: GTK2 grabbed the pointer here.  GTK4 has implicit grabs, only
      remember that button 1 is held down */
   list->button_pressed = TRUE;

   return retval;
}


static gboolean
gimv_zlist_button_release (GimvScrolled *scrolled, GimvEventButton *event)
{
   GtkWidget *widget;
   GimvZList *list;
   gboolean retval = FALSE;
   gint index;

   widget = GTK_WIDGET (scrolled);
   list = GIMV_ZLIST (scrolled);

   /* call parent class callback */
   if (parent_class_scrolled->button_release)
      retval = parent_class_scrolled->button_release (scrolled, event);

   /* was: remove the grab */
   list->button_pressed = FALSE;

   index = gimv_zlist_cell_index_from_xy (list, event->x, event->y);
   if (index < 0) goto FUNC_END;

   switch (list->selection_mode) {
   case GTK_SELECTION_SINGLE:
      if (list->anchor == index) {
         list->focus = index;
         gimv_zlist_unselect_all (list);
         gimv_zlist_cell_toggle (list, index);
      }
      list->anchor = index;
      break;


   case GTK_SELECTION_MULTIPLE:   /* was GTK_SELECTION_EXTENDED */
      if (event->state & GDK_CONTROL_MASK) {
      } else if (event->state & GDK_SHIFT_MASK) {
      } else {
         if (event->button == 1 && !list->region_select) {
            gimv_zlist_unselect_all (list);
            gimv_zlist_cell_select (list, index);
         }
      }

      if ((list->flags & GIMV_ZLIST_USES_DND)) {
         gint x, y;

         /* on a drag-drop event, the event->x & event->y always seem to be 0 */
         if (!gimv_widget_get_pointer (widget, &x, &y)) {
            x = event->x;
            y = event->y;
         }
         index = gimv_zlist_cell_index_from_xy (list, x, y);
         if (index < 0) goto FUNC_END;
         /*
           if (list->anchor == index)
           gimv_zlist_extend_selection (list, list->focus);
         */
      }

      list->anchor = list->focus;
      break;

   default:
      break;
   }

FUNC_END:

   /* unset region select */
   if (list->region_select)
      WIDGET_DRAW (widget);
   gimv_zlist_unset_selection_mask (list);
   list->region_select = GIMV_ZLIST_REGION_SELECT_OFF;

   return retval;
}


static gboolean
gimv_zlist_motion_notify (GimvScrolled *scrolled, GimvEventMotion *event)
{
   GtkWidget *widget;
   GimvZList *list;
   gint index, x, y;
   gboolean retval = FALSE;

   gint start_x, start_y, end_x, end_y;
   gint flags;
   gboolean pressed;

   widget = GTK_WIDGET (scrolled);
   list = GIMV_ZLIST (scrolled);

   flags = scrolled->autoscroll_flags;
   pressed = scrolled->pressed;

   /* call parent class callback */
   if (parent_class_scrolled->motion_notify)
      retval = parent_class_scrolled->motion_notify (scrolled, event);

   /* a DnD operation may have eaten the button release event */
   if (list->button_pressed && !(event->state & GDK_BUTTON1_MASK))
      list->button_pressed = FALSE;

   if (list->region_select) {
      if ((pressed && (flags & GIMV_SCROLLED_AUTO_SCROLL_MOTION))
          || (flags & GIMV_SCROLLED_AUTO_SCROLL_MOTION_ALL))
      {
         start_x = MIN (scrolled->drag_start_vx, GIMV_SCROLLED_VX (scrolled, event->x));
         start_y = MIN (scrolled->drag_start_vy, GIMV_SCROLLED_VY (scrolled, event->y));
         end_x   = MAX (scrolled->drag_start_vx, GIMV_SCROLLED_VX (scrolled, event->x));
         end_y   = MAX (scrolled->drag_start_vy, GIMV_SCROLLED_VY (scrolled, event->y));

         gimv_zlist_cell_select_by_pixel_region (list,
                                                 start_x, start_y,
                                                 end_x, end_y);

         /* redraw the rubber band */
         WIDGET_DRAW (widget);
      }
   }

   if (list->flags & GIMV_ZLIST_USES_DND) return retval;

   x = event->x;
   y = event->y;

   index = gimv_zlist_cell_index_from_xy (list, x, y);
   if (index < 0) return retval;

   /* was: gdk_pointer_is_grabbed () && GTK_WIDGET_HAS_GRAB (widget) */
   if (list->button_pressed) {

      if (index == list->focus) return retval;

      gimv_zlist_cell_draw_default (list, list->focus);
      list->focus = index;

      switch (list->selection_mode) {
      case GTK_SELECTION_SINGLE:
         gimv_zlist_cell_draw_focus (list, index);
         break;

      case GTK_SELECTION_BROWSE:
         gimv_zlist_unselect_all (list);
         gimv_zlist_cell_select (list, index);
         break;

#if 0
      case GTK_SELECTION_MULTIPLE:
         if (event->state & GDK_CONTROL_MASK) {
            list->anchor = index;
            gimv_zlist_cell_toggle (list, index);
         } else {
            gimv_zlist_extend_selection (list, index);
         }
         break;
#endif

      default:
         break;
      }
   } else {
   }

   return retval;
}


/*
 * GtkContainers doesn't seem to forward the focus movement to
 * containers which don't have child widgets
 *
 */

static gboolean
gimv_zlist_key_press (GimvScrolled *scrolled, GimvEventKey *event)
{
   GtkWidget *widget;
   gint direction = -1;

   g_return_val_if_fail (scrolled && event, FALSE);

   widget = GTK_WIDGET (scrolled);

   if (gtk_widget_has_focus (widget)) {
      switch (event->keyval) {
      case GDK_KEY_Up:
      case GDK_KEY_KP_Up:
         direction = GTK_DIR_UP;
         break;
      case GDK_KEY_Down:
      case GDK_KEY_KP_Down:
         direction = GTK_DIR_DOWN;
         break;
      case GDK_KEY_Left:
      case GDK_KEY_KP_Left:
         direction = GTK_DIR_LEFT;
         break;
      case GDK_KEY_Right:
      case GDK_KEY_KP_Right:
         direction = GTK_DIR_RIGHT;
         break;
      /*
      case GDK_KEY_Tab:
      case GDK_KEY_ISO_Left_Tab:
         if (event->state & GDK_SHIFT_MASK)
            direction = GTK_DIR_TAB_BACKWARD;
         else
            direction = GTK_DIR_TAB_FORWARD;
          break;
      */
      case GDK_KEY_Page_Up:
      case GDK_KEY_KP_Page_Up:
         gimv_scrolled_page_up (scrolled);
         break;
      case GDK_KEY_Page_Down:
      case GDK_KEY_KP_Page_Down:
         gimv_scrolled_page_down (scrolled);
         break;
      default:
         break;
      }
   }

   if (direction != -1) {
      gimv_zlist_focus (GIMV_ZLIST (widget), direction);
      return FALSE;
   }

   if (parent_class_scrolled->key_press &&
       parent_class_scrolled->key_press (scrolled, event))
      return TRUE;

   return FALSE;
}


static void
gimv_zlist_drag_motion (GimvScrolled *scrolled, gint x, gint y)
{
   GimvZList *list;
   gint index;

   g_return_if_fail (scrolled);

   if (parent_class_scrolled->drag_motion)
      parent_class_scrolled->drag_motion (scrolled, x, y);

   list = GIMV_ZLIST(scrolled);

   if (!(list->flags & GIMV_ZLIST_USES_DND))
      return;

   if (!(list->flags & GIMV_ZLIST_HIGHLIGHTED)) {
      list->flags |= GIMV_ZLIST_HIGHLIGHTED;
      WIDGET_DRAW (GTK_WIDGET (list));   /* drawn by gimv_zlist_highlight () */
   }

   index = gimv_zlist_cell_index_from_xy (list, x, y);
   if (index < 0)
      return;
   /*
   if (g_list_find (ZLIST(widget)->selection, GUINT_TO_POINTER(index)))
      return TRUE;
   */
}


static void
gimv_zlist_drag_leave (GimvScrolled *scrolled)
{
   GimvZList *list;

   if (parent_class_scrolled->drag_leave)
      parent_class_scrolled->drag_leave (scrolled);

   list = GIMV_ZLIST(scrolled);
   if (list->flags & GIMV_ZLIST_HIGHLIGHTED) {
      list->flags &= ~GIMV_ZLIST_HIGHLIGHTED;
      gimv_zlist_unhighlight (list);
   }
}


/* was: gtk_paint_shadow (GTK_SHADOW_OUT) + a black frame around the widget */
static void
gimv_zlist_highlight (GimvZList *list, cairo_t *cr)
{
   GtkWidget *widget = GTK_WIDGET (list);
   GdkRGBA color;
   gint width, height;

   width  = gtk_widget_get_width  (widget) - 2 * bw (widget);
   height = gtk_widget_get_height (widget) - 2 * bw (widget);
   if (width <= 0 || height <= 0) return;

   gimv_widget_get_fg_color (widget, &color);

   cairo_save (cr);
   gdk_cairo_set_source_rgba (cr, &color);
   cairo_set_line_width (cr, HIGHLIGHT_SIZE);
   cairo_rectangle (cr,
                    HIGHLIGHT_SIZE / 2.0, HIGHLIGHT_SIZE / 2.0,
                    width  - HIGHLIGHT_SIZE,
                    height - HIGHLIGHT_SIZE);
   cairo_stroke (cr);
   cairo_restore (cr);
}


static void
gimv_zlist_unhighlight (GimvZList *list)
{
   /* the highlight is not drawn any more by the next draw */
   WIDGET_DRAW (GTK_WIDGET (list));
}


static gboolean
gimv_zlist_focus (GimvZList *list, GtkDirectionType dir)
{
   gint   focus;

   g_return_val_if_fail (GIMV_IS_ZLIST (list), FALSE);

   if (list->focus < 0)
      return FALSE;

   focus = list->focus;
   switch (dir) {
   case GTK_DIR_LEFT:
      focus -= list->flags & GIMV_ZLIST_HORIZONTAL ? list->rows : 1;
      break;
   case GTK_DIR_RIGHT:
      focus += list->flags & GIMV_ZLIST_HORIZONTAL ? list->rows : 1;
      break;
   case GTK_DIR_UP:
   case GTK_DIR_TAB_BACKWARD:
      focus -= list->flags & GIMV_ZLIST_HORIZONTAL ? 1 : list->columns;
      break;
   case GTK_DIR_DOWN:
   case GTK_DIR_TAB_FORWARD:
      focus += list->flags & GIMV_ZLIST_HORIZONTAL ? 1 : list->columns;
      break;
   default:
      return FALSE;
      break;
   }

   if (focus < 0 || focus >= list->cell_count)
      return FALSE;

   gimv_zlist_cell_draw_default (list, list->focus);
   list->focus = focus;
   gimv_zlist_cell_draw_focus (list, focus);
   gimv_zlist_moveto (list, focus);

   return TRUE;
}


guint
gimv_zlist_add (GimvZList *list, gpointer cell)
{
   g_return_val_if_fail (GIMV_IS_ZLIST (list), 0);

   return gimv_zlist_insert (list, list->cells->len, cell);
}


guint
gimv_zlist_insert (GimvZList *list, guint pos, gpointer cell)
{
   GtkRequisition requisition = { 0, 0 };
   gint adjust = FALSE;

   g_return_val_if_fail (GIMV_IS_ZLIST (list), 0);
   g_return_val_if_fail (list->cells, 0);

   if (pos > list->cells->len)
      pos = list->cells->len;
   list->cells = g_array_insert_val (list->cells, pos, cell);

   g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_SIZE_REQUEST], 0,
                  cell, &requisition);

   if (list->flags & GIMV_ZLIST_HORIZONTAL) {
      if (list->cell_count && list->cell_count % list->rows == 0) {
         list->columns++;
         adjust = TRUE;
      }
   } else {
      if (list->cell_count && list->cell_count % list->columns == 0) {
         list->rows ++;
         adjust = TRUE;
      }
   }

   list->cell_count++;

   if (requisition.width  + list->cell_x_pad > list->cell_width ||
       requisition.height + list->cell_y_pad > list->cell_height)
   {
      list->cell_width
         = MAX(list->cell_width, requisition.width + list->cell_x_pad);
      list->cell_height
         = MAX(list->cell_height, requisition.height + list->cell_y_pad);

      gimv_zlist_update (list);
      WIDGET_DRAW (GTK_WIDGET (list));

   } else {

      if (adjust)
         gimv_zlist_adjust_adjustments (GIMV_SCROLLED(list));

      gimv_zlist_update (list);
      gimv_zlist_draw_cell (list, list->cell_count - 1);
   }

   return pos;
}


void
gimv_zlist_remove (GimvZList *list, gpointer cell)
{
   GList *item;
   gint index;

   g_return_if_fail (GIMV_IS_ZLIST (list));

   index = gimv_zlist_cell_index (list, cell);
   if (index == -1)
      return;

   list->cells = g_array_remove_index (list->cells, index);
   list->cell_count --;

   list->selection = g_list_remove (list->selection, GUINT_TO_POINTER(index));
   for (item = list->selection; item; item = item->next) {
      gint i = GPOINTER_TO_UINT (item->data);
      if (i > index)
         item->data = GUINT_TO_POINTER (i - 1);
   }

   if (list->focus == index)
      list->focus = -1;
   else if (list->focus > index)
      list->focus --;

   if (list->anchor == index)
      list->anchor = -1;
   else if (list->anchor > index)
      list->anchor --;

   if ((list->flags & GIMV_ZLIST_HORIZONTAL) && list->cell_count % list->rows == 0) {
      list->columns --;
      gimv_zlist_adjust_adjustments (GIMV_SCROLLED(list));
   } else if (!(list->flags & GIMV_ZLIST_HORIZONTAL) && list->cell_count % list->columns == 0) {
      list->rows --;
      gimv_zlist_adjust_adjustments (GIMV_SCROLLED(list));
   }

   gimv_zlist_update (list);

   /* GTK2 redrew the cells after the removed one only */
   WIDGET_DRAW (GTK_WIDGET (list));
}


static void
configure_adjustment (GtkAdjustment *adj, gdouble upper, gdouble page,
                      gdouble step, gdouble pad)
{
   if (page < 0) page = 0;

   /* don't scroll only for the padding after the last cell */
   if (upper > page && upper - page <= pad)
      upper = page;
   upper = MAX (upper, page);

   gtk_adjustment_configure (adj,
                             gtk_adjustment_get_value (adj),
                             0,             /* lower */
                             upper,
                             MAX (step, 1), /* step increment */
                             page,          /* page increment */
                             page);         /* page size */
}


static void
gimv_zlist_adjust_adjustments (GimvScrolled *scrolled)
{
   GimvZList *list;
   GtkWidget *widget;

   list = GIMV_ZLIST(scrolled);
   widget = GTK_WIDGET(scrolled);

   if (scrolled->freeze_count)
      return;

   if (scrolled->h_adjustment)
      configure_adjustment (scrolled->h_adjustment,
                            LIST_WIDTH(list) + 2 * list->x_pad + list->cell_x_pad,
                            gtk_widget_get_width (widget) - 2 * bw (widget),
                            list->cell_width,
                            list->cell_x_pad);

   if (scrolled->v_adjustment)
      configure_adjustment (scrolled->v_adjustment,
                            LIST_HEIGHT(list) + 2 * list->y_pad + list->cell_y_pad,
                            gtk_widget_get_height (widget) - 2 * bw (widget),
                            list->cell_height,
                            list->cell_y_pad);
}


void
gimv_zlist_clear (GimvZList *list)
{
   GimvScrolled *scrolled;

   g_return_if_fail (GIMV_IS_ZLIST (list));

   gimv_zlist_unselect_all (list);

   g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CLEAR], 0);

   if (list->cells)
      g_array_set_size (list->cells, 0);
   list->cell_count     = 0;
   list->focus          = -1;
   list->anchor         = -1;
   list->entered_cell   = NULL;

   if (list->flags & GIMV_ZLIST_HORIZONTAL)
      list->columns = 1;
   else
      list->rows = 1;

   scrolled = GIMV_SCROLLED(list);
   gimv_zlist_adjust_adjustments (scrolled);
   if (scrolled->h_adjustment)
      gtk_adjustment_set_value (scrolled->h_adjustment, 0);
   if (scrolled->v_adjustment)
      gtk_adjustment_set_value (scrolled->v_adjustment, 0);

   WIDGET_DRAW (GTK_WIDGET (list));
}


void
gimv_zlist_set_cell_padding (GimvZList *list, gint x_pad, gint y_pad)
{
   g_return_if_fail (list);
   g_return_if_fail (x_pad >= 0 && y_pad >= 0);

   list->cell_width  += x_pad - list->cell_x_pad;
   list->cell_height += y_pad - list->cell_y_pad;
   list->cell_width  = MAX (list->cell_width,  1);
   list->cell_height = MAX (list->cell_height, 1);

   list->cell_x_pad = x_pad;
   list->cell_y_pad = y_pad;

   gimv_zlist_update (list);
   WIDGET_DRAW (GTK_WIDGET (list));
}


void
gimv_zlist_set_cell_size (GimvZList *list, gint width, gint height)
{
   g_return_if_fail (GIMV_IS_ZLIST (list));

   if (width > 0)
      list->cell_width  = width  + list->cell_x_pad;

   if (height > 0)
      list->cell_height = height + list->cell_y_pad;

   gimv_zlist_update (list);
   WIDGET_DRAW (GTK_WIDGET (list));
}


void
gimv_zlist_set_selection_mode (GimvZList *list, GtkSelectionMode mode)
{
   g_return_if_fail (GIMV_IS_ZLIST (list));

   list->selection_mode = mode;
   gimv_zlist_unselect_all (list);
   WIDGET_DRAW (GTK_WIDGET (list));
}


gint
gimv_zlist_cell_index_from_xy (GimvZList *list, gint x, gint y)
{
   gint row, column, cell_x, cell_y, index;

   g_return_val_if_fail (GIMV_IS_ZLIST (list), -1);

   if (!list->cell_count || x < list->x_pad || y < list->y_pad)
      return -1;

   /* (the pixel is left of/above the first column/row) */
   if (GIMV_SCROLLED_VX (list, x - list->x_pad - bw (list)) < 0
       || GIMV_SCROLLED_VY (list, y - list->y_pad - bw (list)) < 0)
   {
      return -1;
   }

   row    = CELL_ROW_FROM_Y(list, y);
   column = CELL_COL_FROM_X(list, x);

   if (row >= list->rows || column >= list->columns)
      return -1;

   cell_x = CELL_X_FROM_COL(list, column);
   if (x < list->cell_x_pad * 2 + cell_x)
      return -1;

   cell_y = CELL_Y_FROM_ROW(list, row);
   if (y < list->cell_y_pad * 2 + cell_y)
      return -1;

   index = list->flags & GIMV_ZLIST_HORIZONTAL ? column * list->rows + row : row * list->columns + column;

   return index < list->cell_count ? index : -1;
}


void
gimv_zlist_set_1 (GimvZList *list, gint one)
{
   g_return_if_fail (list);

   if (one)
      list->flags |= GIMV_ZLIST_1;
   else
      list->flags &= ~GIMV_ZLIST_1;

   gimv_zlist_update (list);
   WIDGET_DRAW (GTK_WIDGET (list));
}


gpointer
gimv_zlist_cell_from_xy (GimvZList *list, gint x, gint y)
{
   gint index;

   index = gimv_zlist_cell_index_from_xy (list, x, y);
   return index < 0 ? NULL : GIMV_ZLIST_CELL_FROM_INDEX (list, index);
}


static void
gimv_zlist_cell_pos (GimvZList *list, gint index, gint *row, gint *col)
{
   g_return_if_fail (list && index != -1 && row && col);

   if (list->flags & GIMV_ZLIST_HORIZONTAL) {
      *row = index % list->rows;
      *col = index / list->rows;
   } else {
      *row = index / list->columns;
      *col = index % list->columns;
   }
}


static void
gimv_zlist_cell_area (GimvZList *list, gint index, GdkRectangle *cell_area)
{
   gint row, col;
   g_return_if_fail (list && index != -1 && cell_area);

   gimv_zlist_cell_pos (list, index, &row, &col);

   cell_area->x      = CELL_X_FROM_COL(list, col) + list->cell_x_pad;
   cell_area->y      = CELL_Y_FROM_ROW(list, row) + list->cell_y_pad;
   cell_area->width  = list->cell_width - list->cell_x_pad;
   cell_area->height = list->cell_height - list->cell_y_pad;

}


/*
 *  GTK4: queues a redraw of the cell (painting happens in the draw method)
 */
void
gimv_zlist_draw_cell (GimvZList *list, gint index)
{
   g_return_if_fail (list && index != -1);

   if (!gtk_widget_is_drawable (GTK_WIDGET (list))
       || GIMV_SCROLLED (list)->freeze_count)
   {
      return;
   }

   WIDGET_DRAW (GTK_WIDGET (list));
}


gint
gimv_zlist_cell_index (GimvZList *list, gpointer cell)
{
   gint i;
   g_return_val_if_fail (list && cell, -1);

   for (i = 0; i < list->cell_count; i++)
      if (GIMV_ZLIST_CELL_FROM_INDEX (list, i) == cell)
         return i;

   return -1;
}


gint
gimv_zlist_update_cell_size (GimvZList *list, gpointer cell)
{
   GtkRequisition requisition = { 0, 0 };

   g_return_val_if_fail (list && cell, FALSE);

   if (list->flags & GIMV_ZLIST_1)
      return FALSE;

   g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_SIZE_REQUEST], 0,
                  cell, &requisition);

   if (requisition.width  + list->cell_x_pad > list->cell_width ||
       requisition.height + list->cell_y_pad > list->cell_height) {
      list->cell_width  = MAX(list->cell_width,  requisition.width  + list->cell_x_pad);
      list->cell_height = MAX(list->cell_height, requisition.height + list->cell_y_pad);

      gimv_zlist_update (list);
      WIDGET_DRAW (GTK_WIDGET (list));

      return TRUE;
   }

   return FALSE;
}


/*
 *
 * Focus & Selection handling
 *
 */

/* GTK4: the focus decoration is painted by gimv_zlist_draw () */
static void
gimv_zlist_cell_draw_focus (GimvZList *list, gint index)
{
   g_return_if_fail (list && index != -1);

   gimv_zlist_draw_cell (list, index);
}


/* GTK4: the default decoration is painted by gimv_zlist_draw_list () */
static void
gimv_zlist_cell_draw_default (GimvZList *list, gint index)
{
   g_return_if_fail (list);
   if (index < 0) return;
   /* g_return_if_fail (index != -1); */

   gimv_zlist_draw_cell (list, index);
}


void
gimv_zlist_cell_select (GimvZList *list, gint index)
{
   GList *node;

   g_return_if_fail (GIMV_IS_ZLIST (list) && index != -1);

   node = g_list_find (list->selection, GUINT_TO_POINTER(index));
   if (!node) {
      g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_SELECT], 0, index);
      list->selection = g_list_prepend (list->selection, GUINT_TO_POINTER(index));
      gimv_zlist_draw_cell (list, index);
   }
}


void
gimv_zlist_cell_unselect (GimvZList *list, gint index)
{
   GList *node;

   g_return_if_fail (GIMV_IS_ZLIST (list) && index != -1);

   node = g_list_find (list->selection, GUINT_TO_POINTER(index));
   if (node) {
      g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_UNSELECT], 0, index);
      list->selection = g_list_remove (list->selection, GUINT_TO_POINTER(index));
      gimv_zlist_draw_cell (list, index);
   }
}


void
gimv_zlist_cell_toggle (GimvZList *list, gint index)
{
   g_return_if_fail (GIMV_IS_ZLIST (list) && index != -1);

   if (g_list_find (list->selection, GUINT_TO_POINTER(index)))
      gimv_zlist_cell_unselect (list, index);
   else
      gimv_zlist_cell_select (list, index);
}


void
gimv_zlist_unselect_all (GimvZList *list)
{
   GList *item;

   g_return_if_fail (GIMV_IS_ZLIST (list));

   item = list->selection;
   while (item) {
      g_signal_emit (G_OBJECT(list), gimv_zlist_signals [CELL_UNSELECT], 0,
                     GPOINTER_TO_UINT(item->data));
      gimv_zlist_draw_cell (list, GPOINTER_TO_UINT(item->data));

      item = item->next;
   }

   g_list_free (list->selection);
   list->selection = NULL;
}


void
gimv_zlist_extend_selection (GimvZList *list, gint to)
{
   GList *item;
   gint i, s, e;

   g_return_if_fail (list && to != -1);

   if (list->anchor < to) {
      s = list->anchor;
      e = to;
   } else {
      s = to;
      e = list->anchor;
   }
   if (s < 0) s = 0;

   for (i = s; i <= e; i++)
      /* XXX the state of the cell should be cached in the cells array */
      if (!g_list_find (list->selection, GUINT_TO_POINTER(i)))
         gimv_zlist_cell_select (list, i);
      else if (i == list->focus)
         gimv_zlist_cell_draw_focus (list, i);

   item = list->selection;
   while (item) {
      i = GPOINTER_TO_UINT(item->data);
      item = item->next;

      if (i < s || i > e)
         gimv_zlist_cell_unselect (list, i);
   }
}


static void
select_each_cell_by_region (GimvZList *list, gint index, gboolean in_region)
{
   gboolean select = FALSE, selected = FALSE;
   gboolean is_mask = FALSE;

   g_return_if_fail (index < list->cell_count);

   if (g_list_find (list->selection, GUINT_TO_POINTER (index)))
      selected = TRUE;

   if (g_list_find (list->selection_mask, GUINT_TO_POINTER (index)))
      is_mask = TRUE;

   switch (list->region_select) {
   case GIMV_ZLIST_REGION_SELECT_TOGGLE:
      if ((in_region && !is_mask) || (!in_region && is_mask))
         select = TRUE;
      break;

   case GIMV_ZLIST_REGION_SELECT_EXPAND:
      if (in_region || is_mask)
         select = TRUE;
      break;

   case GIMV_ZLIST_REGION_SELECT_NORMAL:
      if (in_region)
         select = TRUE;
      break;
   default:
      break;
   }

   if (select && !selected)
      gimv_zlist_cell_select (list, index);
   if (!select && selected)
      gimv_zlist_cell_unselect (list, index);
}


void
gimv_zlist_cell_select_by_region (GimvZList *list,
                                  guint start_col, guint start_row,
                                  guint end_col, guint end_row)
{
   gint index = 0, i, j, cols, rows;
   guint col, row;

   g_return_if_fail (list);
   g_return_if_fail (GIMV_IS_ZLIST (list));
   g_return_if_fail (end_col >= start_col);
   g_return_if_fail (end_row >= start_row);

   if (list->flags & GIMV_ZLIST_HORIZONTAL) {
      cols = list->rows;
      rows = list->columns;
   } else {
      cols = list->columns;
      rows = list->rows;
   }

   for (i = 0; i < rows; i++) {
      for (j = 0; j < cols; j++) {
         gboolean in_region = FALSE;

         if (list->flags & GIMV_ZLIST_HORIZONTAL) {
            index = list->rows * i + j;
         } else {
            index = list->columns * i + j;
         }
         if (index >= list->cell_count) break;

         if (list->flags & GIMV_ZLIST_HORIZONTAL) {
            col = i;
            row = j;
         } else {
            col = j;
            row = i;
         }

         if (row >= start_row && row <= end_row && col >= start_col && col <= end_col)
            in_region = TRUE;

         select_each_cell_by_region (list, index, in_region);
      }

      if (index >= list->cell_count) break;
   }
}


void
gimv_zlist_cell_select_by_pixel_region (GimvZList *list,
                                        gint start_x, gint start_y,
                                        gint end_x, gint end_y)
{
   gint start_col, start_row, end_col, end_row, cell_x, cell_y;

   g_return_if_fail (list);
   g_return_if_fail (GIMV_IS_ZLIST (list));
   g_return_if_fail (end_x >= start_x);
   g_return_if_fail (end_y >= start_y);

   start_x = start_x - list->x_pad - bw (list);
   if (start_x < 0) start_x = 0;
   start_y = start_y - list->y_pad - bw (list);
   if (start_y < 0) start_y = 0;

   end_x = end_x - list->x_pad - bw (list);
   if (end_x < 0) end_x = 0;
   end_y = end_y - list->y_pad - bw (list);
   if (end_y < 0) end_y = 0;

   start_col = start_x / list->cell_width;
   start_row = start_y / list->cell_height;
   end_col   = end_x   / list->cell_width;
   end_row   = end_y   / list->cell_height;

   /*
   cell_x = CELL_X_FROM_COL(list, start_col);
   if (start_x  < list->cell_x_pad * 2 + cell_x && start_col > 1)
      start_col--;
      cell_y = CELL_Y_FROM_ROW(list, start_row);
   if (start_y < list->cell_y_pad * 2 + cell_y && start_row > 1)
      start_row--;
   */

   cell_x = CELL_X_FROM_COL(list, end_col);

   if (GIMV_SCROLLED_X (GIMV_SCROLLED (list), end_x) < list->cell_x_pad * 2 + cell_x
       && end_col > 0 && end_col > start_col)
   {
      end_col--;
   }

   cell_y = CELL_Y_FROM_ROW(list, end_row);

   if (GIMV_SCROLLED_Y (GIMV_SCROLLED (list), end_y) < list->cell_y_pad * 2 + cell_y
       && end_row > 0 && end_row > start_row)
   {
      end_row--;
   }

   gimv_zlist_cell_select_by_region (list,
                                     start_col, start_row,
                                     end_col, end_row);
}


void
gimv_zlist_set_selection_mask (GimvZList *list, GList *mask_list)
{
   g_return_if_fail (list);
   g_return_if_fail (GIMV_IS_ZLIST (list));

   if (list->selection_mask && list->selection_mask != mask_list)
      g_list_free (list->selection_mask);

   if (mask_list) {
      list->selection_mask = mask_list;
   } else {
      if (list->selection)
         list->selection_mask = g_list_copy (list->selection);
      else
         list->selection_mask = NULL;
   }
}


void
gimv_zlist_unset_selection_mask (GimvZList *list)
{
   g_return_if_fail (list);
   g_return_if_fail (GIMV_IS_ZLIST (list));

   if (list->selection_mask)
      g_list_free (list->selection_mask);
   list->selection_mask = NULL;
}


static void
adjustment_add_value (GtkAdjustment *adj, gdouble delta)
{
   gdouble value, lower, upper;

   if (!adj) return;

   lower = gtk_adjustment_get_lower (adj);
   upper = gtk_adjustment_get_upper (adj) - gtk_adjustment_get_page_size (adj);
   value = gtk_adjustment_get_value (adj) + delta;
   value = MAX (lower, MIN (value, upper));
   gtk_adjustment_set_value (adj, value);
}


void
gimv_zlist_moveto (GimvZList *list, gint index)
{
   GdkRectangle cell_area;
   GtkAdjustment *adj;

   g_return_if_fail (list && index != -1);
   g_return_if_fail (index < list->cell_count);

   if (!gtk_widget_is_drawable (GTK_WIDGET (list)) || GIMV_SCROLLED(list)->freeze_count)
      return;

   gimv_zlist_cell_area (list, index, &cell_area);

   if (list->flags & GIMV_ZLIST_HORIZONTAL) { /* horizontal list */

      adj = GIMV_SCROLLED(list)->h_adjustment;
      if (!adj) return;

      if (cell_area.x < 0) {
         adjustment_add_value (adj, cell_area.x);
      } else if (cell_area.x + cell_area.width > gtk_widget_get_width (GTK_WIDGET (list))) {
         adjustment_add_value (adj,
                               (cell_area.x - gtk_adjustment_get_page_size (adj))
                               + cell_area.width);
      }

   } else { /* vertical list */

      adj = GIMV_SCROLLED(list)->v_adjustment;
      if (!adj) return;

      if (cell_area.y < 0) {
         adjustment_add_value (adj, cell_area.y);
      } else if (cell_area.y + cell_area.height > gtk_widget_get_height (GTK_WIDGET (list))) {
         adjustment_add_value (adj,
                               (cell_area.y - gtk_adjustment_get_page_size (adj))
                               + cell_area.height);
      }
   }
}


void
gimv_zlist_cell_set_focus (GimvZList *list, gint index)
{
   g_return_if_fail (GIMV_IS_ZLIST (list));
   g_return_if_fail (index >= 0 && index < list->cell_count);

   list->focus = index;
   gimv_zlist_cell_draw_focus (list, list->focus);
}


void
gimv_zlist_cell_unset_focus (GimvZList *list)
{
   gint focus;

   g_return_if_fail (GIMV_IS_ZLIST (list));

   focus = list->focus;
   list->focus = -1;

   if (focus >= 0 && focus < list->cell_count)
      gimv_zlist_cell_draw_default (list, focus);
}
