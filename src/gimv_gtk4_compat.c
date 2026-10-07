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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "gimv_gtk4_compat.h"


/******************************************************************************
 *
 *   main loop
 *
 ******************************************************************************/
static GSList *main_loops = NULL;


void
gimv_main (void)
{
   GMainLoop *loop = g_main_loop_new (NULL, FALSE);

   main_loops = g_slist_prepend (main_loops, loop);
   g_main_loop_run (loop);
   main_loops = g_slist_remove (main_loops, loop);
   g_main_loop_unref (loop);
}


void
gimv_main_quit (void)
{
   if (main_loops)
      g_main_loop_quit (main_loops->data);
}


guint
gimv_main_level (void)
{
   return g_slist_length (main_loops);
}


/*
 *  Handle the pending events while a long job (loading thumbnails or an
 *  image) runs in a callback.  Background jobs that run in idle callbacks
 *  (e.g. reading the image sizes of the detail view) check
 *  gimv_flush_events_running () and wait until the long job has returned
 *  to the main loop: otherwise they would run here, and the job that
 *  flushes (e.g. loading the image clicked for the preview) would wait
 *  until they had finished.
 */
static gint flush_depth = 0;

void
gimv_flush_events (void)
{
   gint i;

   flush_depth++;
   /* limit iterations so that a continuously-firing idle cannot hang us */
   for (i = 0; i < 1000 && g_main_context_pending (NULL); i++)
      g_main_context_iteration (NULL, FALSE);
   flush_depth--;
}


gboolean
gimv_flush_events_running (void)
{
   return flush_depth > 0;
}



/******************************************************************************
 *
 *   containers & packing
 *
 ******************************************************************************/
#define PACK_END_KEY  "gimv-pack-end"
#define SPACER_KEY    "gimv-box-spacer"
#define ALIGN_KEY     "gimv-alignment"


GtkWidget *
gimv_hbox_new (gboolean homogeneous, gint spacing)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, spacing);
   gtk_box_set_homogeneous (GTK_BOX (box), homogeneous);
   return box;
}


GtkWidget *
gimv_vbox_new (gboolean homogeneous, gint spacing)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, spacing);
   gtk_box_set_homogeneous (GTK_BOX (box), homogeneous);
   return box;
}


static void
set_pack_props (GtkBox *box, GtkWidget *child,
                gboolean expand, gboolean fill, guint padding)
{
   GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));
   /* GTK2: children of homogeneous boxes get extra space as if expanded */
   gboolean centered = (expand || gtk_box_get_homogeneous (box)) && !fill;

   /* GTK2 separators drew a line in the middle of their allocation, GTK4
      ones fill it */
   if (GTK_IS_SEPARATOR (child)) {
      if (gtk_orientable_get_orientation (GTK_ORIENTABLE (child))
          == GTK_ORIENTATION_HORIZONTAL)
         gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
      else
         gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
   }

   if (orient == GTK_ORIENTATION_HORIZONTAL) {
      gtk_widget_set_hexpand (child, expand);
      if (centered)
         gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
      if (padding > 0) {
         gtk_widget_set_margin_start (child, padding);
         gtk_widget_set_margin_end (child, padding);
      }
   } else {
      gtk_widget_set_vexpand (child, expand);
      if (centered)
         gtk_widget_set_valign (child, GTK_ALIGN_CENTER);
      if (padding > 0) {
         gtk_widget_set_margin_top (child, padding);
         gtk_widget_set_margin_bottom (child, padding);
      }
   }
}


static GtkWidget *
box_last_start_child (GtkBox *box)
{
   GtkWidget *child, *last = NULL;

   for (child = gtk_widget_get_first_child (GTK_WIDGET (box));
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      if (g_object_get_data (G_OBJECT (child), PACK_END_KEY)) break;
      last = child;
   }

   return last;
}


static gboolean
box_has_expanding_start_child (GtkBox *box)
{
   GtkWidget *child;
   GtkOrientation orient = gtk_orientable_get_orientation (GTK_ORIENTABLE (box));

   for (child = gtk_widget_get_first_child (GTK_WIDGET (box));
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      if (g_object_get_data (G_OBJECT (child), PACK_END_KEY)) break;
      if (orient == GTK_ORIENTATION_HORIZONTAL
          ? gtk_widget_get_hexpand (child) : gtk_widget_get_vexpand (child))
         return TRUE;
   }

   return FALSE;
}


static void
box_update_spacer (GtkBox *box)
{
   GtkWidget *spacer = g_object_get_data (G_OBJECT (box), SPACER_KEY);
   gboolean expand;

   if (!spacer) return;

   expand = !box_has_expanding_start_child (box);
   if (gtk_orientable_get_orientation (GTK_ORIENTABLE (box))
       == GTK_ORIENTATION_HORIZONTAL)
   {
      gtk_widget_set_hexpand (spacer, expand);
   } else {
      gtk_widget_set_vexpand (spacer, expand);
   }
}


void
gimv_box_pack_start (GtkBox *box, GtkWidget *child,
                     gboolean expand, gboolean fill, guint padding)
{
   g_return_if_fail (GTK_IS_BOX (box));
   g_return_if_fail (GTK_IS_WIDGET (child));

   set_pack_props (box, child, expand, fill, padding);
   gtk_box_insert_child_after (box, child, box_last_start_child (box));
   box_update_spacer (box);
}


static void
cb_spacer_destroy (GtkWidget *spacer, GtkBox *box)
{
   g_object_set_data (G_OBJECT (box), SPACER_KEY, NULL);
}


void
gimv_box_pack_end (GtkBox *box, GtkWidget *child,
                   gboolean expand, gboolean fill, guint padding)
{
   GtkWidget *spacer;

   g_return_if_fail (GTK_IS_BOX (box));
   g_return_if_fail (GTK_IS_WIDGET (child));

   set_pack_props (box, child, expand, fill, padding);
   g_object_set_data (G_OBJECT (child), PACK_END_KEY, GINT_TO_POINTER (1));

   spacer = g_object_get_data (G_OBJECT (box), SPACER_KEY);
   if (!spacer && !gtk_box_get_homogeneous (box)) {
      spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_widget_add_css_class (spacer, "gimv-box-spacer");
      g_object_set_data (G_OBJECT (spacer), PACK_END_KEY, GINT_TO_POINTER (1));
      g_object_set_data (G_OBJECT (box), SPACER_KEY, spacer);
      g_signal_connect (spacer, "destroy", G_CALLBACK (cb_spacer_destroy), box);
      gtk_box_insert_child_after (box, spacer, box_last_start_child (box));
   }

   if (spacer)
      gtk_box_insert_child_after (box, child, spacer);
   else
      gtk_box_insert_child_after (box, child, box_last_start_child (box));

   box_update_spacer (box);
}


void
gimv_box_reorder_child (GtkBox *box, GtkWidget *child, gint position)
{
   GtkWidget *sibling = NULL, *c;
   gint i = 0;

   g_return_if_fail (GTK_IS_BOX (box));

   if (position < 0) {
      /* move to the end of the start group */
      gtk_box_reorder_child_after (box, child, box_last_start_child (box));
      return;
   }

   for (c = gtk_widget_get_first_child (GTK_WIDGET (box));
        c && i < position;
        c = gtk_widget_get_next_sibling (c))
   {
      if (c == child) continue;
      sibling = c;
      i++;
   }

   gtk_box_reorder_child_after (box, child, sibling);
}


typedef struct {
   gfloat xalign, yalign, xscale, yscale;
} AlignParams;


static void
apply_alignment (GtkWidget *align, GtkWidget *child)
{
   AlignParams *p = g_object_get_data (G_OBJECT (align), ALIGN_KEY);

   if (!p) return;

   if (p->xscale >= 1.0)
      gtk_widget_set_halign (child, GTK_ALIGN_FILL);
   else if (p->xalign <= 0.01)
      gtk_widget_set_halign (child, GTK_ALIGN_START);
   else if (p->xalign >= 0.99)
      gtk_widget_set_halign (child, GTK_ALIGN_END);
   else
      gtk_widget_set_halign (child, GTK_ALIGN_CENTER);

   if (p->yscale >= 1.0)
      gtk_widget_set_valign (child, GTK_ALIGN_FILL);
   else if (p->yalign <= 0.01)
      gtk_widget_set_valign (child, GTK_ALIGN_START);
   else if (p->yalign >= 0.99)
      gtk_widget_set_valign (child, GTK_ALIGN_END);
   else
      gtk_widget_set_valign (child, GTK_ALIGN_CENTER);

   gtk_widget_set_hexpand (child, TRUE);
   gtk_widget_set_vexpand (child, TRUE);
}


void
gimv_container_add (GtkWidget *container, GtkWidget *child)
{
   g_return_if_fail (GTK_IS_WIDGET (container));
   g_return_if_fail (GTK_IS_WIDGET (child));

   if (GTK_IS_WINDOW (container)) {
      gtk_window_set_child (GTK_WINDOW (container), child);
   } else if (GTK_IS_BOX (container)) {
      apply_alignment (container, child);
      gimv_box_pack_start (GTK_BOX (container), child,
                           g_object_get_data (G_OBJECT (container), ALIGN_KEY)
                           ? TRUE : FALSE,
                           TRUE, 0);
   } else if (GTK_IS_SCROLLED_WINDOW (container)) {
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (container), child);
   } else if (GTK_IS_VIEWPORT (container)) {
      gtk_viewport_set_child (GTK_VIEWPORT (container), child);
   } else if (GTK_IS_FRAME (container)) {
      gtk_frame_set_child (GTK_FRAME (container), child);
   } else if (GTK_IS_BUTTON (container)) {
      gtk_button_set_child (GTK_BUTTON (container), child);
   } else if (GTK_IS_MENU_BUTTON (container)) {
      gtk_menu_button_set_child (GTK_MENU_BUTTON (container), child);
   } else if (GTK_IS_CHECK_BUTTON (container)) {
      gtk_check_button_set_child (GTK_CHECK_BUTTON (container), child);
   } else if (GTK_IS_EXPANDER (container)) {
      gtk_expander_set_child (GTK_EXPANDER (container), child);
   } else if (GTK_IS_POPOVER (container)) {
      gtk_popover_set_child (GTK_POPOVER (container), child);
   } else if (GTK_IS_OVERLAY (container)) {
      if (!gtk_overlay_get_child (GTK_OVERLAY (container)))
         gtk_overlay_set_child (GTK_OVERLAY (container), child);
      else
         gtk_overlay_add_overlay (GTK_OVERLAY (container), child);
   } else if (GTK_IS_PANED (container)) {
      if (!gtk_paned_get_start_child (GTK_PANED (container)))
         gtk_paned_set_start_child (GTK_PANED (container), child);
      else
         gtk_paned_set_end_child (GTK_PANED (container), child);
   } else if (GTK_IS_NOTEBOOK (container)) {
      gtk_notebook_append_page (GTK_NOTEBOOK (container), child, NULL);
   } else if (GTK_IS_GRID (container)) {
      gtk_grid_attach_next_to (GTK_GRID (container), child, NULL,
                               GTK_POS_BOTTOM, 1, 1);
   } else if (GTK_IS_FIXED (container)) {
      gtk_fixed_put (GTK_FIXED (container), child, 0, 0);
   } else if (GTK_IS_LIST_BOX (container)) {
      gtk_list_box_append (GTK_LIST_BOX (container), child);
   } else if (GTK_IS_FLOW_BOX (container)) {
      gtk_flow_box_append (GTK_FLOW_BOX (container), child);
   } else if (GTK_IS_REVEALER (container)) {
      gtk_revealer_set_child (GTK_REVEALER (container), child);
   } else if (GTK_IS_ASPECT_FRAME (container)) {
      gtk_aspect_frame_set_child (GTK_ASPECT_FRAME (container), child);
   } else if (GTK_IS_LIST_BOX_ROW (container)) {
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (container), child);
   } else if (GTK_IS_FLOW_BOX_CHILD (container)) {
      gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (container), child);
   } else if (GTK_IS_CENTER_BOX (container)) {
      gtk_center_box_set_center_widget (GTK_CENTER_BOX (container), child);
   } else {
      g_warning ("gimv_container_add: unsupported container type %s",
                 G_OBJECT_TYPE_NAME (container));
      gtk_widget_set_parent (child, container);
   }
}


