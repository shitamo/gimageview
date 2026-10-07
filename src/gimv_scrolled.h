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
 * $Id: gimv_scrolled.h,v 1.3 2003/07/05 16:41:11 makeinu Exp $
 */

/*
 *  These codes are mostly taken from Another X image viewer.
 *
 *  Another X image viewer Author:
 *     David Ramboz <dramboz@users.sourceforge.net>
 */

#ifndef _GIMV_SCROLLED_H_
#define _GIMV_SCROLLED_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include "gimv_gtk4_compat.h"

/*
 *  GTK4 port:
 *
 *  GimvScrolled is a GtkWidget implementing GtkScrollable.  It keeps the
 *  scroll offsets in sync with its adjustments and routes input to class
 *  methods, so that subclasses (GimvZList, GimvZAlbum) do not have to deal
 *  with GTK4 event controllers themselves.  Painting is done by the "draw"
 *  class method with a cairo context in widget coordinates.
 */

#define GIMV_TYPE_SCROLLED            gimv_scrolled_get_type ()
#define GIMV_SCROLLED(obj)            G_TYPE_CHECK_INSTANCE_CAST (obj, GIMV_TYPE_SCROLLED, GimvScrolled)
#define GIMV_SCROLLED_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GIMV_TYPE_SCROLLED, GimvScrolledClass))
#define GIMV_SCROLLED_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GIMV_TYPE_SCROLLED, GimvScrolledClass))
#define GIMV_IS_SCROLLED(obj)         G_TYPE_CHECK_INSTANCE_TYPE (obj, GIMV_TYPE_SCROLLED)
#define GIMV_SCROLLED_X(scrolled, x)  (-GIMV_SCROLLED(scrolled)->x_offset + (x))
#define GIMV_SCROLLED_Y(scrolled, y)  (-GIMV_SCROLLED(scrolled)->y_offset + (y))
#define GIMV_SCROLLED_VX(scrolled, x) (GIMV_SCROLLED(scrolled)->x_offset + (x))
#define GIMV_SCROLLED_VY(scrolled, y) (GIMV_SCROLLED(scrolled)->y_offset + (y))


typedef struct _GimvScrolled GimvScrolled;
typedef struct _GimvScrolledClass GimvScrolledClass;

typedef enum
{
   GIMV_SCROLLED_AUTO_SCROLL_NONE       = 0,
   GIMV_SCROLLED_AUTO_SCROLL_HORIZONTAL = 1 << 0,
   GIMV_SCROLLED_AUTO_SCROLL_VERTICAL   = 1 << 1,
   GIMV_SCROLLED_AUTO_SCROLL_BOTH       = 1 << 2,

   GIMV_SCROLLED_AUTO_SCROLL_DND        = 1 << 3, /* for drag motion event */
   GIMV_SCROLLED_AUTO_SCROLL_MOTION     = 1 << 4, /* for motion notify event */
   GIMV_SCROLLED_AUTO_SCROLL_MOTION_ALL = 1 << 5, /* do not check whether button
                                                     was pressed or not */
} GimvScrolledAutoScrollFlags;

struct _GimvScrolled {
   GtkWidget      parent;

   gint           x_offset;
   gint           y_offset;

   GtkAdjustment *h_adjustment;
   GtkAdjustment *v_adjustment;
   guint          hscroll_policy : 1;
   guint          vscroll_policy : 1;

   guint          freeze_count;

   /* for auto scroll */
   gint          autoscroll_flags;
   gint          scroll_edge_x, scroll_edge_y;
   gint          x_step, y_step;
   gint          x_interval, y_interval;
   gboolean      pressed;
   gint          drag_start_vx;    /* virtual x of drag start point */
   gint          drag_start_vy;    /* virtual y of drag start point */
   gint          drag_motion_x;    /* current real x in the window */
   gint          drag_motion_y;    /* current real y in the window */
   gfloat        step_scale;
   gint          hscroll_timer_id;
   gint          vscroll_timer_id;
};


struct _GimvScrolledClass {
   GtkWidgetClass parent_class;

   /* signal */
   void     (*adjust_adjustments) (GimvScrolled    *scrolled);

   /* painting (widget coordinates) */
   void     (*draw)               (GimvScrolled    *scrolled,
                                   cairo_t         *cr,
                                   GdkRectangle    *area);

   /* input; return TRUE to stop further processing */
   gboolean (*button_press)       (GimvScrolled    *scrolled,
                                   GimvEventButton *event);
   gboolean (*button_release)     (GimvScrolled    *scrolled,
                                   GimvEventButton *event);
   gboolean (*motion_notify)      (GimvScrolled    *scrolled,
                                   GimvEventMotion *event);
   gboolean (*key_press)          (GimvScrolled    *scrolled,
                                   GimvEventKey    *event);
   void     (*focus_in)           (GimvScrolled    *scrolled);
   void     (*focus_out)          (GimvScrolled    *scrolled);
   void     (*drag_motion)        (GimvScrolled    *scrolled,
                                   gint             x,
                                   gint             y);
   void     (*drag_leave)         (GimvScrolled    *scrolled);
};


GType      gimv_scrolled_get_type (void);

void       gimv_scrolled_realize                      (GimvScrolled *scrolled);
void       gimv_scrolled_unrealize                    (GimvScrolled *scrolled);
void       gimv_scrolled_freeze                       (GimvScrolled *scrolled);
void       gimv_scrolled_thawn                        (GimvScrolled *scrolled);
void       gimv_scrolled_adjust_adjustments           (GimvScrolled *scrolled);
void       gimv_scrolled_page_up                      (GimvScrolled *scrolled);
void       gimv_scrolled_page_down                    (GimvScrolled *scrolled);
void       gimv_scrolled_page_left                    (GimvScrolled *scrolled);
void       gimv_scrolled_page_right                   (GimvScrolled *scrolled);
void       gimv_scrolled_set_auto_scroll              (GimvScrolled *scrolled,
                                                       GimvScrolledAutoScrollFlags flags);
void       gimv_scrolled_unset_auto_scroll            (GimvScrolled *scrolled);
void       gimv_scrolled_set_auto_scroll_edge_width   (GimvScrolled *scrolled,
                                                       gint      x_edge,
                                                       gint      y_edge); /* -1 to use default value */
void       gimv_scrolled_set_h_auto_scroll_resolution (GimvScrolled *scrolled,
                                                       gint      step,
                                                       gint      interval);
void       gimv_scrolled_set_v_auto_scroll_resolution (GimvScrolled *scrolled,
                                                       gint      step,
                                                       gint      interval);
gboolean   gimv_scrolled_is_dragging                  (GimvScrolled *scrolled);
void       gimv_scrolled_stop_auto_scroll             (GimvScrolled *scrolled);

#endif /* _GIMV_SCROLLED_H_ */
