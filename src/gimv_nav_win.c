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
 * $Id: gimv_nav_win.c,v 1.4 2004/03/07 11:53:31 makeinu Exp $
 */

/*
 * These codes are mostly taken from gThumb.
 * gThumb code Copyright (C) 2001 The Free Software Foundation, Inc.
 * gThumb author: Paolo Bacchilega
 */

#include <math.h>
#include <string.h>

#include "gimageview.h"

#include "gimv_nav_win.h"
#include "gtk2-compat.h"


#define PEN_WIDTH         3       /* Square border width. */ 
#define BORDER_WIDTH      4       /* Window border width. */


enum {
   MOVE_SIGNAL,
   LAST_SIGNAL
};


static guint gimv_nav_win_signals[LAST_SIGNAL] = {0};


/* object class */
static void     gimv_nav_win_dispose        (GObject         *object);

/* event handlers (class handlers of GTK2) */
static gboolean gimv_nav_win_key_press      (GtkWidget       *widget,
                                             GimvEventKey    *event,
                                             gpointer         data);
static gboolean gimv_nav_win_button_release (GtkWidget       *widget,
                                             GimvEventButton *event,
                                             gpointer         data);
static gboolean gimv_nav_win_motion_notify  (GtkWidget       *widget,
                                             GimvEventMotion *event,
                                             gpointer         data);
static void     gimv_nav_win_preview_draw   (GtkDrawingArea  *area,
                                             cairo_t         *cr,
                                             gint             width,
                                             gint             height,
                                             gpointer         data);
/* nav_win class */
static void      navwin_draw_sqr            (GimvNavWin      *navwin,
                                             gboolean         undraw,
                                             gint             x,
                                             gint             y);
static void      get_sqr_origin_as_double   (GimvNavWin      *navwin,
                                             gint             mx,
                                             gint             my,
                                             gdouble         *x,
                                             gdouble         *y);
static void      navwin_update_view         (GimvNavWin      *navwin);
static void      navwin_draw                (GimvNavWin      *navwin);
static void      navwin_grab_pointer        (GimvNavWin      *navwin);
static void      navwin_set_win_pos_size    (GimvNavWin *navwin);


G_DEFINE_TYPE (GimvNavWin, gimv_nav_win, GTK_TYPE_BOX)


static void
gimv_nav_win_class_init (GimvNavWinClass *klass)
{
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

   gobject_class->dispose = gimv_nav_win_dispose;

   gimv_nav_win_signals[MOVE_SIGNAL]
      = g_signal_new ("move",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_FIRST,
                      G_STRUCT_OFFSET (GimvNavWinClass, move),
                      NULL, NULL,
                      NULL,
                      G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}


static void
gimv_nav_win_init (GimvNavWin *navwin)
{
   navwin->fix_x_pos = navwin->fix_y_pos = 1;

   navwin->pixmap       = NULL;
   navwin->mask         = NULL;

   /* GTK2 created it as a GTK_WINDOW_POPUP, see gimv_nav_win.h */
   gtk_widget_set_halign (GTK_WIDGET (navwin), GTK_ALIGN_START);
   gtk_widget_set_valign (GTK_WIDGET (navwin), GTK_ALIGN_START);
   gtk_widget_set_focusable (GTK_WIDGET (navwin), TRUE);
   gtk_widget_set_visible (GTK_WIDGET (navwin), FALSE);
   gtk_widget_add_css_class (GTK_WIDGET (navwin), "gimv-nav-win");
   gtk_widget_set_cursor_from_name (GTK_WIDGET (navwin), "crosshair");

   navwin->out_frame = gtk_frame_new (NULL);
   gimv_container_add (GTK_WIDGET (navwin), navwin->out_frame);
   gtk_widget_show (navwin->out_frame);

   navwin->in_frame = gtk_frame_new (NULL);
   gimv_container_add (GTK_WIDGET (navwin->out_frame), navwin->in_frame);
   gtk_widget_show (navwin->in_frame);

   navwin->preview = gtk_drawing_area_new ();
   gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (navwin->preview),
                                   gimv_nav_win_preview_draw,
                                   navwin, NULL);
   gimv_container_add (GTK_WIDGET (navwin->in_frame), navwin->preview);
   gtk_widget_show (navwin->preview);

   /* GTK2 class handlers: run after the handlers connected by users */
   gimv_event_connect_after (GTK_WIDGET (navwin), GIMV_EVENT_KEY_PRESS,
                             G_CALLBACK (gimv_nav_win_key_press), NULL);
   gimv_event_connect_after (GTK_WIDGET (navwin), GIMV_EVENT_BUTTON_RELEASE,
                             G_CALLBACK (gimv_nav_win_button_release), NULL);
   gimv_event_connect_after (GTK_WIDGET (navwin), GIMV_EVENT_MOTION_NOTIFY,
                             G_CALLBACK (gimv_nav_win_motion_notify), NULL);
}