void
gimv_container_remove (GtkWidget *container, GtkWidget *child)
{
   g_return_if_fail (GTK_IS_WIDGET (container));
   g_return_if_fail (GTK_IS_WIDGET (child));

   if (GTK_IS_WINDOW (container)) {
      if (gtk_window_get_child (GTK_WINDOW (container)) == child)
         gtk_window_set_child (GTK_WINDOW (container), NULL);
   } else if (GTK_IS_BOX (container)) {
      gtk_box_remove (GTK_BOX (container), child);
      box_update_spacer (GTK_BOX (container));
   } else if (GTK_IS_SCROLLED_WINDOW (container)) {
      GtkWidget *c = gtk_scrolled_window_get_child (GTK_SCROLLED_WINDOW (container));
      if (c == child) {
         gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (container), NULL);
      } else if (GTK_IS_VIEWPORT (c)
                 && gtk_viewport_get_child (GTK_VIEWPORT (c)) == child) {
         gtk_viewport_set_child (GTK_VIEWPORT (c), NULL);
      }
   } else if (GTK_IS_VIEWPORT (container)) {
      gtk_viewport_set_child (GTK_VIEWPORT (container), NULL);
   } else if (GTK_IS_FRAME (container)) {
      gtk_frame_set_child (GTK_FRAME (container), NULL);
   } else if (GTK_IS_BUTTON (container)) {
      gtk_button_set_child (GTK_BUTTON (container), NULL);
   } else if (GTK_IS_MENU_BUTTON (container)) {
      gtk_menu_button_set_child (GTK_MENU_BUTTON (container), NULL);
   } else if (GTK_IS_CHECK_BUTTON (container)) {
      gtk_check_button_set_child (GTK_CHECK_BUTTON (container), NULL);
   } else if (GTK_IS_EXPANDER (container)) {
      gtk_expander_set_child (GTK_EXPANDER (container), NULL);
   } else if (GTK_IS_POPOVER (container)) {
      gtk_popover_set_child (GTK_POPOVER (container), NULL);
   } else if (GTK_IS_OVERLAY (container)) {
      if (gtk_overlay_get_child (GTK_OVERLAY (container)) == child)
         gtk_overlay_set_child (GTK_OVERLAY (container), NULL);
      else
         gtk_overlay_remove_overlay (GTK_OVERLAY (container), child);
   } else if (GTK_IS_PANED (container)) {
      if (gtk_paned_get_start_child (GTK_PANED (container)) == child)
         gtk_paned_set_start_child (GTK_PANED (container), NULL);
      else if (gtk_paned_get_end_child (GTK_PANED (container)) == child)
         gtk_paned_set_end_child (GTK_PANED (container), NULL);
   } else if (GTK_IS_NOTEBOOK (container)) {
      gint n = gtk_notebook_page_num (GTK_NOTEBOOK (container), child);
      if (n >= 0)
         gtk_notebook_remove_page (GTK_NOTEBOOK (container), n);
   } else if (GTK_IS_GRID (container)) {
      gtk_grid_remove (GTK_GRID (container), child);
   } else if (GTK_IS_FIXED (container)) {
      gtk_fixed_remove (GTK_FIXED (container), child);
   } else if (GTK_IS_LIST_BOX (container)) {
      gtk_list_box_remove (GTK_LIST_BOX (container), child);
   } else if (GTK_IS_FLOW_BOX (container)) {
      gtk_flow_box_remove (GTK_FLOW_BOX (container), child);
   } else if (GTK_IS_REVEALER (container)) {
      gtk_revealer_set_child (GTK_REVEALER (container), NULL);
   } else if (GTK_IS_ASPECT_FRAME (container)) {
      gtk_aspect_frame_set_child (GTK_ASPECT_FRAME (container), NULL);
   } else if (GTK_IS_LIST_BOX_ROW (container)) {
      gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (container), NULL);
   } else if (GTK_IS_FLOW_BOX_CHILD (container)) {
      gtk_flow_box_child_set_child (GTK_FLOW_BOX_CHILD (container), NULL);
   } else if (GTK_IS_CENTER_BOX (container)) {
      GtkCenterBox *cb = GTK_CENTER_BOX (container);
      if (gtk_center_box_get_start_widget (cb) == child)
         gtk_center_box_set_start_widget (cb, NULL);
      else if (gtk_center_box_get_center_widget (cb) == child)
         gtk_center_box_set_center_widget (cb, NULL);
      else if (gtk_center_box_get_end_widget (cb) == child)
         gtk_center_box_set_end_widget (cb, NULL);
   } else if (gtk_widget_get_parent (child) == container) {
      gtk_widget_unparent (child);
   }
}


GList *
gimv_container_get_children (GtkWidget *container)
{
   GList *list = NULL;
   GtkWidget *child;

   g_return_val_if_fail (GTK_IS_WIDGET (container), NULL);

   if (GTK_IS_NOTEBOOK (container)) {
      gint i, n = gtk_notebook_get_n_pages (GTK_NOTEBOOK (container));
      for (i = 0; i < n; i++)
         list = g_list_append (list,
                               gtk_notebook_get_nth_page (GTK_NOTEBOOK (container), i));
      return list;
   }

   if (GTK_IS_SCROLLED_WINDOW (container) || GTK_IS_WINDOW (container)
       || GTK_IS_FRAME (container) || GTK_IS_BUTTON (container))
   {
      child = gimv_bin_get_child (container);
      return child ? g_list_append (NULL, child) : NULL;
   }

   for (child = gtk_widget_get_first_child (container);
        child;
        child = gtk_widget_get_next_sibling (child))
   {
      if (g_object_get_data (G_OBJECT (container), SPACER_KEY) == child)
         continue;
      /* skip internal children of frames/expanders etc. */
      if (GTK_IS_BOX (container) || GTK_IS_GRID (container)
          || GTK_IS_FIXED (container) || GTK_IS_PANED (container)
          || GTK_IS_OVERLAY (container) || GTK_IS_CENTER_BOX (container)
          || !GTK_IS_WIDGET (container))
      {
         list = g_list_append (list, child);
      } else {
         list = g_list_append (list, child);
      }
   }

   if (GTK_IS_PANED (container)) {
      g_list_free (list);
      list = NULL;
      if (gtk_paned_get_start_child (GTK_PANED (container)))
         list = g_list_append (list, gtk_paned_get_start_child (GTK_PANED (container)));
      if (gtk_paned_get_end_child (GTK_PANED (container)))
         list = g_list_append (list, gtk_paned_get_end_child (GTK_PANED (container)));
   }

   return list;
}


void
gimv_container_set_border_width (GtkWidget *widget, guint width)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));

   if (GTK_IS_WINDOW (widget)) {
      GtkWidget *child = gtk_window_get_child (GTK_WINDOW (widget));
      /* windows have no margin of their own; apply it to the child when set */
      g_object_set_data (G_OBJECT (widget), "gimv-border-width",
                         GUINT_TO_POINTER (width));
      if (!child) return;
      widget = child;
   }

   gtk_widget_set_margin_start (widget, width);
   gtk_widget_set_margin_end (widget, width);
   gtk_widget_set_margin_top (widget, width);
   gtk_widget_set_margin_bottom (widget, width);
}


GtkWidget *
gimv_bin_get_child (GtkWidget *bin)
{
   g_return_val_if_fail (GTK_IS_WIDGET (bin), NULL);

   if (GTK_IS_WINDOW (bin))
      return gtk_window_get_child (GTK_WINDOW (bin));
   if (GTK_IS_BUTTON (bin))
      return gtk_button_get_child (GTK_BUTTON (bin));
   if (GTK_IS_MENU_BUTTON (bin))
      return gtk_menu_button_get_child (GTK_MENU_BUTTON (bin));
   if (GTK_IS_CHECK_BUTTON (bin))
      return gtk_check_button_get_child (GTK_CHECK_BUTTON (bin));
   if (GTK_IS_FRAME (bin))
      return gtk_frame_get_child (GTK_FRAME (bin));
   if (GTK_IS_SCROLLED_WINDOW (bin)) {
      GtkWidget *c = gtk_scrolled_window_get_child (GTK_SCROLLED_WINDOW (bin));
      /* GTK4 wraps non scrollable children into a viewport automatically */
      if (GTK_IS_VIEWPORT (c) && gtk_viewport_get_child (GTK_VIEWPORT (c)))
         return gtk_viewport_get_child (GTK_VIEWPORT (c));
      return c;
   }
   if (GTK_IS_VIEWPORT (bin))
      return gtk_viewport_get_child (GTK_VIEWPORT (bin));
   if (GTK_IS_EXPANDER (bin))
      return gtk_expander_get_child (GTK_EXPANDER (bin));
   if (GTK_IS_POPOVER (bin))
      return gtk_popover_get_child (GTK_POPOVER (bin));
   if (GTK_IS_REVEALER (bin))
      return gtk_revealer_get_child (GTK_REVEALER (bin));

   return gtk_widget_get_first_child (bin);
}


