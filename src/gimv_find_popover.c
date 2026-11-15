/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: Ctrl+F on a view of items (not a GimvListView: that has its
 * own): a search entry in a popover at its top right.  It works as the one
 * of the lists (gimv_list_view.c): prefix first, else a part; Up and Down,
 * Enter, Escape; closed after 5 seconds without a key.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimv_find_popover.h"

#include <string.h>

#define FIND_KEY     "gimv-find-popover"
#define FIND_TIMEOUT 5     /* seconds, as the lists */

typedef struct {
   GtkWidget     *view;     /* weak */
   GtkWidget     *anchor;   /* weak */
   GtkWidget     *popover;
   GtkWidget     *entry;
   GimvFindFuncs  funcs;
   gpointer       data;
   guint          found;
   guint          idle_id;
   guint          focus_id;
} Find;


static gchar *
fold (const gchar *text)
{
   gchar *norm = text ? g_utf8_normalize (text, -1, G_NORMALIZE_ALL) : NULL;
   gchar *ret = norm ? g_utf8_casefold (norm, -1) : NULL;
   g_free (norm);
   return ret;
}


/* from start (itself too unless again), around; prefix first, else a part */
static gboolean
find_from (Find *f, guint start, gboolean forward, gboolean again)
{
   gchar *key = fold (gtk_editable_get_text (GTK_EDITABLE (f->entry)));
   guint n = f->funcs.count (f->data), i, pass;
   gboolean found = FALSE;

   if (!key || !*key || !n) {
      g_free (key);
      return FALSE;
   }
   if (start >= n) start = 0;
   for (pass = 0; pass < 2 && !found; pass++) {
      for (i = again ? 1 : 0; i <= n && !found; i++) {
         guint pos = forward ? (start + i) % n : (start + n - (i % n)) % n;
         gchar *text = f->funcs.text (pos, f->data), *t = fold (text);
         if (t && (pass == 0 ? g_str_has_prefix (t, key) : strstr (t, key) != NULL)) {
            found = TRUE;
            f->found = pos;
         }
         g_free (t);
         g_free (text);
         if (i == n && !again) break;
      }
   }
   g_free (key);

   if (found) {
      f->funcs.select (f->found, f->data);
      gtk_widget_remove_css_class (f->entry, "error");
   } else {
      gtk_widget_add_css_class (f->entry, "error");
   }
   return found;
}


static gboolean
find_timeout (gpointer data)
{
   Find *f = data;
   f->idle_id = 0;
   gtk_popover_popdown (GTK_POPOVER (f->popover));
   return G_SOURCE_REMOVE;
}


static void
find_touch (Find *f)
{
   if (f->idle_id) g_source_remove (f->idle_id);
   f->idle_id = g_timeout_add_seconds (FIND_TIMEOUT, find_timeout, f);
}


static guint
find_current (Find *f)
{
   if (f->found != GIMV_FIND_NONE) return f->found;
   return f->funcs.current ? f->funcs.current (f->data) : GIMV_FIND_NONE;
}


static void
cb_changed (GtkSearchEntry *entry, Find *f)
{
   guint cur = find_current (f);
   find_touch (f);
   find_from (f, cur == GIMV_FIND_NONE ? 0 : cur, TRUE, FALSE);
}


static void
cb_activate (GtkSearchEntry *entry, Find *f)
{
   guint pos = find_current (f);

   gtk_popover_popdown (GTK_POPOVER (f->popover));
   if (pos != GIMV_FIND_NONE && f->funcs.activate)
      f->funcs.activate (pos, f->data);
}


static gboolean
cb_entry_key (GtkEventControllerKey *ctrl, guint keyval, guint code,
              GdkModifierType state, Find *f)
{
   guint cur = find_current (f);

   find_touch (f);
   if (cur == GIMV_FIND_NONE) cur = 0;
   if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
      find_from (f, cur, TRUE, TRUE);
      return TRUE;
   }
   if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
      find_from (f, cur, FALSE, TRUE);
      return TRUE;
   }
   if (keyval == GDK_KEY_Escape) {
      gtk_popover_popdown (GTK_POPOVER (f->popover));
      return TRUE;
   }
   /* the text, not the window's Ctrl+A, Ctrl+F */
   if ((keyval == GDK_KEY_a || keyval == GDK_KEY_A || keyval == GDK_KEY_f || keyval == GDK_KEY_F)
       && (state & GDK_CONTROL_MASK))
   {
      gtk_editable_select_region (GTK_EDITABLE (f->entry), 0, -1);
      return TRUE;
   }
   return FALSE;
}