static void
gimv_nav_win_dispose (GObject *object)
{
   GimvNavWin *navwin = GIMV_NAV_WIN (object);

   if (navwin->pixmap)
      g_object_unref (navwin->pixmap);
   navwin->pixmap = NULL;

   if (navwin->mask)
      g_object_unref (navwin->mask);
   navwin->mask = NULL;

   G_OBJECT_CLASS (gimv_nav_win_parent_class)->dispose (object);
}


static void
gimv_nav_win_preview_draw (GtkDrawingArea *area,
                           cairo_t *cr,
                           gint width,
                           gint height,
                           gpointer data)
{
   GimvNavWin *navwin = data;

   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   if (navwin->pixmap)
      gimv_cairo_draw_texture (cr, navwin->pixmap, 0, 0);

   /* the square (GTK2 drew it with a GDK_INVERT GC) */
   if ((navwin->sqr_x == 0)
       && (navwin->sqr_y == 0)
       && (navwin->sqr_width == navwin->popup_width)
       && (navwin->sqr_height == navwin->popup_height))
      return;

   cairo_save (cr);
   cairo_set_operator (cr, CAIRO_OPERATOR_DIFFERENCE);
   cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
   cairo_set_line_width (cr, PEN_WIDTH);
   cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
   cairo_set_line_join (cr, CAIRO_LINE_JOIN_MITER);
   /* GDK draws lines centered on the pixel grid ((x, y) is the line center) */
   cairo_rectangle (cr,
                    navwin->sqr_x + 1 + 0.5,
                    navwin->sqr_y + 1 + 0.5,
                    navwin->sqr_width - PEN_WIDTH,
                    navwin->sqr_height - PEN_WIDTH);
   cairo_stroke (cr);
   cairo_restore (cr);
}


static gboolean
gimv_nav_win_key_press (GtkWidget *widget,
                        GimvEventKey *event,
                        gpointer data)
{
   GimvNavWin *navwin;
   gboolean move = FALSE;
   gint mx, my;
   guint keyval;

   g_return_val_if_fail (GIMV_IS_NAV_WIN (widget), FALSE);

   navwin = GIMV_NAV_WIN (widget);

   mx = navwin->view_pos_x;
   my = navwin->view_pos_y;

   keyval = event->keyval;

   if (keyval == GDK_KEY_Left) {
      mx -= 10;
      move = TRUE;
   } else if (keyval == GDK_KEY_Right) {
      mx += 10;
      move = TRUE;
   } else if (keyval == GDK_KEY_Up) {
      my -= 10;
      move = TRUE;
   } else if (keyval == GDK_KEY_Down) {
      my += 10;
      move = TRUE;
   } else if (keyval == GDK_KEY_Escape) {
      /* GTK4: the pointer can't be grabbed, give a way to close the popup */
      gimv_nav_win_hide (navwin);
      return TRUE;
   }

   if (move) {
      if (navwin->fix_x_pos < 0) mx = navwin->fix_x_pos;
      if (navwin->fix_y_pos < 0) my = navwin->fix_y_pos;
      navwin->view_pos_x = mx;
      navwin->view_pos_y = my;
      g_signal_emit (G_OBJECT (navwin),
                     gimv_nav_win_signals[MOVE_SIGNAL], 0,
                     mx, my);
      mx *= navwin->factor;
      my *= navwin->factor;
      navwin_draw_sqr (navwin, TRUE, mx, my);
   }

   return FALSE;
}