void
gimv_widget_destroy (GtkWidget *widget)
{
   GtkWidget *parent;

   if (!widget) return;
   g_return_if_fail (GTK_IS_WIDGET (widget));

   if (GTK_IS_WINDOW (widget)) {
      gtk_window_destroy (GTK_WINDOW (widget));
      return;
   }

   parent = gtk_widget_get_parent (widget);
   if (parent) {
      /* GtkScrolledWindow wraps children into a GtkViewport */
      if (GTK_IS_VIEWPORT (parent)
          && GTK_IS_SCROLLED_WINDOW (gtk_widget_get_parent (parent)))
      {
         gtk_viewport_set_child (GTK_VIEWPORT (parent), NULL);
      } else {
         /* the parent may hold the last reference */
         g_object_ref (widget);
         gimv_container_remove (parent, widget);
         /* fallback for internal children */
         if (gtk_widget_get_parent (widget) == parent)
            gtk_widget_unparent (widget);
         g_object_unref (widget);
      }
   } else if (g_object_is_floating (widget)) {
      g_object_ref_sink (widget);
      g_object_unref (widget);
   } else {
      /* a widget which is owned by a reference (e.g. an unparented popover)
         -- let the owner drop it. make it at least emit ::destroy */
      g_object_run_dispose (G_OBJECT (widget));
   }
}


void
gimv_widget_show_all (GtkWidget *widget)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));

   if (GTK_IS_WINDOW (widget))
      gtk_window_present (GTK_WINDOW (widget));
   else
      gtk_widget_set_visible (widget, TRUE);
}


void
gimv_widget_hide_all (GtkWidget *widget)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));
   gtk_widget_set_visible (widget, FALSE);
}


GtkWidget *
gimv_widget_get_toplevel (GtkWidget *widget)
{
   GtkRoot *root;

   g_return_val_if_fail (GTK_IS_WIDGET (widget), NULL);

   root = gtk_widget_get_root (widget);
   if (root) return GTK_WIDGET (root);

   /* not yet in a window: return the topmost ancestor */
   while (gtk_widget_get_parent (widget))
      widget = gtk_widget_get_parent (widget);
   return widget;
}


gboolean
gimv_widget_is_toplevel (GtkWidget *widget)
{
   return GTK_IS_ROOT (widget);
}


gboolean
gimv_widget_is_mapped (GtkWidget *widget)
{
   return widget && gtk_widget_get_mapped (widget);
}


gboolean
gimv_widget_is_visible (GtkWidget *widget)
{
   return widget && gtk_widget_get_visible (widget);
}


gboolean
gimv_widget_is_realized (GtkWidget *widget)
{
   return widget && gtk_widget_get_realized (widget);
}


void
gimv_widget_set_size (GtkWidget *widget, gint width, gint height)
{
   gint w, h;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   /* gtk_widget_set_usize() of GTK1 accepted -2 as "unchanged" */
   gtk_widget_get_size_request (widget, &w, &h);
   if (width  < -1) width  = w;
   if (height < -1) height = h;

   if (GTK_IS_WINDOW (widget))
      gtk_window_set_default_size (GTK_WINDOW (widget), width, height);
   else
      gtk_widget_set_size_request (widget, width, height);
}


void
gimv_misc_set_alignment (GtkWidget *widget, gfloat xalign, gfloat yalign)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));

   if (GTK_IS_LABEL (widget)) {
      gtk_label_set_xalign (GTK_LABEL (widget), xalign);
      gtk_label_set_yalign (GTK_LABEL (widget), yalign);
      return;
   }

   if (xalign <= 0.01)
      gtk_widget_set_halign (widget, GTK_ALIGN_START);
   else if (xalign >= 0.99)
      gtk_widget_set_halign (widget, GTK_ALIGN_END);
   else
      gtk_widget_set_halign (widget, GTK_ALIGN_CENTER);

   if (yalign <= 0.01)
      gtk_widget_set_valign (widget, GTK_ALIGN_START);
   else if (yalign >= 0.99)
      gtk_widget_set_valign (widget, GTK_ALIGN_END);
   else
      gtk_widget_set_valign (widget, GTK_ALIGN_CENTER);
}


void
gimv_misc_set_padding (GtkWidget *widget, gint xpad, gint ypad)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));

   gtk_widget_set_margin_start (widget, xpad);
   gtk_widget_set_margin_end (widget, xpad);
   gtk_widget_set_margin_top (widget, ypad);
   gtk_widget_set_margin_bottom (widget, ypad);
}


GtkWidget *
gimv_alignment_new (gfloat xalign, gfloat yalign, gfloat xscale, gfloat yscale)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   AlignParams *p = g_new0 (AlignParams, 1);

   p->xalign = xalign; p->yalign = yalign;
   p->xscale = xscale; p->yscale = yscale;
   g_object_set_data_full (G_OBJECT (box), ALIGN_KEY, p, g_free);

   return box;
}


GtkWidget *
gimv_event_box_new (void)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
   gtk_widget_add_css_class (box, "gimv-event-box");
   return box;
}


GtkWidget *
gimv_frame_new (const gchar *label)
{
   return gtk_frame_new (label);
}


GtkWidget *
gimv_table_new (guint rows, guint columns, gboolean homogeneous)
{
   GtkWidget *grid = gtk_grid_new ();

   gtk_grid_set_row_homogeneous (GTK_GRID (grid), homogeneous);
   gtk_grid_set_column_homogeneous (GTK_GRID (grid), homogeneous);

   return grid;
}


void
gimv_table_attach (GtkWidget *table, GtkWidget *child,
                   guint left, guint right, guint top, guint bottom,
                   GimvAttachOptions xoptions, GimvAttachOptions yoptions,
                   guint xpadding, guint ypadding)
{
   g_return_if_fail (GTK_IS_GRID (table));
   g_return_if_fail (GTK_IS_WIDGET (child));

   gtk_widget_set_hexpand (child, (xoptions & GIMV_EXPAND) ? TRUE : FALSE);
   gtk_widget_set_vexpand (child, (yoptions & GIMV_EXPAND) ? TRUE : FALSE);
   if (!(xoptions & GIMV_FILL) && GTK_IS_LABEL (child)
       && gtk_label_get_xalign (GTK_LABEL (child)) == 0.5)
      gtk_widget_set_halign (child, GTK_ALIGN_CENTER);
   else if (!(xoptions & GIMV_FILL) && !GTK_IS_LABEL (child))
      gtk_widget_set_halign (child, GTK_ALIGN_START);
   if (!(yoptions & GIMV_FILL))
      gtk_widget_set_valign (child, GTK_ALIGN_CENTER);

   if (xpadding) {
      gtk_widget_set_margin_start (child, xpadding);
      gtk_widget_set_margin_end (child, xpadding);
   }
   if (ypadding) {
      gtk_widget_set_margin_top (child, ypadding);
      gtk_widget_set_margin_bottom (child, ypadding);
   }

   gtk_grid_attach (GTK_GRID (table), child, left, top,
                    MAX (1, (gint) right - (gint) left),
                    MAX (1, (gint) bottom - (gint) top));
}


void
gimv_table_attach_defaults (GtkWidget *table, GtkWidget *child,
                            guint left, guint right, guint top, guint bottom)
{
   gimv_table_attach (table, child, left, right, top, bottom,
                      GIMV_EXPAND | GIMV_FILL, GIMV_EXPAND | GIMV_FILL, 0, 0);
}


void
gimv_container_foreach (GtkWidget *container, GimvCallback callback, gpointer data)
{
   GList *list, *node;

   list = gimv_container_get_children (container);
   for (node = list; node; node = g_list_next (node))
      callback (node->data, data);
   g_list_free (list);
}


void
gimv_paned_pack1 (GtkPaned *paned, GtkWidget *child, gboolean resize, gboolean shrink)
{
   g_return_if_fail (GTK_IS_PANED (paned));
   gtk_paned_set_start_child (paned, child);
   gtk_paned_set_resize_start_child (paned, resize);
   gtk_paned_set_shrink_start_child (paned, shrink);
}


void
gimv_paned_pack2 (GtkPaned *paned, GtkWidget *child, gboolean resize, gboolean shrink)
{
   g_return_if_fail (GTK_IS_PANED (paned));
   gtk_paned_set_end_child (paned, child);
   gtk_paned_set_resize_end_child (paned, resize);
   gtk_paned_set_shrink_end_child (paned, shrink);
}


GtkWidget *
gimv_popup_window_new (void)
{
   GtkWidget *window = gtk_window_new ();
   gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
   gtk_window_set_resizable (GTK_WINDOW (window), FALSE);
   return window;
}


/*
 *  GTK4 wants dialogs to have a transient parent.  GTK2 code often created
 *  them without; use the active (or any visible) application window.
 */
void
gimv_window_set_default_transient (GtkWindow *window)
{
   GListModel *toplevels;
   GtkWindow *parent = NULL, *fallback = NULL;
   guint i, n;

   g_return_if_fail (GTK_IS_WINDOW (window));

   if (gtk_window_get_transient_for (window)) return;

   toplevels = gtk_window_get_toplevels ();
   n = g_list_model_get_n_items (toplevels);
   for (i = 0; i < n; i++) {
      GtkWindow *w = g_list_model_get_item (toplevels, i);
      g_object_unref (w);   /* the list keeps a reference */
      if (w == window || !gtk_widget_get_visible (GTK_WIDGET (w))) continue;
      if (GTK_IS_DIALOG (w)) continue;
      if (gtk_window_is_active (w)) {
         parent = w;
         break;
      }
      if (!fallback) fallback = w;
   }
   if (!parent) parent = fallback;

   if (parent)
      gtk_window_set_transient_for (window, parent);
}


void
gimv_grab_add (GtkWidget *widget)
{
   /* GTK4 has no application level grabs; modal windows block the others */
   if (GTK_IS_WINDOW (widget))
      gtk_window_set_modal (GTK_WINDOW (widget), TRUE);
}


void
gimv_grab_remove (GtkWidget *widget)
{
   if (GTK_IS_WINDOW (widget))
      gtk_window_set_modal (GTK_WINDOW (widget), FALSE);
}


GtkWidget *
gimv_entry_new_with_max_length (gint max)
{
   GtkWidget *entry = gtk_entry_new ();
   gtk_entry_set_max_length (GTK_ENTRY (entry), max);
   return entry;
}


GtkWidget *
gimv_scrolled_window_new (GtkAdjustment *hadj, GtkAdjustment *vadj)
{
   GtkWidget *sw = gtk_scrolled_window_new ();

   if (hadj)
      gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (sw), hadj);
   if (vadj)
      gtk_scrolled_window_set_vadjustment (GTK_SCROLLED_WINDOW (sw), vadj);
   gtk_widget_set_hexpand (sw, TRUE);
   gtk_widget_set_vexpand (sw, TRUE);

   return sw;
}



