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
 * $Id: gimv_scrolled.c,v 1.3 2004/09/21 08:44:32 makeinu Exp $
 */

/*
 *  These codes are mostly taken from Another X image viewer.
 *
 *  Another X image viewer Author:
 *     David Ramboz <dramboz@users.sourceforge.net>
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gimv_scrolled.h"

/* for auto-scroll and auto-expand at drag */
#define AUTO_SCROLL_TIMEOUT    100
#define AUTO_SCROLL_EDGE_WIDTH 20
#define AUTO_SCROLL_SCALE_RATE 32 /* times / pixel */

enum {
   ADJUST_ADJUSTMENTS,
   LAST_SIGNAL
};

enum {
   PROP_0,
   PROP_HADJUSTMENT,
   PROP_VADJUSTMENT,
   PROP_HSCROLL_POLICY,
   PROP_VSCROLL_POLICY
};

static void     gimv_scrolled_dispose          (GObject         *object);
static void     gimv_scrolled_set_property     (GObject         *object,
                                                guint            prop_id,
                                                const GValue    *value,
                                                GParamSpec      *pspec);
static void     gimv_scrolled_get_property     (GObject         *object,
                                                guint            prop_id,
                                                GValue          *value,
                                                GParamSpec      *pspec);
static void     gimv_scrolled_snapshot         (GtkWidget       *widget,
                                                GtkSnapshot     *snapshot);
static void     set_hadjustment                (GimvScrolled    *scrolled,
                                                GtkAdjustment   *adj);
static void     set_vadjustment                (GimvScrolled    *scrolled,
                                                GtkAdjustment   *adj);
static gboolean cb_button_press                (GtkWidget       *widget,
                                                GimvEventButton *event,
                                                gpointer         data);
static gboolean cb_button_release              (GtkWidget       *widget,
                                                GimvEventButton *event,
                                                gpointer         data);
static gboolean cb_motion_notify               (GtkWidget       *widget,
                                                GimvEventMotion *event,
                                                gpointer         data);
static gboolean cb_key_press                   (GtkWidget       *widget,
                                                GimvEventKey    *event,
                                                gpointer         data);
static gboolean cb_focus_in                    (GtkWidget       *widget,
                                                gpointer         event,
                                                gpointer         data);
static gboolean cb_focus_out                   (GtkWidget       *widget,
                                                gpointer         event,
                                                gpointer         data);
static void     cb_drop_motion                 (GtkDropControllerMotion *controller,
                                                gdouble          x,
                                                gdouble          y,
                                                gpointer         data);
static void     cb_drop_leave                  (GtkDropControllerMotion *controller,
                                                gpointer         data);

static void     cancel_auto_scroll             (GimvScrolled    *scrolled);
static void     setup_drag_scroll              (GimvScrolled    *scrolled,
                                                gint             x,
                                                gint             y);

static guint gimv_scrolled_signals [LAST_SIGNAL] = {0};


G_DEFINE_TYPE_WITH_CODE (GimvScrolled, gimv_scrolled, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, NULL))


static void
gimv_scrolled_class_init (GimvScrolledClass *klass)
{
   GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
   GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

   gobject_class->dispose      = gimv_scrolled_dispose;
   gobject_class->set_property = gimv_scrolled_set_property;
   gobject_class->get_property = gimv_scrolled_get_property;

   widget_class->snapshot      = gimv_scrolled_snapshot;

   g_object_class_override_property (gobject_class, PROP_HADJUSTMENT,    "hadjustment");
   g_object_class_override_property (gobject_class, PROP_VADJUSTMENT,    "vadjustment");
   g_object_class_override_property (gobject_class, PROP_HSCROLL_POLICY, "hscroll-policy");
   g_object_class_override_property (gobject_class, PROP_VSCROLL_POLICY, "vscroll-policy");

   gimv_scrolled_signals[ADJUST_ADJUSTMENTS] =
      g_signal_new ("adjust_adjustments",
                    G_TYPE_FROM_CLASS (klass),
                    G_SIGNAL_RUN_FIRST,
                    G_STRUCT_OFFSET (GimvScrolledClass, adjust_adjustments),
                    NULL, NULL,
                    g_cclosure_marshal_VOID__VOID,
                    G_TYPE_NONE, 0);

   klass->adjust_adjustments = NULL;
   klass->draw               = NULL;
   klass->button_press       = NULL;
   klass->button_release     = NULL;
   klass->motion_notify      = NULL;
   klass->key_press          = NULL;
   klass->focus_in           = NULL;
   klass->focus_out          = NULL;
   klass->drag_motion        = NULL;
   klass->drag_leave         = NULL;
}