/* after the popover has given the focus back where it was */
static gboolean
find_refocus (gpointer data)
{
   Find *f = data;

   f->focus_id = 0;
   if (f->view && gtk_widget_get_mapped (f->view))
      gtk_widget_grab_focus (f->view);
   return G_SOURCE_REMOVE;
}


static void
cb_closed (GtkPopover *popover, Find *f)
{
   if (f->idle_id) {
      g_source_remove (f->idle_id);
      f->idle_id = 0;
   }
   if (!f->focus_id)
      f->focus_id = g_timeout_add (50, find_refocus, f);
}


static void
find_start (Find *f)
{
   if (!f->view) return;
   if (!f->popover) {
      /* the parent of the view as it is now, if none was given */
      if (!f->anchor) {
         f->anchor = gtk_widget_get_parent (f->view);
         if (!f->anchor) f->anchor = f->view;
         g_object_add_weak_pointer (G_OBJECT (f->anchor), (gpointer *) &f->anchor);
      }
      GtkEventController *keys;

      f->entry = gtk_search_entry_new ();
      gtk_editable_set_width_chars (GTK_EDITABLE (f->entry), 16);
      g_signal_connect (f->entry, "search-changed", G_CALLBACK (cb_changed), f);
      g_signal_connect (f->entry, "activate", G_CALLBACK (cb_activate), f);
      keys = gtk_event_controller_key_new ();
      gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_entry_key), f);
      gtk_widget_add_controller (f->entry, keys);

      f->popover = gtk_popover_new ();
      gtk_popover_set_child (GTK_POPOVER (f->popover), f->entry);
      gtk_popover_set_has_arrow (GTK_POPOVER (f->popover), FALSE);
      gtk_popover_set_position (GTK_POPOVER (f->popover), GTK_POS_BOTTOM);
      gtk_widget_set_parent (f->popover, f->anchor);
      g_signal_connect (f->popover, "closed", G_CALLBACK (cb_closed), f);
   }
   if (!f->anchor) return;
   {
      /* at the top right of the view */
      GdkRectangle rect = { MAX (gtk_widget_get_width (f->anchor) - 1, 0), 0, 1, 1 };
      gtk_popover_set_pointing_to (GTK_POPOVER (f->popover), &rect);
   }
   f->found = GIMV_FIND_NONE;
   g_signal_handlers_block_by_func (f->entry, cb_changed, f);
   gtk_editable_set_text (GTK_EDITABLE (f->entry), "");
   g_signal_handlers_unblock_by_func (f->entry, cb_changed, f);
   gtk_widget_remove_css_class (f->entry, "error");
   gtk_popover_popup (GTK_POPOVER (f->popover));
   gtk_widget_grab_focus (f->entry);
   find_touch (f);
}


/* Ctrl+F on the view (before the window's keys: [File | Open...]) */
static gboolean
cb_view_key (GtkEventControllerKey *ctrl, guint keyval, guint code,
             GdkModifierType state, Find *f)
{
   if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F)
       && (state & GDK_MODIFIER_MASK) == GDK_CONTROL_MASK)
   {
      find_start (f);
      return TRUE;
   }
   return FALSE;
}


static void
find_free (Find *f)
{
   if (f->idle_id) g_source_remove (f->idle_id);
   if (f->focus_id) g_source_remove (f->focus_id);
   if (f->view) g_object_remove_weak_pointer (G_OBJECT (f->view), (gpointer *) &f->view);
   if (f->anchor) {
      g_object_remove_weak_pointer (G_OBJECT (f->anchor), (gpointer *) &f->anchor);
      if (f->popover && gtk_widget_get_parent (f->popover) == f->anchor)
         gtk_widget_unparent (f->popover);
   }
   g_free (f);
}


void
gimv_find_popover_attach (GtkWidget *view, GtkWidget *anchor,
                          const GimvFindFuncs *funcs, gpointer data)
{
   Find *f;
   GtkEventController *keys;

   g_return_if_fail (GTK_IS_WIDGET (view));
   g_return_if_fail (funcs && funcs->count && funcs->text && funcs->select);
   if (g_object_get_data (G_OBJECT (view), FIND_KEY)) return;

   f = g_new0 (Find, 1);
   f->view = view;
   g_object_add_weak_pointer (G_OBJECT (view), (gpointer *) &f->view);
   f->anchor = anchor;
   if (anchor) g_object_add_weak_pointer (G_OBJECT (anchor), (gpointer *) &f->anchor);
   f->funcs = *funcs;
   f->data = data;
   f->found = GIMV_FIND_NONE;
   g_object_set_data_full (G_OBJECT (view), FIND_KEY, f, (GDestroyNotify) find_free);

   keys = gtk_event_controller_key_new ();
   g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_view_key), f);
   gtk_widget_add_controller (view, keys);
}