/******************************************************************************
 *
 *   buttons
 *
 ******************************************************************************/
gboolean
gimv_toggle_get_active (GtkWidget *widget)
{
   g_return_val_if_fail (GTK_IS_WIDGET (widget), FALSE);

   if (GTK_IS_TOGGLE_BUTTON (widget))
      return gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget));
   if (GTK_IS_CHECK_BUTTON (widget))
      return gtk_check_button_get_active (GTK_CHECK_BUTTON (widget));
   if (GTK_IS_SWITCH (widget))
      return gtk_switch_get_active (GTK_SWITCH (widget));

   g_warning ("gimv_toggle_get_active: %s is not a toggle widget",
              G_OBJECT_TYPE_NAME (widget));
   return FALSE;
}


void
gimv_toggle_set_active (GtkWidget *widget, gboolean active)
{
   g_return_if_fail (GTK_IS_WIDGET (widget));

   if (GTK_IS_TOGGLE_BUTTON (widget))
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), active);
   else if (GTK_IS_CHECK_BUTTON (widget))
      gtk_check_button_set_active (GTK_CHECK_BUTTON (widget), active);
   else if (GTK_IS_SWITCH (widget))
      gtk_switch_set_active (GTK_SWITCH (widget), active);
   else
      g_warning ("gimv_toggle_set_active: %s is not a toggle widget",
                 G_OBJECT_TYPE_NAME (widget));
}


/*
 *  GTK2's radio button API identifies a group by a GSList of its members.
 *  Emulate that by keeping a shared list object on every member.
 */
typedef struct {
   GSList *members;
   gint    ref_count;
} RadioGroup;

#define RADIO_GROUP_KEY "gimv-radio-group"

static void
radio_group_unref (gpointer data)
{
   RadioGroup *group = data;
   if (--group->ref_count <= 0) {
      g_slist_free (group->members);
      g_free (group);
   }
}


static void
cb_radio_destroy (GtkWidget *widget, gpointer data)
{
   RadioGroup *group = g_object_get_data (G_OBJECT (widget), RADIO_GROUP_KEY);
   if (group)
      group->members = g_slist_remove (group->members, widget);
}


static GtkWidget *
radio_new (GtkWidget *member, const gchar *label)
{
   GtkWidget *radio = gtk_check_button_new_with_mnemonic (label);
   RadioGroup *group = NULL;

   if (member)
      group = g_object_get_data (G_OBJECT (member), RADIO_GROUP_KEY);

   if (group) {
      gtk_check_button_set_group (GTK_CHECK_BUTTON (radio),
                                  GTK_CHECK_BUTTON (member));
   } else {
      group = g_new0 (RadioGroup, 1);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (radio), TRUE);
   }

   /* GTK2 prepends new members */
   group->members = g_slist_prepend (group->members, radio);
   group->ref_count++;
   g_object_set_data_full (G_OBJECT (radio), RADIO_GROUP_KEY, group,
                           radio_group_unref);
   g_signal_connect (radio, "destroy", G_CALLBACK (cb_radio_destroy), NULL);

   return radio;
}


GtkWidget *
gimv_radio_button_new_with_label (GSList *group, const gchar *label)
{
   return radio_new (group ? group->data : NULL, label);
}


GtkWidget *
gimv_radio_button_new_with_label_from_widget (GtkWidget *member,
                                              const gchar *label)
{
   return radio_new (member, label);
}


GSList *
gimv_radio_button_get_group (GtkWidget *radio)
{
   RadioGroup *group;

   g_return_val_if_fail (GTK_IS_WIDGET (radio), NULL);

   group = g_object_get_data (G_OBJECT (radio), RADIO_GROUP_KEY);
   return group ? group->members : NULL;
}


GtkWidget *
gimv_button_new_with_mnemonic_label (const gchar *label)
{
   return gtk_button_new_with_mnemonic (label);
}


GtkWidget *
gimv_button_new_from_stock (const gchar *stock_id)
{
   return gtk_button_new_with_mnemonic (stock_id);
}


GtkWidget *
gimv_arrow_new (GtkArrowType arrow_type)
{
   const gchar *name;

   switch (arrow_type) {
   case GTK_ARROW_UP:    name = "pan-up-symbolic";    break;
   case GTK_ARROW_DOWN:  name = "pan-down-symbolic";  break;
   case GTK_ARROW_LEFT:  name = "pan-start-symbolic"; break;
   case GTK_ARROW_RIGHT: name = "pan-end-symbolic";   break;
   default:              name = "pan-down-symbolic";  break;
   }

   return gtk_image_new_from_icon_name (name);
}



/******************************************************************************
 *
 *   option menu
 *
 ******************************************************************************/
typedef struct {
   GCallback func;
   gpointer  data;
   gint      block;
} OptionMenuPriv;

#define OPTION_MENU_KEY "gimv-option-menu"


static void
cb_option_menu_selected (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
   OptionMenuPriv *priv = g_object_get_data (obj, OPTION_MENU_KEY);
   guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (obj));

   if (sel == GTK_INVALID_LIST_POSITION) return;

   g_object_set_data (obj, "num", GINT_TO_POINTER (sel));

   if (!priv || priv->block > 0 || !priv->func) return;

   ((void (*) (GtkWidget *, gpointer)) priv->func) (GTK_WIDGET (obj), priv->data);
}


GtkWidget *
gimv_option_menu_new (const gchar **labels, gint n_labels, gint def_val,
                      GCallback func, gpointer data)
{
   GtkStringList *list;
   GtkWidget *dropdown;
   OptionMenuPriv *priv;
   gint i;

   list = gtk_string_list_new (NULL);
   for (i = 0; labels && labels[i] && (n_labels < 0 || i < n_labels); i++)
      gtk_string_list_append (list, labels[i]);

   dropdown = gtk_drop_down_new (G_LIST_MODEL (list), NULL);

   priv = g_new0 (OptionMenuPriv, 1);
   priv->func = func;
   priv->data = data;
   priv->block = 1;
   g_object_set_data_full (G_OBJECT (dropdown), OPTION_MENU_KEY, priv, g_free);

   g_signal_connect (dropdown, "notify::selected",
                     G_CALLBACK (cb_option_menu_selected), NULL);
   if (def_val >= 0 && def_val < i)
      gtk_drop_down_set_selected (GTK_DROP_DOWN (dropdown), def_val);
   g_object_set_data (G_OBJECT (dropdown), "num", GINT_TO_POINTER (MAX (def_val, 0)));
   priv->block = 0;

   return dropdown;
}


void
gimv_option_menu_set_history (GtkWidget *option_menu, gint index)
{
   OptionMenuPriv *priv;

   g_return_if_fail (GTK_IS_DROP_DOWN (option_menu));

   priv = g_object_get_data (G_OBJECT (option_menu), OPTION_MENU_KEY);
   if (priv) priv->block++;
   gtk_drop_down_set_selected (GTK_DROP_DOWN (option_menu), index);
   g_object_set_data (G_OBJECT (option_menu), "num", GINT_TO_POINTER (index));
   if (priv) priv->block--;
}


gint
gimv_option_menu_get_history (GtkWidget *option_menu)
{
   guint sel;

   g_return_val_if_fail (GTK_IS_DROP_DOWN (option_menu), -1);

   sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (option_menu));
   return sel == GTK_INVALID_LIST_POSITION ? -1 : (gint) sel;
}


void
gimv_option_menu_set_sensitive_item (GtkWidget *option_menu,
                                     gint index, gboolean sensitive)
{
   /* GtkDropDown cannot disable single items. */
}



/******************************************************************************
 *
 *   combo
 *
 ******************************************************************************/
GtkWidget *
gimv_combo_new (void)
{
   return gtk_combo_box_text_new_with_entry ();
}


GtkWidget *
gimv_combo_get_entry (GtkWidget *combo)
{
   g_return_val_if_fail (GTK_IS_COMBO_BOX (combo), NULL);
   return gtk_combo_box_get_child (GTK_COMBO_BOX (combo));
}


void
gimv_combo_set_popdown_strings (GtkWidget *combo, GList *strings)
{
   GList *node;

   g_return_if_fail (GTK_IS_COMBO_BOX_TEXT (combo));

   gtk_combo_box_text_remove_all (GTK_COMBO_BOX_TEXT (combo));
   for (node = strings; node; node = g_list_next (node))
      gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (combo), node->data);
}



/******************************************************************************
 *
 *   dialogs
 *
 ******************************************************************************/
GtkWidget *
gimv_dialog_get_vbox (GtkWidget *dialog)
{
   g_return_val_if_fail (GTK_IS_DIALOG (dialog), NULL);
   return gtk_dialog_get_content_area (GTK_DIALOG (dialog));
}


GtkWidget *
gimv_dialog_get_action_area (GtkWidget *dialog)
{
   GtkWidget *area, *vbox;

   g_return_val_if_fail (GTK_IS_DIALOG (dialog), NULL);

   area = g_object_get_data (G_OBJECT (dialog), "gimv-action-area");
   if (area) return area;

   vbox = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
   area = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_set_homogeneous (GTK_BOX (area), TRUE);
   gtk_widget_set_halign (area, GTK_ALIGN_END);
   gtk_widget_set_margin_top (area, 6);
   gtk_widget_set_margin_bottom (area, 6);
   gtk_widget_set_margin_start (area, 6);
   gtk_widget_set_margin_end (area, 6);
   gtk_widget_add_css_class (area, "dialog-action-area");
   gimv_box_pack_end (GTK_BOX (vbox), area, FALSE, FALSE, 0);
   g_object_set_data (G_OBJECT (dialog), "gimv-action-area", area);

   return area;
}


typedef struct {
   GMainLoop *loop;
   gint       response;
   gboolean   destroyed;
} RunInfo;


static void
run_shutdown_loop (RunInfo *ri)
{
   if (g_main_loop_is_running (ri->loop))
      g_main_loop_quit (ri->loop);
}


static void
cb_run_response (GtkDialog *dialog, gint response_id, RunInfo *ri)
{
   ri->response = response_id;
   run_shutdown_loop (ri);
}


static gboolean
cb_run_close_request (GtkWindow *window, RunInfo *ri)
{
   ri->response = GTK_RESPONSE_DELETE_EVENT;
   run_shutdown_loop (ri);
   return TRUE; /* do not destroy, the caller does it */
}


static void
cb_run_destroy (GtkWidget *widget, RunInfo *ri)
{
   ri->destroyed = TRUE;
   run_shutdown_loop (ri);
}