static void
gimv_scrolled_init (GimvScrolled *scrolled)
{
   GtkWidget *widget = GTK_WIDGET (scrolled);
   GtkEventController *drop_motion;

   scrolled->x_offset         = 0;
   scrolled->y_offset         = 0;
   scrolled->h_adjustment     = NULL;
   scrolled->v_adjustment     = NULL;
   scrolled->hscroll_policy   = GTK_SCROLL_MINIMUM;
   scrolled->vscroll_policy   = GTK_SCROLL_MINIMUM;
   scrolled->freeze_count     = 0;

   /* for auto scroll */
   scrolled->autoscroll_flags = 0;
   scrolled->scroll_edge_x    = AUTO_SCROLL_EDGE_WIDTH;
   scrolled->scroll_edge_y    = AUTO_SCROLL_EDGE_WIDTH;
   scrolled->x_step           = -1;
   scrolled->y_step           = -1;
   scrolled->x_interval       = AUTO_SCROLL_TIMEOUT;
   scrolled->y_interval       = AUTO_SCROLL_TIMEOUT;
   scrolled->pressed          = FALSE;
   scrolled->drag_start_vx    = -1;
   scrolled->drag_start_vy    = -1;
   scrolled->drag_motion_x    = -1;
   scrolled->drag_motion_y    = -1;
   scrolled->step_scale       = 0.0;
   scrolled->hscroll_timer_id = -1;
   scrolled->vscroll_timer_id = -1;

   gtk_widget_set_focusable (widget, TRUE);
   gtk_widget_set_overflow (widget, GTK_OVERFLOW_HIDDEN);

   /* the class methods run after handlers connected by users, as the
      class handlers of GTK2's event signals did */
   gimv_event_connect_after (widget, GIMV_EVENT_BUTTON_PRESS,
                             G_CALLBACK (cb_button_press), NULL);
   gimv_event_connect_after (widget, GIMV_EVENT_BUTTON_RELEASE,
                             G_CALLBACK (cb_button_release), NULL);
   gimv_event_connect_after (widget, GIMV_EVENT_MOTION_NOTIFY,
                             G_CALLBACK (cb_motion_notify), NULL);
   gimv_event_connect_after (widget, GIMV_EVENT_KEY_PRESS,
                             G_CALLBACK (cb_key_press), NULL);
   gimv_event_connect_after (widget, GIMV_EVENT_FOCUS_IN,
                             G_CALLBACK (cb_focus_in), NULL);
   gimv_event_connect_after (widget, GIMV_EVENT_FOCUS_OUT,
                             G_CALLBACK (cb_focus_out), NULL);

   drop_motion = gtk_drop_controller_motion_new ();
   g_signal_connect (drop_motion, "enter",  G_CALLBACK (cb_drop_motion), scrolled);
   g_signal_connect (drop_motion, "motion", G_CALLBACK (cb_drop_motion), scrolled);
   g_signal_connect (drop_motion, "leave",  G_CALLBACK (cb_drop_leave),  scrolled);
   gtk_widget_add_controller (widget, drop_motion);
}


static void
gimv_scrolled_dispose (GObject *object)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (object);

   cancel_auto_scroll (scrolled);
   set_hadjustment (scrolled, NULL);
   set_vadjustment (scrolled, NULL);

   G_OBJECT_CLASS (gimv_scrolled_parent_class)->dispose (object);
}