static gboolean
gimv_nav_win_button_release  (GtkWidget *widget,
                              GimvEventButton *event,
                              gpointer data)
{
   GimvNavWin *navwin;

   g_return_val_if_fail (GIMV_IS_NAV_WIN (widget), FALSE);

   navwin = GIMV_NAV_WIN (widget);

   switch (event->button) {
   case 1:
      gimv_nav_win_hide (navwin);
      /* gimv_widget_destroy (GTK_WIDGET (navwin)); */
      return TRUE;
      break;
   default:
      break;
   }

   return FALSE;
}


static gboolean
gimv_nav_win_motion_notify (GtkWidget *widget,
                            GimvEventMotion *event,
                            gpointer data)
{
   GimvNavWin *navwin;
   gint mx, my;
   gdouble x, y, px, py;

   g_return_val_if_fail (GIMV_IS_NAV_WIN (widget), FALSE);

   navwin = GIMV_NAV_WIN (widget);

   /* GTK2 used window coordinates which are BORDER_WIDTH off the preview */
   if (gtk_widget_translate_coordinates (widget, navwin->preview,
                                         event->x, event->y, &px, &py))
   {
      mx = (gint) px + BORDER_WIDTH;
      my = (gint) py + BORDER_WIDTH;
   } else {
      mx = (gint) event->x;
      my = (gint) event->y;
   }
   get_sqr_origin_as_double (navwin, mx, my, &x, &y);

   mx = (gint) x;
   my = (gint) y;
   navwin_draw_sqr (navwin, TRUE, mx, my);

   mx = (gint) (x / navwin->factor);
   my = (gint) (y / navwin->factor);
   if (navwin->fix_x_pos < 0) mx = navwin->fix_x_pos;
   if (navwin->fix_y_pos < 0) my = navwin->fix_y_pos;

   g_signal_emit (G_OBJECT (navwin),
                  gimv_nav_win_signals[MOVE_SIGNAL], 0,
                  mx, my);

   return FALSE;
}


GtkWidget *
gimv_nav_win_new (GdkTexture *pixmap, GdkTexture *mask,
                  gint image_width, gint image_height,
                  gint view_width, gint view_height,
                  gint fpos_x, gint fpos_y)
{
   GimvNavWin *navwin;

   g_return_val_if_fail (pixmap, NULL);

   navwin = GIMV_NAV_WIN (g_object_new (GIMV_TYPE_NAV_WIN, NULL));

   navwin->pixmap = g_object_ref (pixmap);
   if (mask) navwin->mask = g_object_ref (mask);

   navwin->x_root = 0;
   navwin->y_root = 0;

   navwin->image_width  = image_width;
   navwin->image_height = image_height;

   navwin->view_width   = view_width;
   navwin->view_height  = view_height;
   navwin->view_pos_x   = fpos_x;
   navwin->view_pos_y   = fpos_y;

   navwin_update_view (navwin);

   return GTK_WIDGET (navwin);
}


void
gimv_nav_win_show (GimvNavWin *navwin, gint x, gint y)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   navwin->x_root = x;
   navwin->y_root = y;

   navwin_update_view (navwin);
   navwin_set_win_pos_size (navwin);

   gtk_widget_set_visible (GTK_WIDGET (navwin), TRUE);
   navwin_grab_pointer (navwin);
}


void
gimv_nav_win_hide (GimvNavWin *navwin)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   gtk_widget_set_visible (GTK_WIDGET (navwin), FALSE);
}


/*
 *  Pointer events of the button press which opened the navigator (the
 *  pointer is grabbed by the widget which got the press).  x, y are in the
 *  coordinates of the parent (the overlay).
 */
void
gimv_nav_win_pointer_motion (GimvNavWin *navwin, gdouble x, gdouble y)
{
   GimvEventMotion ev;
   GtkWidget *parent;
   graphene_point_t p, q;

   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   if (!gtk_widget_get_visible (GTK_WIDGET (navwin))) return;

   parent = gtk_widget_get_parent (GTK_WIDGET (navwin));
   if (!parent) return;

   graphene_point_init (&p, x, y);
   if (!gtk_widget_compute_point (parent, GTK_WIDGET (navwin), &p, &q))
      return;

   memset (&ev, 0, sizeof (ev));
   ev.type = GDK_MOTION_NOTIFY;
   ev.x = ev.x_root = q.x;
   ev.y = ev.y_root = q.y;

   gimv_nav_win_motion_notify (GTK_WIDGET (navwin), &ev, NULL);
}