gint
gimv_dialog_run (GtkDialog *dialog)
{
   RunInfo ri = { NULL, GTK_RESPONSE_NONE, FALSE };
   gulong id_response, id_close, id_destroy;
   gboolean was_modal;

   g_return_val_if_fail (GTK_IS_DIALOG (dialog), -1);

   g_object_ref (dialog);

   was_modal = gtk_window_get_modal (GTK_WINDOW (dialog));
   if (!was_modal)
      gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);

   id_response = g_signal_connect (dialog, "response",
                                   G_CALLBACK (cb_run_response), &ri);
   id_close = g_signal_connect (dialog, "close-request",
                                G_CALLBACK (cb_run_close_request), &ri);
   id_destroy = g_signal_connect (dialog, "destroy",
                                  G_CALLBACK (cb_run_destroy), &ri);

   gtk_window_present (GTK_WINDOW (dialog));

   ri.loop = g_main_loop_new (NULL, FALSE);
   g_main_loop_run (ri.loop);
   g_main_loop_unref (ri.loop);

   if (!ri.destroyed) {
      g_signal_handler_disconnect (dialog, id_response);
      g_signal_handler_disconnect (dialog, id_close);
      g_signal_handler_disconnect (dialog, id_destroy);
      if (!was_modal)
         gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);
   }

   g_object_unref (dialog);

   return ri.response;
}


void
gimv_window_run_modal (GtkWindow *window)
{
   RunInfo ri = { NULL, GTK_RESPONSE_NONE, FALSE };
   gulong id;

   g_return_if_fail (GTK_IS_WINDOW (window));

   id = g_signal_connect (window, "destroy", G_CALLBACK (cb_run_destroy), &ri);
   gtk_window_present (window);

   ri.loop = g_main_loop_new (NULL, FALSE);
   g_main_loop_run (ri.loop);
   g_main_loop_unref (ri.loop);

   if (!ri.destroyed)
      g_signal_handler_disconnect (window, id);
}


void
gimv_window_set_icon_pixbuf (GtkWindow *window, GdkPixbuf *pixbuf)
{
   /* GTK4 only supports themed icon names for windows */
   gtk_window_set_icon_name (window, "gimv");
}



/******************************************************************************
 *
 *   keyboard shortcuts
 *
 ******************************************************************************/
typedef struct {
   GimvShortcutFunc func;
   gpointer         data;
} ShortcutData;


static gboolean
cb_shortcut (GtkWidget *widget, GVariant *args, gpointer user_data)
{
   ShortcutData *sd = user_data;
   return sd->func (widget, sd->data);
}


void
gimv_widget_add_shortcut (GtkWidget *widget, guint keyval, GdkModifierType mods,
                          GimvShortcutFunc func, gpointer data)
{
   GtkEventController *controller;
   GtkShortcut *shortcut;
   ShortcutData *sd;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   controller = g_object_get_data (G_OBJECT (widget), "gimv-shortcut-controller");
   if (!controller) {
      controller = gtk_shortcut_controller_new ();
      gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_BUBBLE);
      gtk_widget_add_controller (widget, controller);
      g_object_set_data (G_OBJECT (widget), "gimv-shortcut-controller", controller);
   }

   sd = g_new0 (ShortcutData, 1);
   sd->func = func;
   sd->data = data;

   shortcut = gtk_shortcut_new (gtk_keyval_trigger_new (keyval, mods),
                                gtk_callback_action_new (cb_shortcut, sd, g_free));
   gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (controller),
                                         shortcut);
}


gboolean
gimv_accelerator_parse (const gchar *accel, guint *keyval, GdkModifierType *mods)
{
   gchar *str, *p;
   gboolean retval;

   *keyval = 0;
   *mods = 0;
   if (!accel || !*accel) return FALSE;

   /* GTK2 accepted "<control>" and "<alt>", GTK4 wants the same but is
      stricter about "<mod1>" */
   str = g_strdup (accel);
   while ((p = strstr (str, "<mod1>")) || (p = strstr (str, "<Mod1>"))) {
      memcpy (p, "<alt> ", 6);
   }
   /* remove the blank we may have inserted */
   {
      gchar **v = g_strsplit (str, " ", -1);
      g_free (str);
      str = g_strjoinv ("", v);
      g_strfreev (v);
   }

   retval = gtk_accelerator_parse (str, keyval, mods);
   if (retval && *keyval) {
      /* shortcuts are matched on lower case letters */
      *keyval = gdk_keyval_to_lower (*keyval);
   }
   g_free (str);

   return retval && *keyval != 0;
}



/******************************************************************************
 *
 *   event emulation
 *
 ******************************************************************************/
typedef struct {
   GimvEventSignal signal;
   GCallback       handler;
   gpointer        data;
   gboolean        capture;  /* dispatched from the capture phase controller */
   gboolean        after;    /* run after the other handlers */
   gint            blocked;
   gulong          id;
} EventHandler;

typedef struct {
   GList              *handlers;
   GtkEventController *legacy_capture;
   GtkEventController *legacy_bubble;
   GtkEventController *motion;   /* enter / leave */
   GtkEventController *focus;
   gulong              close_request_id;
   gulong              realize_id;
   gulong              layout_id;
   GdkSurface         *surface;

   /* double click detection */
   GdkEvent           *last_counted_event;
   guint32             last_press_time;
   guint               last_press_button;
   gdouble             last_press_x, last_press_y;
   gint                click_count;

   /* button events arriving while a button press is being dispatched
      (handlers often call gimv_flush_events()) are delayed until the
      press has been handled, to keep GTK2's event order
      (press, 2button-press, release) */
   gint                in_press;
   GList              *pending;     /* PendingEvent */
   guint               pending_id;
   gboolean            replaying;

   /* last known pointer position */
   gboolean            have_pointer;
   gdouble             pointer_x, pointer_y;
} EventData;

#define EVENT_DATA_KEY "gimv-event-data"

static gulong event_handler_id_seq = 0;


typedef struct {
   GdkEvent           *event;
   GtkEventController *controller;
   gboolean            capture;
} PendingEvent;


static void
pending_event_free (gpointer data)
{
   PendingEvent *pe = data;
   gdk_event_unref (pe->event);
   g_free (pe);
}


static void
event_data_free (gpointer data)
{
   EventData *ed = data;
   if (ed->pending_id)
      g_source_remove (ed->pending_id);
   g_list_free_full (ed->pending, pending_event_free);
   g_list_free_full (ed->handlers, g_free);
   g_free (ed);
}


static EventData *
get_event_data (GtkWidget *widget, gboolean create)
{
   EventData *ed = g_object_get_data (G_OBJECT (widget), EVENT_DATA_KEY);

   if (!ed && create) {
      ed = g_new0 (EventData, 1);
      g_object_set_data_full (G_OBJECT (widget), EVENT_DATA_KEY, ed,
                              event_data_free);
   }

   return ed;
}


static gboolean
widget_wants_capture (GtkWidget *widget, GimvEventSignal signal)
{
   if (GTK_IS_TREE_VIEW (widget) || GTK_IS_TEXT_VIEW (widget)
       || GTK_IS_ENTRY (widget) || GTK_IS_TEXT (widget)
       || GTK_IS_ICON_VIEW (widget) || GTK_IS_SPIN_BUTTON (widget)
       || GTK_IS_NOTEBOOK (widget) || GTK_IS_SCROLLED_WINDOW (widget))
   {
      return TRUE;
   }

   /* GTK2 ran key handlers of a window before the focus widget got the key */
   if (GTK_IS_WINDOW (widget)
       && (signal == GIMV_EVENT_KEY_PRESS || signal == GIMV_EVENT_KEY_RELEASE))
   {
      return TRUE;
   }

   return FALSE;
}


static void
translate_coords (GtkWidget *widget, GdkEvent *event,
                  gdouble *x, gdouble *y, gdouble *x_root, gdouble *y_root)
{
   GtkNative *native;
   double sx = 0, sy = 0, nx = 0, ny = 0;
   graphene_point_t in, out;

   *x = *y = 0;
   if (x_root) *x_root = 0;
   if (y_root) *y_root = 0;

   if (!gdk_event_get_position (event, &sx, &sy)) return;

   if (x_root) *x_root = sx;
   if (y_root) *y_root = sy;

   native = gtk_widget_get_native (widget);
   if (!native) {
      *x = sx; *y = sy;
      return;
   }

   gtk_native_get_surface_transform (native, &nx, &ny);
   in.x = sx - nx;
   in.y = sy - ny;

   if (gtk_widget_compute_point (GTK_WIDGET (native), widget, &in, &out)) {
      *x = out.x;
      *y = out.y;
   } else {
      *x = in.x;
      *y = in.y;
   }
}


typedef gboolean (*EventFunc) (GtkWidget *widget, gpointer event, gpointer data);


static gboolean
dispatch (GtkWidget *widget, EventData *ed, GimvEventSignal signal,
          gboolean capture, gpointer event)
{
   GList *node, *list;
   gboolean handled = FALSE;
   gint pass;

   /* copy: handlers may disconnect themselves */
   list = g_list_copy (ed->handlers);
   g_object_ref (widget);

   /* handlers connected with gimv_event_connect_after() run last, like
      GTK2's class handlers of RUN_LAST event signals */
   for (pass = 0; pass < 2 && !handled; pass++) {
      for (node = list; node; node = g_list_next (node)) {
         EventHandler *h = node->data;

         if (!g_list_find (ed->handlers, h)) continue;
         if (h->signal != signal || h->capture != capture || h->blocked > 0)
            continue;
         if ((pass == 0) == h->after) continue;

         handled = ((EventFunc) h->handler) (widget, event, h->data);
         if (handled) break;

         /* the widget may have been destroyed by the handler */
         if (!get_event_data (widget, FALSE)) {
            handled = TRUE;
            break;
         }
      }
   }

   g_object_unref (widget);
   g_list_free (list);

   return handled;
}


static gboolean handle_legacy_event (GtkWidget *widget,
                                     GtkEventController *controller,
                                     GdkEvent *event, gboolean capture);


static gboolean
idle_dispatch_pending (gpointer data)
{
   GtkWidget *widget = data;
   EventData *ed = get_event_data (widget, FALSE);

   if (!ed) return G_SOURCE_REMOVE;
   if (ed->in_press > 0) return G_SOURCE_CONTINUE;

   ed->pending_id = 0;

   g_object_ref (widget);
   while ((ed = get_event_data (widget, FALSE)) && ed->pending && ed->in_press == 0) {
      PendingEvent *pe = ed->pending->data;
      ed->pending = g_list_delete_link (ed->pending, ed->pending);
      ed->replaying = TRUE;
      handle_legacy_event (widget, pe->controller, pe->event, pe->capture);
      ed = get_event_data (widget, FALSE);
      if (ed) ed->replaying = FALSE;
      pending_event_free (pe);
   }
   /* a press dispatched above may have queued more events */
   if (ed && ed->pending && !ed->pending_id)
      ed->pending_id = g_idle_add (idle_dispatch_pending, widget);
   g_object_unref (widget);

   return G_SOURCE_REMOVE;
}