static void
gimv_scrolled_set_property (GObject *object, guint prop_id,
                            const GValue *value, GParamSpec *pspec)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (object);

   switch (prop_id) {
   case PROP_HADJUSTMENT:
      set_hadjustment (scrolled, g_value_get_object (value));
      break;
   case PROP_VADJUSTMENT:
      set_vadjustment (scrolled, g_value_get_object (value));
      break;
   case PROP_HSCROLL_POLICY:
      scrolled->hscroll_policy = g_value_get_enum (value);
      gtk_widget_queue_resize (GTK_WIDGET (scrolled));
      break;
   case PROP_VSCROLL_POLICY:
      scrolled->vscroll_policy = g_value_get_enum (value);
      gtk_widget_queue_resize (GTK_WIDGET (scrolled));
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
   }
}


static void
gimv_scrolled_get_property (GObject *object, guint prop_id,
                            GValue *value, GParamSpec *pspec)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (object);

   switch (prop_id) {
   case PROP_HADJUSTMENT:
      g_value_set_object (value, scrolled->h_adjustment);
      break;
   case PROP_VADJUSTMENT:
      g_value_set_object (value, scrolled->v_adjustment);
      break;
   case PROP_HSCROLL_POLICY:
      g_value_set_enum (value, scrolled->hscroll_policy);
      break;
   case PROP_VSCROLL_POLICY:
      g_value_set_enum (value, scrolled->vscroll_policy);
      break;
   default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
   }
}


static void
gimv_scrolled_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);
   GdkRectangle area;
   cairo_t *cr;

   if (!klass->draw) return;

   area.x = 0;
   area.y = 0;
   area.width  = gtk_widget_get_width (widget);
   area.height = gtk_widget_get_height (widget);
   if (area.width <= 0 || area.height <= 0) return;

   cr = gtk_snapshot_append_cairo (snapshot,
                                   &GRAPHENE_RECT_INIT (0, 0, area.width, area.height));
   klass->draw (GIMV_SCROLLED (widget), cr, &area);
   cairo_destroy (cr);
}


void
gimv_scrolled_realize (GimvScrolled *scrolled)
{
   /* nothing to do in GTK4 (was: create the GC for scrolling) */
}


void
gimv_scrolled_unrealize (GimvScrolled *scrolled)
{
}


static void
hadjustment_value_changed (GtkAdjustment *hadjustment, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (data);
   gint value = gtk_adjustment_get_value (hadjustment);

   if (value < 0) value = 0;
   if (scrolled->x_offset == value) return;

   scrolled->x_offset = value;
   if (!scrolled->freeze_count)
      gtk_widget_queue_draw (GTK_WIDGET (scrolled));
}


static void
vadjustment_value_changed (GtkAdjustment *vadjustment, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (data);
   gint value = gtk_adjustment_get_value (vadjustment);

   if (value < 0) value = 0;
   if (scrolled->y_offset == value) return;

   scrolled->y_offset = value;
   if (!scrolled->freeze_count)
      gtk_widget_queue_draw (GTK_WIDGET (scrolled));
}


static void
set_hadjustment (GimvScrolled *scrolled, GtkAdjustment *adj)
{
   if (scrolled->h_adjustment == adj && adj) return;

   if (scrolled->h_adjustment) {
      g_signal_handlers_disconnect_by_data (scrolled->h_adjustment, scrolled);
      g_object_unref (scrolled->h_adjustment);
      scrolled->h_adjustment = NULL;
   }

   if (!adj) {
      if (gtk_widget_in_destruction (GTK_WIDGET (scrolled))) return;
      adj = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
   }

   scrolled->h_adjustment = g_object_ref_sink (adj);
   g_signal_connect (adj, "value-changed",
                     G_CALLBACK (hadjustment_value_changed), scrolled);

   gimv_scrolled_adjust_adjustments (scrolled);
   g_object_notify (G_OBJECT (scrolled), "hadjustment");
}