void
gimv_nav_win_pointer_release (GimvNavWin *navwin, guint button)
{
   GimvEventButton ev;

   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   if (!gtk_widget_get_visible (GTK_WIDGET (navwin))) return;

   memset (&ev, 0, sizeof (ev));
   ev.type = GIMV_BUTTON_RELEASE;
   ev.button = button;

   gimv_nav_win_button_release (GTK_WIDGET (navwin), &ev, NULL);
}


void
gimv_nav_win_set_pixmap (GimvNavWin *navwin,
                         GdkTexture *pixmap, GdkTexture *mask,
                         gint image_width, gint image_height)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));
   g_return_if_fail (pixmap);

   if (navwin->pixmap)
      g_object_unref (navwin->pixmap);
   navwin->pixmap = NULL;
   if (navwin->mask)
      g_object_unref (navwin->mask);
   navwin->mask   = NULL;

   navwin->pixmap = g_object_ref (pixmap);
   if (mask) navwin->mask = g_object_ref (mask);

   navwin->image_width  = image_width;
   navwin->image_height = image_height;

   if (gtk_widget_get_mapped (GTK_WIDGET (navwin))) {
      navwin_update_view (navwin);
      navwin_set_win_pos_size (navwin);
      navwin_draw (navwin);
   }
}


void
gimv_nav_win_set_orig_image_size (GimvNavWin *navwin,
                                  gint width, gint height)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   navwin->image_width  = width;
   navwin->image_height = height;

   if (gtk_widget_get_mapped (GTK_WIDGET (navwin))) {
      navwin_update_view (navwin);
      navwin_draw (navwin);
   }
}


void
gimv_nav_win_set_view_size (GimvNavWin *navwin,
                            gint width, gint height)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   navwin->view_width  = width;
   navwin->view_height = height;

   if (gtk_widget_get_mapped (GTK_WIDGET (navwin))) {
      navwin_update_view (navwin);
      navwin_draw (navwin);
   }
}


void
gimv_nav_win_set_view_position (GimvNavWin *navwin,
                                gint x, gint y)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   navwin->view_pos_x = x;
   navwin->view_pos_y = y;

   if (gtk_widget_get_mapped (GTK_WIDGET (navwin))) {
      navwin_update_view (navwin);
      navwin_draw (navwin);
   }
}



/******************************************************************************
 *
 *   Private functions.
 *
 ******************************************************************************/
static void
navwin_draw_sqr (GimvNavWin *navwin,
                 gboolean undraw,
                 gint x,
                 gint y)
{
   if ((navwin->sqr_x == x) && (navwin->sqr_y == y) && undraw)
      return;

   /* the square is drawn by the draw function of the preview */
   navwin->sqr_x = x;
   navwin->sqr_y = y;

   gtk_widget_queue_draw (navwin->preview);
}


static void
get_sqr_origin_as_double (GimvNavWin *navwin,
                          gint mx,
                          gint my,
                          gdouble *x,
                          gdouble *y)
{
   *x = MIN (mx - BORDER_WIDTH, GIMV_NAV_WIN_SIZE);
   *y = MIN (my - BORDER_WIDTH, GIMV_NAV_WIN_SIZE);

   if (*x - navwin->sqr_width / 2.0 < 0.0) 
      *x = navwin->sqr_width / 2.0;
	
   if (*y - navwin->sqr_height / 2.0 < 0.0)
      *y = navwin->sqr_height / 2.0;

   if (*x + navwin->sqr_width / 2.0 > navwin->popup_width - 0)
      *x = navwin->popup_width - 0 - navwin->sqr_width / 2.0;

   if (*y + navwin->sqr_height / 2.0 > navwin->popup_height - 0)
      *y = navwin->popup_height - 0 - navwin->sqr_height / 2.0;

   *x = *x - navwin->sqr_width / 2.0;
   *y = *y - navwin->sqr_height / 2.0;
}