static gboolean
handle_legacy_event (GtkWidget *widget, GtkEventController *controller,
                     GdkEvent *event, gboolean capture)
{
   EventData *ed = get_event_data (widget, FALSE);
   GdkEventType type;
   gboolean handled = FALSE;

   if (!ed) return FALSE;

   type = gdk_event_get_event_type (event);

   if ((type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE)
       && (ed->in_press > 0 || (ed->pending && !ed->replaying)))
   {
      PendingEvent *pe = g_new0 (PendingEvent, 1);
      pe->event = gdk_event_ref (event);
      pe->controller = controller;
      pe->capture = capture;
      ed->pending = g_list_append (ed->pending, pe);
      if (!ed->pending_id)
         ed->pending_id = g_idle_add (idle_dispatch_pending, widget);
      return FALSE;
   }

   switch (type) {
   case GDK_BUTTON_PRESS:
   {
      GimvEventButton ev;
      gint dbl_time = 400, dbl_dist = 5;
      GtkSettings *settings = gtk_widget_get_settings (widget);

      memset (&ev, 0, sizeof (ev));
      ev.type = GIMV_BUTTON_PRESS;
      ev.time = gdk_event_get_time (event);
      ev.state = gdk_event_get_modifier_state (event);
      ev.button = gdk_button_event_get_button (event);
      ev.event = event;
      ev.controller = controller;
      translate_coords (widget, event, &ev.x, &ev.y, &ev.x_root, &ev.y_root);

      ed->have_pointer = TRUE;
      ed->pointer_x = ev.x;
      ed->pointer_y = ev.y;

      if (ed->last_counted_event != event) {
         /* detect multiple clicks only once per event */
         ed->last_counted_event = event;
         if (settings)
            g_object_get (settings,
                          "gtk-double-click-time", &dbl_time,
                          "gtk-double-click-distance", &dbl_dist,
                          NULL);
         if (ed->click_count > 0
             && ev.button == ed->last_press_button
             && ev.time - ed->last_press_time <= (guint32) dbl_time
             && ABS (ev.x - ed->last_press_x) <= dbl_dist
             && ABS (ev.y - ed->last_press_y) <= dbl_dist)
         {
            ed->click_count++;
         } else {
            ed->click_count = 1;
         }
         ed->last_press_time = ev.time;
         ed->last_press_button = ev.button;
         ed->last_press_x = ev.x;
         ed->last_press_y = ev.y;
      }

      ed->in_press++;
      g_object_ref (widget);

      handled = dispatch (widget, ed, GIMV_EVENT_BUTTON_PRESS, capture, &ev);

      ed = get_event_data (widget, FALSE);
      if (ed && (ed->click_count == 2 || ed->click_count == 3)) {
         ev.type = ed->click_count == 2 ? GIMV_2BUTTON_PRESS : GIMV_3BUTTON_PRESS;
         handled = dispatch (widget, ed, GIMV_EVENT_BUTTON_PRESS, capture, &ev)
            || handled;
         ed = get_event_data (widget, FALSE);
         if (ed && ed->click_count == 3) ed->click_count = 0;
      }

      if (ed) ed->in_press--;
      g_object_unref (widget);
      break;
   }
   case GDK_BUTTON_RELEASE:
   {
      GimvEventButton ev;

      memset (&ev, 0, sizeof (ev));
      ev.type = GIMV_BUTTON_RELEASE;
      ev.time = gdk_event_get_time (event);
      ev.state = gdk_event_get_modifier_state (event);
      ev.button = gdk_button_event_get_button (event);
      ev.event = event;
      ev.controller = controller;
      translate_coords (widget, event, &ev.x, &ev.y, &ev.x_root, &ev.y_root);

      handled = dispatch (widget, ed, GIMV_EVENT_BUTTON_RELEASE, capture, &ev);
      break;
   }
   case GDK_MOTION_NOTIFY:
   {
      GimvEventMotion ev;

      memset (&ev, 0, sizeof (ev));
      ev.type = GDK_MOTION_NOTIFY;
      ev.time = gdk_event_get_time (event);
      ev.state = gdk_event_get_modifier_state (event);
      ev.event = event;
      ev.controller = controller;
      translate_coords (widget, event, &ev.x, &ev.y, &ev.x_root, &ev.y_root);

      ed->have_pointer = TRUE;
      ed->pointer_x = ev.x;
      ed->pointer_y = ev.y;

      handled = dispatch (widget, ed, GIMV_EVENT_MOTION_NOTIFY, capture, &ev);
      break;
   }
   case GDK_KEY_PRESS:
   case GDK_KEY_RELEASE:
   {
      GimvEventKey ev;

      memset (&ev, 0, sizeof (ev));
      ev.type = type;
      ev.time = gdk_event_get_time (event);
      ev.state = gdk_event_get_modifier_state (event);
      ev.keyval = gdk_key_event_get_keyval (event);
      ev.hardware_keycode = gdk_key_event_get_keycode (event);
      ev.event = event;
      ev.controller = controller;

      handled = dispatch (widget, ed,
                          type == GDK_KEY_PRESS
                          ? GIMV_EVENT_KEY_PRESS : GIMV_EVENT_KEY_RELEASE,
                          capture, &ev);
      break;
   }
   case GDK_SCROLL:
   {
      GimvEventScroll ev;
      GdkScrollDirection dir;

      memset (&ev, 0, sizeof (ev));
      ev.type = GDK_SCROLL;
      ev.time = gdk_event_get_time (event);
      ev.state = gdk_event_get_modifier_state (event);
      ev.event = event;
      ev.controller = controller;
      translate_coords (widget, event, &ev.x, &ev.y, NULL, NULL);

      dir = gdk_scroll_event_get_direction (event);
      if (dir == GDK_SCROLL_SMOOTH) {
         gdk_scroll_event_get_deltas (event, &ev.delta_x, &ev.delta_y);
         if (ev.delta_y < 0)
            dir = GDK_SCROLL_UP;
         else if (ev.delta_y > 0)
            dir = GDK_SCROLL_DOWN;
         else if (ev.delta_x < 0)
            dir = GDK_SCROLL_LEFT;
         else if (ev.delta_x > 0)
            dir = GDK_SCROLL_RIGHT;
         else
            break;  /* stop event */
      }
      ev.direction = dir;

      handled = dispatch (widget, ed, GIMV_EVENT_SCROLL, capture, &ev);
      break;
   }
   default:
      break;
   }

   return handled;
}


static gboolean
cb_legacy_event_capture (GtkEventControllerLegacy *controller, GdkEvent *event,
                         gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   return handle_legacy_event (widget, GTK_EVENT_CONTROLLER (controller), event, TRUE);
}


static gboolean
cb_legacy_event_bubble (GtkEventControllerLegacy *controller, GdkEvent *event,
                        gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   return handle_legacy_event (widget, GTK_EVENT_CONTROLLER (controller), event, FALSE);
}


static void
cb_motion_enter (GtkEventControllerMotion *controller, gdouble x, gdouble y,
                 gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   EventData *ed = get_event_data (widget, FALSE);
   GimvEventCrossing ev;

   if (!ed) return;

   ed->have_pointer = TRUE;
   ed->pointer_x = x;
   ed->pointer_y = y;

   ev.type = GDK_ENTER_NOTIFY;
   ev.x = x;
   ev.y = y;
   ev.state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (controller));
   dispatch (widget, ed, GIMV_EVENT_ENTER_NOTIFY, FALSE, &ev);
}


static void
cb_motion_leave (GtkEventControllerMotion *controller, gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   EventData *ed = get_event_data (widget, FALSE);
   GimvEventCrossing ev;

   if (!ed) return;

   ev.type = GDK_LEAVE_NOTIFY;
   ev.x = ed->pointer_x;
   ev.y = ed->pointer_y;
   ev.state = 0;
   dispatch (widget, ed, GIMV_EVENT_LEAVE_NOTIFY, FALSE, &ev);
}


static void
cb_motion_track (GtkEventControllerMotion *controller, gdouble x, gdouble y,
                 gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   EventData *ed = get_event_data (widget, FALSE);

   if (!ed) return;
   ed->have_pointer = TRUE;
   ed->pointer_x = x;
   ed->pointer_y = y;
}


static void
cb_focus_enter (GtkEventControllerFocus *controller, gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   EventData *ed = get_event_data (widget, FALSE);

   if (!ed) return;
   dispatch (widget, ed, GIMV_EVENT_FOCUS_IN, FALSE, NULL);
}


static void
cb_focus_leave (GtkEventControllerFocus *controller, gpointer data)
{
   GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
   EventData *ed = get_event_data (widget, FALSE);

   if (!ed) return;
   dispatch (widget, ed, GIMV_EVENT_FOCUS_OUT, FALSE, NULL);
}


static gboolean
cb_close_request (GtkWindow *window, gpointer data)
{
   EventData *ed = get_event_data (GTK_WIDGET (window), FALSE);

   if (!ed) return FALSE;
   return dispatch (GTK_WIDGET (window), ed, GIMV_EVENT_DELETE, FALSE, NULL);
}


static void
cb_surface_layout (GdkSurface *surface, gint width, gint height, GtkWidget *widget)
{
   EventData *ed = get_event_data (widget, FALSE);
   GimvEventConfigure ev;

   if (!ed) return;

   ev.x = ev.y = 0;
   ev.width = width;
   ev.height = height;
   dispatch (widget, ed, GIMV_EVENT_CONFIGURE, FALSE, &ev);
}


static void
cb_widget_realize_for_configure (GtkWidget *widget, gpointer data)
{
   EventData *ed = get_event_data (widget, FALSE);
   GtkNative *native = gtk_widget_get_native (widget);
   GdkSurface *surface;

   if (!ed || !native) return;

   surface = gtk_native_get_surface (native);
   if (!surface || surface == ed->surface) return;

   if (ed->surface && ed->layout_id)
      g_signal_handler_disconnect (ed->surface, ed->layout_id);

   ed->surface = surface;
   ed->layout_id = g_signal_connect_object (surface, "layout",
                                            G_CALLBACK (cb_surface_layout),
                                            widget, 0);
}