static void
set_vadjustment (GimvScrolled *scrolled, GtkAdjustment *adj)
{
   if (scrolled->v_adjustment == adj && adj) return;

   if (scrolled->v_adjustment) {
      g_signal_handlers_disconnect_by_data (scrolled->v_adjustment, scrolled);
      g_object_unref (scrolled->v_adjustment);
      scrolled->v_adjustment = NULL;
   }

   if (!adj) {
      if (gtk_widget_in_destruction (GTK_WIDGET (scrolled))) return;
      adj = gtk_adjustment_new (0, 0, 0, 0, 0, 0);
   }

   scrolled->v_adjustment = g_object_ref_sink (adj);
   g_signal_connect (adj, "value-changed",
                     G_CALLBACK (vadjustment_value_changed), scrolled);

   gimv_scrolled_adjust_adjustments (scrolled);
   g_object_notify (G_OBJECT (scrolled), "vadjustment");
}


/*
 *  Ask the subclass to update the adjustments (bounds, page size) from the
 *  current allocation and content size.
 */
void
gimv_scrolled_adjust_adjustments (GimvScrolled *scrolled)
{
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));

   if (!scrolled->h_adjustment || !scrolled->v_adjustment) return;
   if (scrolled->freeze_count) return;

   g_signal_emit (scrolled, gimv_scrolled_signals[ADJUST_ADJUSTMENTS], 0);
}


void
gimv_scrolled_freeze (GimvScrolled *scrolled)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (scrolled->freeze_count != (guint) -1);

   scrolled->freeze_count ++;
}


void
gimv_scrolled_thawn (GimvScrolled *scrolled)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (scrolled->freeze_count);

   scrolled->freeze_count --;
   if (!scrolled->freeze_count) {
      gimv_scrolled_adjust_adjustments (scrolled);
      gtk_widget_queue_draw (GTK_WIDGET(scrolled));
   }
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
gimv_scrolled_page_up (GimvScrolled *scrolled)
{
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));
   adjustment_add_value (scrolled->v_adjustment,
                         -gtk_adjustment_get_page_size (scrolled->v_adjustment));
}


void
gimv_scrolled_page_down (GimvScrolled *scrolled)
{
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));
   adjustment_add_value (scrolled->v_adjustment,
                         gtk_adjustment_get_page_size (scrolled->v_adjustment));
}


void
gimv_scrolled_page_left (GimvScrolled *scrolled)
{
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));
   adjustment_add_value (scrolled->h_adjustment,
                         -gtk_adjustment_get_page_size (scrolled->h_adjustment));
}


void
gimv_scrolled_page_right (GimvScrolled *scrolled)
{
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));
   adjustment_add_value (scrolled->h_adjustment,
                         gtk_adjustment_get_page_size (scrolled->h_adjustment));
}



/******************************************************************************
 *
 *   input
 *
 ******************************************************************************/
static gboolean
cb_button_press (GtkWidget *widget, GimvEventButton *event, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (widget);
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);

   scrolled->pressed = TRUE;
   scrolled->drag_start_vx = GIMV_SCROLLED_VX (scrolled, (gint) event->x);
   scrolled->drag_start_vy = GIMV_SCROLLED_VY (scrolled, (gint) event->y);

   if (klass->button_press)
      return klass->button_press (scrolled, event);

   return FALSE;
}


static gboolean
cb_button_release (GtkWidget *widget, GimvEventButton *event, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (widget);
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);
   gboolean retval = FALSE;

   if (klass->button_release)
      retval = klass->button_release (scrolled, event);

   gimv_scrolled_stop_auto_scroll (scrolled);

   return retval;
}


static gboolean
cb_motion_notify (GtkWidget *widget, GimvEventMotion *event, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (widget);
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);
   gint flags = scrolled->autoscroll_flags;

   scrolled->drag_motion_x = event->x;
   scrolled->drag_motion_y = event->y;

   if ((scrolled->pressed && (flags & GIMV_SCROLLED_AUTO_SCROLL_MOTION))
       || (flags & GIMV_SCROLLED_AUTO_SCROLL_MOTION_ALL))
   {
      setup_drag_scroll (scrolled, (gint) event->x, (gint) event->y);
   }

   if (klass->motion_notify)
      return klass->motion_notify (scrolled, event);

   return FALSE;
}


static gboolean
cb_key_press (GtkWidget *widget, GimvEventKey *event, gpointer data)
{
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);

   if (klass->key_press)
      return klass->key_press (GIMV_SCROLLED (widget), event);

   return FALSE;
}