static void
navwin_update_view (GimvNavWin *navwin)
{
   gint popup_x, popup_y;
   gint popup_width, popup_height;
   gint w, h, x_pos, y_pos;
   gint screen_width, screen_height;
   gdouble factor;

   w = navwin->image_width;
   h = navwin->image_height;

   factor = MIN ((gdouble) (GIMV_NAV_WIN_SIZE) / w, 
                 (gdouble) (GIMV_NAV_WIN_SIZE) / h);
   navwin->factor = factor;

   /* Popup window size. */
   popup_width  = MAX ((gint) floor (factor * w + 0.5), 1);
   popup_height = MAX ((gint) floor (factor * h + 0.5), 1);

   gtk_widget_set_size_request (navwin->preview,
                                popup_width,
                                popup_height);

   /* The square. */
   x_pos = navwin->view_pos_x;
   y_pos = navwin->view_pos_y;

   navwin->sqr_width = navwin->view_width * factor;
   navwin->sqr_width = MAX (navwin->sqr_width, BORDER_WIDTH);
   navwin->sqr_width = MIN (navwin->sqr_width, popup_width);

   navwin->sqr_height = navwin->view_height * factor;
   navwin->sqr_height = MAX (navwin->sqr_height, BORDER_WIDTH); 
   navwin->sqr_height = MIN (navwin->sqr_height, popup_height); 

   navwin->sqr_x = x_pos * factor;
   if (navwin->sqr_x < 0) navwin->sqr_x = 0;
   navwin->sqr_y = y_pos * factor;
   if (navwin->sqr_y < 0) navwin->sqr_y = 0;

   /* fix x (or y) if image is smaller than frame */
   if (navwin->view_width  > navwin->image_width) 
      navwin->fix_x_pos = x_pos;
   else
      navwin->fix_x_pos = 1;
   if (navwin->view_height > navwin->image_height)
      navwin->fix_y_pos = y_pos;
   else
      navwin->fix_y_pos = 1;

   /* Popup window position (GTK4: inside the parent, not the screen). */
   if (gtk_widget_get_parent (GTK_WIDGET (navwin))) {
      GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (navwin));
      screen_width  = gtk_widget_get_width (parent);
      screen_height = gtk_widget_get_height (parent);
   } else {
      gimv_screen_get_size (&screen_width, &screen_height);
   }
   popup_x = MIN (navwin->x_root - navwin->sqr_x 
                  - BORDER_WIDTH 
                  - navwin->sqr_width / 2,
                  screen_width - popup_width - BORDER_WIDTH * 2);
   popup_y = MIN (navwin->y_root - navwin->sqr_y 
                  - BORDER_WIDTH
                  - navwin->sqr_height / 2,
                  screen_height - popup_height - BORDER_WIDTH * 2);
   popup_x = MAX (popup_x, 0);
   popup_y = MAX (popup_y, 0);

   navwin->popup_x      = popup_x;
   navwin->popup_y      = popup_y;
   navwin->popup_width  = popup_width;
   navwin->popup_height = popup_height;

   navwin->view_pos_x   = x_pos;
   navwin->view_pos_y   = y_pos;

   gtk_widget_queue_draw (navwin->preview);
}


static void
navwin_grab_pointer (GimvNavWin *navwin)
{
   /* GTK4: no explicit pointer grab (see gimv_nav_win_pointer_motion ()).
      Capture keyboard events. */
   gtk_widget_grab_focus (GTK_WIDGET (navwin));
}


static void
navwin_draw (GimvNavWin *navwin)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   gtk_widget_queue_draw (navwin->preview);
}


static void
navwin_set_win_pos_size (GimvNavWin *navwin)
{
   g_return_if_fail (GIMV_IS_NAV_WIN (navwin));

   /* placed in the overlay by its margins */
   gtk_widget_set_margin_start (GTK_WIDGET (navwin), navwin->popup_x);
   gtk_widget_set_margin_top   (GTK_WIDGET (navwin), navwin->popup_y);
}