static void
ensure_controllers (GtkWidget *widget, EventData *ed,
                    GimvEventSignal signal, gboolean after)
{
   switch (signal) {
   case GIMV_EVENT_BUTTON_PRESS:
   case GIMV_EVENT_BUTTON_RELEASE:
   case GIMV_EVENT_MOTION_NOTIFY:
   case GIMV_EVENT_KEY_PRESS:
   case GIMV_EVENT_KEY_RELEASE:
   case GIMV_EVENT_SCROLL:
      if (!after && widget_wants_capture (widget, signal)) {
         if (!ed->legacy_capture) {
            ed->legacy_capture = gtk_event_controller_legacy_new ();
            gtk_event_controller_set_propagation_phase (ed->legacy_capture,
                                                        GTK_PHASE_CAPTURE);
            g_signal_connect (ed->legacy_capture, "event",
                              G_CALLBACK (cb_legacy_event_capture), NULL);
            gtk_widget_add_controller (widget, ed->legacy_capture);
         }
      } else {
         if (!ed->legacy_bubble) {
            ed->legacy_bubble = gtk_event_controller_legacy_new ();
            gtk_event_controller_set_propagation_phase (ed->legacy_bubble,
                                                        GTK_PHASE_BUBBLE);
            g_signal_connect (ed->legacy_bubble, "event",
                              G_CALLBACK (cb_legacy_event_bubble),
                              GINT_TO_POINTER (FALSE));
            gtk_widget_add_controller (widget, ed->legacy_bubble);
         }
      }
      break;

   case GIMV_EVENT_ENTER_NOTIFY:
   case GIMV_EVENT_LEAVE_NOTIFY:
      if (!ed->motion) {
         ed->motion = gtk_event_controller_motion_new ();
         g_signal_connect (ed->motion, "enter", G_CALLBACK (cb_motion_enter), NULL);
         g_signal_connect (ed->motion, "leave", G_CALLBACK (cb_motion_leave), NULL);
         g_signal_connect (ed->motion, "motion", G_CALLBACK (cb_motion_track), NULL);
         gtk_widget_add_controller (widget, ed->motion);
      }
      break;

   case GIMV_EVENT_FOCUS_IN:
   case GIMV_EVENT_FOCUS_OUT:
      if (!ed->focus) {
         ed->focus = gtk_event_controller_focus_new ();
         g_signal_connect (ed->focus, "enter", G_CALLBACK (cb_focus_enter), NULL);
         g_signal_connect (ed->focus, "leave", G_CALLBACK (cb_focus_leave), NULL);
         gtk_widget_add_controller (widget, ed->focus);
      }
      break;

   case GIMV_EVENT_DELETE:
      if (!ed->close_request_id) {
         g_return_if_fail (GTK_IS_WINDOW (widget));
         ed->close_request_id = g_signal_connect (widget, "close-request",
                                                  G_CALLBACK (cb_close_request),
                                                  NULL);
      }
      break;

   case GIMV_EVENT_CONFIGURE:
      if (!ed->realize_id) {
         ed->realize_id = g_signal_connect (widget, "realize",
                                            G_CALLBACK (cb_widget_realize_for_configure),
                                            NULL);
         if (gtk_widget_get_realized (widget))
            cb_widget_realize_for_configure (widget, NULL);
      }
      break;

   default:
      break;
   }
}


static gulong
event_connect_real (GtkWidget *widget, GimvEventSignal signal,
                    GCallback handler, gpointer data, gboolean after)
{
   EventData *ed;
   EventHandler *h;

   g_return_val_if_fail (GTK_IS_WIDGET (widget), 0);
   g_return_val_if_fail (handler, 0);

   ed = get_event_data (widget, TRUE);

   h = g_new0 (EventHandler, 1);
   h->signal  = signal;
   h->handler = handler;
   h->data    = data;
   /* GTK2 ran handlers before the widget's own class handler.  For widgets
      that handle input themselves (tree views, entries...) the closest
      equivalent is the capture phase; "after" handlers use the bubble phase */
   h->capture = (!after && widget_wants_capture (widget, signal)
                 && signal <= GIMV_EVENT_SCROLL);
   h->after   = after;
   h->id      = ++event_handler_id_seq;
   ed->handlers = g_list_append (ed->handlers, h);

   ensure_controllers (widget, ed, signal, after);

   return h->id;
}


gulong
gimv_event_connect (GtkWidget *widget, GimvEventSignal signal,
                    GCallback handler, gpointer data)
{
   return event_connect_real (widget, signal, handler, data, FALSE);
}


gulong
gimv_event_connect_after (GtkWidget *widget, GimvEventSignal signal,
                          GCallback handler, gpointer data)
{
   return event_connect_real (widget, signal, handler, data, TRUE);
}


void
gimv_event_disconnect_by_func (GtkWidget *widget, GCallback handler, gpointer data)
{
   EventData *ed;
   GList *node;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   ed = get_event_data (widget, FALSE);
   if (!ed) return;

   node = ed->handlers;
   while (node) {
      EventHandler *h = node->data;
      GList *next = g_list_next (node);

      if (h->handler == handler && h->data == data) {
         ed->handlers = g_list_delete_link (ed->handlers, node);
         g_free (h);
      }
      node = next;
   }
}


static void
event_block_real (GtkWidget *widget, GCallback handler, gpointer data, gint n)
{
   EventData *ed;
   GList *node;

   g_return_if_fail (GTK_IS_WIDGET (widget));

   ed = get_event_data (widget, FALSE);
   if (!ed) return;

   for (node = ed->handlers; node; node = g_list_next (node)) {
      EventHandler *h = node->data;
      if (h->handler == handler && h->data == data)
         h->blocked += n;
   }
}


void
gimv_event_block_by_func (GtkWidget *widget, GCallback handler, gpointer data)
{
   event_block_real (widget, handler, data, 1);
}


void
gimv_event_unblock_by_func (GtkWidget *widget, GCallback handler, gpointer data)
{
   event_block_real (widget, handler, data, -1);
}


GdkModifierType
gimv_get_current_modifier_state (GtkWidget *widget)
{
   GdkDisplay *display;
   GdkSeat *seat;
   GdkDevice *keyboard;

   display = widget ? gtk_widget_get_display (widget) : gdk_display_get_default ();
   if (!display) return 0;

   seat = gdk_display_get_default_seat (display);
   if (!seat) return 0;

   keyboard = gdk_seat_get_keyboard (seat);
   if (!keyboard) return 0;

   return gdk_device_get_modifier_state (keyboard);
}


gboolean
gimv_widget_get_pointer (GtkWidget *widget, gint *x, gint *y)
{
   GtkNative *native;
   GdkSurface *surface;
   GdkSeat *seat;
   GdkDevice *pointer;
   double sx, sy, nx = 0, ny = 0;
   graphene_point_t in, out;

   if (x) *x = 0;
   if (y) *y = 0;

   g_return_val_if_fail (GTK_IS_WIDGET (widget), FALSE);

   native = gtk_widget_get_native (widget);
   if (!native) return FALSE;
   surface = gtk_native_get_surface (native);
   if (!surface) return FALSE;

   seat = gdk_display_get_default_seat (gtk_widget_get_display (widget));
   pointer = seat ? gdk_seat_get_pointer (seat) : NULL;

   if (!pointer || !gdk_surface_get_device_position (surface, pointer, &sx, &sy, NULL)) {
      EventData *ed = get_event_data (widget, FALSE);
      if (ed && ed->have_pointer) {
         if (x) *x = ed->pointer_x;
         if (y) *y = ed->pointer_y;
         return TRUE;
      }
      return FALSE;
   }

   gtk_native_get_surface_transform (native, &nx, &ny);
   in.x = sx - nx;
   in.y = sy - ny;
   if (!gtk_widget_compute_point (GTK_WIDGET (native), widget, &in, &out))
      return FALSE;

   if (x) *x = out.x;
   if (y) *y = out.y;

   return TRUE;
}



/******************************************************************************
 *
 *   drawing helpers
 *
 ******************************************************************************/
GdkTexture *
gimv_texture_new_for_pixbuf (GdkPixbuf *pixbuf)
{
   g_return_val_if_fail (GDK_IS_PIXBUF (pixbuf), NULL);
   return gdk_texture_new_for_pixbuf (pixbuf);
}


#define TEXTURE_SURFACE_KEY "gimv-cairo-surface"

static cairo_surface_t *
texture_get_surface (GdkTexture *texture)
{
   cairo_surface_t *surface;
   gint w, h;

   surface = g_object_get_data (G_OBJECT (texture), TEXTURE_SURFACE_KEY);
   if (surface) return surface;

   w = gdk_texture_get_width (texture);
   h = gdk_texture_get_height (texture);
   surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
   /* GdkTexture's default download format equals CAIRO_FORMAT_ARGB32 */
   gdk_texture_download (texture,
                         cairo_image_surface_get_data (surface),
                         cairo_image_surface_get_stride (surface));
   cairo_surface_mark_dirty (surface);

   g_object_set_data_full (G_OBJECT (texture), TEXTURE_SURFACE_KEY, surface,
                           (GDestroyNotify) cairo_surface_destroy);

   return surface;
}


void
gimv_cairo_draw_texture (cairo_t *cr, GdkTexture *texture, gdouble x, gdouble y)
{
   g_return_if_fail (cr);
   g_return_if_fail (GDK_IS_TEXTURE (texture));

   cairo_save (cr);
   cairo_set_source_surface (cr, texture_get_surface (texture), x, y);
   cairo_rectangle (cr, x, y,
                    gdk_texture_get_width (texture),
                    gdk_texture_get_height (texture));
   cairo_fill (cr);
   cairo_restore (cr);
}


void
gimv_cairo_draw_pixbuf (cairo_t *cr, GdkPixbuf *pixbuf, gdouble x, gdouble y)
{
   g_return_if_fail (cr);
   g_return_if_fail (GDK_IS_PIXBUF (pixbuf));

   cairo_save (cr);
   gdk_cairo_set_source_pixbuf (cr, pixbuf, x, y);
   cairo_rectangle (cr, x, y,
                    gdk_pixbuf_get_width (pixbuf),
                    gdk_pixbuf_get_height (pixbuf));
   cairo_fill (cr);
   cairo_restore (cr);
}


void
gimv_cairo_set_source_color_name (cairo_t *cr, const gchar *spec)
{
   GdkRGBA rgba;

   if (!gdk_rgba_parse (&rgba, spec))
      gdk_rgba_parse (&rgba, "black");
   gdk_cairo_set_source_rgba (cr, &rgba);
}


static gboolean
lookup_color (GtkWidget *widget, const gchar *name, GdkRGBA *color)
{
   GtkStyleContext *context;

   if (!widget) return FALSE;
   context = gtk_widget_get_style_context (widget);
   return context && gtk_style_context_lookup_color (context, name, color);
}