static gboolean
cb_focus_in (GtkWidget *widget, gpointer event, gpointer data)
{
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);

   if (klass->focus_in)
      klass->focus_in (GIMV_SCROLLED (widget));
   gtk_widget_queue_draw (widget);

   return FALSE;
}


static gboolean
cb_focus_out (GtkWidget *widget, gpointer event, gpointer data)
{
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (widget);

   if (klass->focus_out)
      klass->focus_out (GIMV_SCROLLED (widget));
   gtk_widget_queue_draw (widget);

   return FALSE;
}


static void
cb_drop_motion (GtkDropControllerMotion *controller, gdouble x, gdouble y,
                gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (data);
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (scrolled);

   if (scrolled->autoscroll_flags & GIMV_SCROLLED_AUTO_SCROLL_DND)
      setup_drag_scroll (scrolled, x, y);

   if (klass->drag_motion)
      klass->drag_motion (scrolled, x, y);
}


static void
cb_drop_leave (GtkDropControllerMotion *controller, gpointer data)
{
   GimvScrolled *scrolled = GIMV_SCROLLED (data);
   GimvScrolledClass *klass = GIMV_SCROLLED_GET_CLASS (scrolled);

   cancel_auto_scroll (scrolled);

   if (klass->drag_leave)
      klass->drag_leave (scrolled);
}



/******************************************************************************
 *
 *   for auto scroll related functions
 *
 ******************************************************************************/
enum
{
   HORIZONTAL,
   VERTICAL
};


static void
cancel_auto_scroll (GimvScrolled *scrolled)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED (scrolled));

   /* horizontal */
   if (scrolled->hscroll_timer_id != -1){
      g_source_remove (scrolled->hscroll_timer_id);
      scrolled->hscroll_timer_id = -1;
   }

   /* vertival */
   if (scrolled->vscroll_timer_id != -1){
      g_source_remove (scrolled->vscroll_timer_id);
      scrolled->vscroll_timer_id = -1;
   }
}


static gboolean
scrolling_is_desirable (GimvScrolled *scrolled, gint direction,
                        gint x, gint y, gfloat *scale)
{
   GtkAdjustment *adj;
   gint pos, edge_width, flags;
   gdouble upper, page_size;

   flags = scrolled->autoscroll_flags;

   if (direction == HORIZONTAL) {
      if (!(flags & GIMV_SCROLLED_AUTO_SCROLL_HORIZONTAL)
          && !(flags & GIMV_SCROLLED_AUTO_SCROLL_BOTH))
      {
         return FALSE;
      }

      adj = scrolled->h_adjustment;
      pos = x;
      edge_width = scrolled->scroll_edge_x;
   } else if (direction == VERTICAL) {
      if (!(flags & GIMV_SCROLLED_AUTO_SCROLL_VERTICAL)
          && !(flags & GIMV_SCROLLED_AUTO_SCROLL_BOTH))
      {
         return FALSE;
      }

      adj = scrolled->v_adjustment;
      pos = y;
      edge_width = scrolled->scroll_edge_y;
   } else {
      return FALSE;
   }

   if (!adj) return FALSE;

   page_size = gtk_adjustment_get_page_size (adj);
   upper = gtk_adjustment_get_upper (adj) - page_size;

   if ((pos < edge_width)
       && (gtk_adjustment_get_value (adj) > gtk_adjustment_get_lower (adj)))
   {
      if (scale)
         *scale = 1 + (0 - pos) / AUTO_SCROLL_SCALE_RATE;
      return TRUE;
   } else if ((pos > (page_size - edge_width))
              && (gtk_adjustment_get_value (adj) < upper))
   {
      if (scale)
         *scale = 1 + (pos - page_size) / AUTO_SCROLL_SCALE_RATE;
      return TRUE;
   }

   return FALSE;
}