void
gimv_widget_get_fg_color (GtkWidget *widget, GdkRGBA *color)
{
   g_return_if_fail (color);
   if (widget)
      gtk_widget_get_color (widget, color);
   else
      gdk_rgba_parse (color, "black");
}


void
gimv_widget_get_selected_bg_color (GtkWidget *widget, GdkRGBA *color)
{
   g_return_if_fail (color);
   if (!lookup_color (widget, "theme_selected_bg_color", color)
       && !lookup_color (widget, "accent_bg_color", color))
      gdk_rgba_parse (color, "#ccccff");
}


void
gimv_widget_get_base_color (GtkWidget *widget, GdkRGBA *color)
{
   g_return_if_fail (color);
   if (!lookup_color (widget, "theme_base_color", color)
       && !lookup_color (widget, "view_bg_color", color))
      gdk_rgba_parse (color, "white");
}


GdkPixbuf *
gimv_pixbuf_from_texture (GdkTexture *texture)
{
   GdkPixbuf *pixbuf;
   cairo_surface_t *surface;

   g_return_val_if_fail (GDK_IS_TEXTURE (texture), NULL);

   surface = texture_get_surface (texture);
   pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0,
                                         gdk_texture_get_width (texture),
                                         gdk_texture_get_height (texture));
   return pixbuf;
}


GdkPixbuf *
gimv_widget_render_icon (GtkWidget *widget, const gchar *icon_name, gint size)
{
   GtkIconTheme *theme;
   GtkIconPaintable *paintable;
   GFile *file;
   GdkPixbuf *pixbuf = NULL;
   GdkDisplay *display;

   display = widget ? gtk_widget_get_display (widget) : gdk_display_get_default ();
   if (!display) return NULL;

   theme = gtk_icon_theme_get_for_display (display);
   paintable = gtk_icon_theme_lookup_icon (theme, icon_name, NULL, size, 1,
                                           GTK_TEXT_DIR_LTR, 0);
   if (!paintable) return NULL;

   file = gtk_icon_paintable_get_file (paintable);
   if (file) {
      gchar *path = g_file_get_path (file);
      if (path)
         pixbuf = gdk_pixbuf_new_from_file_at_size (path, size, size, NULL);
      g_free (path);
      g_object_unref (file);
   }
   g_object_unref (paintable);

   return pixbuf;
}



/******************************************************************************
 *
 *   misc
 *
 ******************************************************************************/
void
gimv_widget_set_tooltip (GtkWidget *widget, const gchar *text)
{
   gtk_widget_set_tooltip_text (widget, text);
}


void
gimv_beep (void)
{
   GdkDisplay *display = gdk_display_get_default ();
   if (display) gdk_display_beep (display);
}


GdkDisplay *
gimv_display (void)
{
   return gdk_display_get_default ();
}


void
gimv_screen_get_size (gint *width, gint *height)
{
   GdkDisplay *display = gdk_display_get_default ();
   GListModel *monitors;
   GdkMonitor *monitor;
   GdkRectangle geom = { 0, 0, 1024, 768 };

   if (display) {
      monitors = gdk_display_get_monitors (display);
      if (g_list_model_get_n_items (monitors) > 0) {
         monitor = g_list_model_get_item (monitors, 0);
         gdk_monitor_get_geometry (monitor, &geom);
         g_object_unref (monitor);
      }
   }

   if (width)  *width  = geom.width;
   if (height) *height = geom.height;
}


void
gimv_clipboard_set_text (const gchar *text)
{
   GdkDisplay *display = gdk_display_get_default ();
   if (!display) return;
   gdk_clipboard_set_text (gdk_display_get_clipboard (display), text);
}


/******************************************************************************
 *
 *   wider column resize handles for GtkTreeView headers
 *
 ******************************************************************************/
#define COLUMN_RESIZE_HANDLE   5     /* pixels on each side of a separator */
#define COLUMN_RESIZE_MIN      8
#define COLUMN_RESIZE_KEY      "gimv-column-resize"

typedef struct {
   GtkTreeView       *treeview;
   GtkTreeViewColumn *column;       /* column being resized */
   gint               start_width;
   GtkWidget         *cursor_widget; /* header button with our cursor */
} ColumnResize;


/* the resizable column whose right header edge is near (x, y), in tree view
   coordinates; NULL if (x, y) is not on the header row near a separator */
static GtkTreeViewColumn *
column_resize_find (GtkTreeView *treeview, gdouble x, gdouble y,
                    GtkWidget **button_under_pointer)
{
   GtkTreeViewColumn *found = NULL;
   gdouble best = COLUMN_RESIZE_HANDLE + 1;
   GList *columns, *node;

   if (button_under_pointer) *button_under_pointer = NULL;
   if (!gtk_tree_view_get_headers_visible (treeview)) return NULL;

   columns = gtk_tree_view_get_columns (treeview);
   for (node = columns; node; node = g_list_next (node)) {
      GtkTreeViewColumn *col = node->data;
      GtkWidget *button = gtk_tree_view_column_get_button (col);
      graphene_rect_t b;
      gdouble edge, dist;

      if (!gtk_tree_view_column_get_visible (col) || !button
          || !gtk_widget_get_realized (button)
          || !gtk_widget_compute_bounds (button, GTK_WIDGET (treeview), &b))
      {
         continue;
      }
      if (y < b.origin.y || y >= b.origin.y + b.size.height)
         continue;

      if (button_under_pointer
          && x >= b.origin.x && x < b.origin.x + b.size.width)
      {
         *button_under_pointer = button;
      }

      if (!gtk_tree_view_column_get_resizable (col)) continue;

      edge = b.origin.x + b.size.width;
      dist = fabs (x - edge);
      if (dist <= COLUMN_RESIZE_HANDLE && dist < best) {
         best = dist;
         found = col;
      }
   }
   g_list_free (columns);

   return found;
}


/* "col-resize" with the core X cursor as fallback: on X11 GDK finds
   col-resize only in cursor themes that have it (or h_double_arrow);
   without one the cursor silently stays unchanged */
static GdkCursor *
column_resize_cursor (void)
{
   static GdkCursor *cursor = NULL;

   if (!cursor) {
      GdkCursor *core = gdk_cursor_new_from_name ("sb_h_double_arrow", NULL);
      cursor = gdk_cursor_new_from_name ("col-resize", core);
      g_object_unref (core);
   }
   return cursor;
}


static void
column_resize_set_cursor (ColumnResize *cr, GtkWidget *button)
{
   /* the cursor is set on the header button under the pointer: the tree
      view resets its own cursor on every motion outside its 6 pixels */
   if (cr->cursor_widget && cr->cursor_widget != button) {
      gtk_widget_set_cursor (cr->cursor_widget, NULL);
      g_object_remove_weak_pointer (G_OBJECT (cr->cursor_widget),
                                    (gpointer *) &cr->cursor_widget);
      cr->cursor_widget = NULL;
   }
   if (button && cr->cursor_widget != button) {
      gtk_widget_set_cursor (button, column_resize_cursor ());
      cr->cursor_widget = button;
      g_object_add_weak_pointer (G_OBJECT (button),
                                 (gpointer *) &cr->cursor_widget);
   }
}


static void
cb_column_resize_motion (GtkEventControllerMotion *motion,
                         gdouble x, gdouble y, ColumnResize *cr)
{
   GtkWidget *button = NULL;
   GtkTreeViewColumn *col;

   if (cr->column) return;   /* dragging */

   col = column_resize_find (cr->treeview, x, y, &button);
   column_resize_set_cursor (cr, col ? button : NULL);
}


static void
cb_column_resize_leave (GtkEventControllerMotion *motion, ColumnResize *cr)
{
   if (!cr->column)
      column_resize_set_cursor (cr, NULL);
}


static void
cb_column_resize_begin (GtkGestureDrag *gesture, gdouble x, gdouble y,
                        ColumnResize *cr)
{
   GtkTreeViewColumn *col = column_resize_find (cr->treeview, x, y, NULL);

   if (!col) {
      gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_DENIED);
      return;
   }

   /* claim it before the header click (sorting), column dragging and our
      own DnD source see the press */
   gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
   cr->column = col;
   cr->start_width = gtk_tree_view_column_get_width (col);
}


static void
cb_column_resize_update (GtkGestureDrag *gesture, gdouble dx, gdouble dy,
                         ColumnResize *cr)
{
   gint width;

   if (!cr->column) return;

   if (gtk_widget_get_direction (GTK_WIDGET (cr->treeview)) == GTK_TEXT_DIR_RTL)
      dx = -dx;
   width = MAX (cr->start_width + (gint) dx, COLUMN_RESIZE_MIN);
   if (width != gtk_tree_view_column_get_fixed_width (cr->column))
      gtk_tree_view_column_set_fixed_width (cr->column, width);
}


static void
cb_column_resize_end (GtkGesture *gesture, GdkEventSequence *sequence,
                      ColumnResize *cr)
{
   cr->column = NULL;
}


static void
column_resize_free (ColumnResize *cr)
{
   if (cr->cursor_widget)
      g_object_remove_weak_pointer (G_OBJECT (cr->cursor_widget),
                                    (gpointer *) &cr->cursor_widget);
   g_free (cr);
}


void
gimv_tree_view_widen_column_resize (GtkTreeView *treeview)
{
   ColumnResize *cr;
   GtkEventController *motion;
   GtkGesture *drag;

   g_return_if_fail (GTK_IS_TREE_VIEW (treeview));

   if (g_object_get_data (G_OBJECT (treeview), COLUMN_RESIZE_KEY)) return;

   cr = g_new0 (ColumnResize, 1);
   cr->treeview = treeview;
   g_object_set_data_full (G_OBJECT (treeview), COLUMN_RESIZE_KEY, cr,
                           (GDestroyNotify) column_resize_free);

   motion = gtk_event_controller_motion_new ();
   gtk_event_controller_set_propagation_phase (motion, GTK_PHASE_CAPTURE);
   g_signal_connect (motion, "motion", G_CALLBACK (cb_column_resize_motion), cr);
   g_signal_connect (motion, "leave", G_CALLBACK (cb_column_resize_leave), cr);
   gtk_widget_add_controller (GTK_WIDGET (treeview), motion);

   drag = gtk_gesture_drag_new ();
   gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), GDK_BUTTON_PRIMARY);
   gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag),
                                               GTK_PHASE_CAPTURE);
   g_signal_connect (drag, "drag-begin", G_CALLBACK (cb_column_resize_begin), cr);
   g_signal_connect (drag, "drag-update", G_CALLBACK (cb_column_resize_update), cr);
   g_signal_connect (drag, "end", G_CALLBACK (cb_column_resize_end), cr);
   gtk_widget_add_controller (GTK_WIDGET (treeview), GTK_EVENT_CONTROLLER (drag));
}