static gboolean
vertical_timeout (gpointer data)
{
   GimvScrolled *scrolled = data;
   GtkAdjustment *vadj;
   gint step;

   vadj = scrolled->v_adjustment;

   if (scrolled->y_step < 0)
      step = gtk_adjustment_get_step_increment (vadj);
   else
      step = scrolled->y_step;

   step *= scrolled->step_scale;

   if (scrolled->drag_motion_y < scrolled->scroll_edge_y)
      adjustment_add_value (vadj, -step);
   else
      adjustment_add_value (vadj, step);

   return TRUE;
}


static gboolean
horizontal_timeout (gpointer data)
{
   GimvScrolled *scrolled = data;
   GtkAdjustment *hadj;
   gint step;

   hadj = scrolled->h_adjustment;

   if (scrolled->x_step < 0)
      step = gtk_adjustment_get_step_increment (hadj);
   else
      step = scrolled->x_step;

   step *= scrolled->step_scale;

   if (scrolled->drag_motion_x < scrolled->scroll_edge_x)
      adjustment_add_value (hadj, -step);
   else
      adjustment_add_value (hadj, step);

   return TRUE;
}


static void
setup_drag_scroll (GimvScrolled *scrolled, gint x, gint y)
{
   gboolean desirable;

   cancel_auto_scroll (scrolled);

   scrolled->drag_motion_x = x;
   scrolled->drag_motion_y = y;

   /* horizonal */
   desirable = scrolling_is_desirable (scrolled, HORIZONTAL,
                                       x, y, &scrolled->step_scale);

   if (desirable)
      scrolled->hscroll_timer_id
         = g_timeout_add (scrolled->x_interval,
                          horizontal_timeout, scrolled);

   /* vertical */
   desirable = scrolling_is_desirable (scrolled, VERTICAL,
                                       x, y, &scrolled->step_scale);

   if (desirable)
      scrolled->vscroll_timer_id
         = g_timeout_add (scrolled->y_interval,
                          vertical_timeout, scrolled);
}


void
gimv_scrolled_set_auto_scroll (GimvScrolled *scrolled,
                               GimvScrolledAutoScrollFlags flags)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   scrolled->autoscroll_flags |= flags;
}


void
gimv_scrolled_unset_auto_scroll (GimvScrolled *scrolled)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   scrolled->autoscroll_flags = 0;
}


void
gimv_scrolled_set_auto_scroll_edge_width (GimvScrolled *scrolled,
                                          gint x_edge,
                                          gint y_edge)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   if (x_edge < 0)
      scrolled->scroll_edge_x = AUTO_SCROLL_EDGE_WIDTH;
   else
      scrolled->scroll_edge_x = x_edge;

   if (y_edge < 0)
      scrolled->scroll_edge_y = AUTO_SCROLL_EDGE_WIDTH;
   else
      scrolled->scroll_edge_y = y_edge;
}


void
gimv_scrolled_set_h_auto_scroll_resolution (GimvScrolled *scrolled,
                                            gint step,
                                            gint interval)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   scrolled->x_step = step;
   if (interval <= 0)
      scrolled->x_interval = AUTO_SCROLL_TIMEOUT;
   else
      scrolled->x_interval = interval;
}


void
gimv_scrolled_set_v_auto_scroll_resolution (GimvScrolled *scrolled,
                                            gint step,
                                            gint interval)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   scrolled->y_step = step;
   if (interval <= 0)
      scrolled->y_interval = AUTO_SCROLL_TIMEOUT;
   else
      scrolled->y_interval = interval;
}


gboolean
gimv_scrolled_is_dragging (GimvScrolled *scrolled)
{
   g_return_val_if_fail (scrolled, FALSE);

   if (scrolled->pressed
       && scrolled->drag_motion_x >= 0
       && scrolled->drag_motion_y >= 0)
   {
      return TRUE;
   }

   return FALSE;
}


void
gimv_scrolled_stop_auto_scroll (GimvScrolled *scrolled)
{
   g_return_if_fail (scrolled);
   g_return_if_fail (GIMV_IS_SCROLLED(scrolled));

   cancel_auto_scroll (scrolled);

   scrolled->pressed = FALSE;
   scrolled->drag_start_vx = -1;
   scrolled->drag_start_vy = -1;
   scrolled->drag_motion_x = -1;
   scrolled->drag_motion_y = -1;
}
