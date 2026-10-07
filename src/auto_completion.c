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
 * $Id: auto_completion.c,v 1.3 2003/06/13 09:43:23 makeinu Exp $
 */

/*
 * These codes are mostly taken from gThumb.
 * gThumb code Copyright (C) 2001 The Free Software Foundation, Inc.
 * gThumb author: Paolo Bacchilega
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#include "auto_completion.h"
#include "charset.h"
#include "fileutil.h"
#include "gfileutil.h"
#include "gtk2-compat.h"
#include "prefs.h"

#define MAX_VISIBLE_ROWS 8
#define CLIST_ROW_PAD    5

static gchar *ac_dir            = NULL;
static gchar *ac_path           = NULL;
static gchar  ac_show_dot       = FALSE;
static GList *ac_subdirs        = NULL; 
static GList *ac_alternatives   = NULL;

static GtkWidget *ac_window     = NULL;
static GtkWidget *ac_clist      = NULL;
static GtkWidget *ac_entry      = NULL;
static GtkListStore *ac_list_store = NULL;

static void 
ac_dir_free (void)
{
   if (!ac_dir) return;

   g_free (ac_dir);
   ac_dir = NULL;
}


static void 
ac_path_free (void)
{
   if (!ac_path) return;

   g_free (ac_path);
   ac_path = NULL;
}


static void 
ac_subdirs_free (void)
{
   if (!ac_subdirs) return;

   g_list_foreach (ac_subdirs, (GFunc) g_free, NULL);
   g_list_free (ac_subdirs);
   ac_subdirs = NULL;
}


static void 
ac_alternatives_free (void)
{
   if (!ac_alternatives) return;

   g_list_foreach (ac_alternatives, (GFunc) g_free, NULL);
   g_list_free (ac_alternatives);
   ac_alternatives = NULL;
}


void
auto_compl_reset (void) 
{
   ac_dir_free ();
   ac_path_free ();
   ac_subdirs_free ();
   ac_alternatives_free ();
}


gint
auto_compl_get_n_alternatives (const gchar *path)
{
   gchar *dir;
   const gchar *filename;
   gint path_len;
   GList *scan;
   gint n;
   gint flags = GETDIR_FOLLOW_SYMLINK;
   gboolean show_dot;

   if (path == NULL) return 0;

   filename = strrchr (path, '/');
   filename = filename ? filename + 1 : path;
   if (filename && filename[0] == '.') {
      show_dot = TRUE;
      flags = flags | GETDIR_READ_DOT;
   } else {
      show_dot = FALSE;
   }

   if (strcmp (path, "/") == 0)
      dir = g_strdup ("/");
   else
      dir = g_path_get_dirname (path);

   if (!isdir (dir)) {
      g_free (dir);
      return 0;
   }

   if ((ac_dir == NULL) || strcmp (dir, ac_dir) || ac_show_dot != show_dot) {
      ac_dir_free ();
      ac_subdirs_free ();

      ac_dir = charset_to_internal (dir,
                                    conf.charset_filename,
                                    conf.charset_auto_detect_fn,
                                    conf.charset_filename_mode);
      if (!ac_dir && dir)
         ac_dir = g_strdup (dir);

      get_dir (dir, flags, NULL, &ac_subdirs);
      if (ac_show_dot != show_dot)
         ac_show_dot = show_dot;
   }

   ac_path_free ();
   ac_alternatives_free ();

   ac_path = g_strdup (path);
   path_len = strlen (ac_path);
   n = 0;

   for (scan = ac_subdirs; scan; scan = scan->next) {
      const gchar *subdir = (gchar*) scan->data;
      gchar *subdir_internal;

      subdir_internal = charset_to_internal (subdir,
                                             conf.charset_filename,
                                             conf.charset_auto_detect_fn,
                                             conf.charset_filename_mode);

      if (!subdir_internal && subdir)
         subdir_internal = g_strdup (subdir);

      if (strncmp (path, subdir_internal, path_len) != 0) {
         g_free (subdir_internal);
         continue;
      }

      ac_alternatives = g_list_prepend (ac_alternatives, 
                                        subdir_internal);

      n++;
   }

   g_free (dir);
   ac_alternatives = g_list_reverse (ac_alternatives);

   return n;
}


static gint
get_common_prefix_length (void) 
{
   gint n;
   GList *scan;
   gchar c1, c2;

   g_return_val_if_fail (ac_path != NULL, 0);
   g_return_val_if_fail (ac_alternatives != NULL, 0);

   /* if the number of alternatives is 1 return its length. */
   if (ac_alternatives->next == NULL)
      return strlen ((gchar*) ac_alternatives->data);

   n = strlen (ac_path);
   while (TRUE) {
      scan = ac_alternatives;

      c1 = ((gchar*) scan->data) [n];

      if (c1 == 0)
         return n;

      /* check that all other alternatives have the same 
       * character at position n. */

      scan = scan->next;
      
      for (; scan; scan = scan->next) {
         c2 = ((gchar*) scan->data) [n];
         if (c1 != c2)
            return n;
      }

      n++;
   }

   return -1;
}


gchar *
auto_compl_get_common_prefix (void) 
{
   gchar *alternative;
   gint n;

   if (ac_path == NULL)
      return NULL;

   if (ac_alternatives == NULL)
      return NULL;

   n = get_common_prefix_length ();
   alternative = (gchar*) ac_alternatives->data;

   return g_strndup (alternative, n);
}


GList * 
auto_compl_get_alternatives (void)
{
   return ac_alternatives;
}


/*
 *  GTK4: the list of alternatives was a popup window placed under the entry
 *  with pointer and keyboard grabs.  Neither window positioning nor grabs
 *  exist any more, so a GtkPopover attached to the entry is used instead
 *  (it is closed automatically when clicking outside of it).
 */
static gboolean
ac_window_key_press_cb (GtkEventControllerKey *controller,
                        guint keyval,
                        guint keycode,
                        GdkModifierType state,
                        gpointer data)
{
   GtkWidget *entry = ac_entry;

   if (keyval == GDK_KEY_Escape) {
      auto_compl_hide_alternatives ();
      return TRUE;
   }

   /* allow keyboard navigation in the alternatives clist */
   if (keyval == GDK_KEY_Up
       || keyval == GDK_KEY_Down
       || keyval == GDK_KEY_Page_Up
       || keyval == GDK_KEY_Page_Down
       || keyval == GDK_KEY_space)
      return FALSE;

   if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      /* the selected row is already in the entry (see cursor_changed) */
      auto_compl_hide_alternatives ();
      return TRUE;
   }

   auto_compl_hide_alternatives ();

   /* pass the key to the entry */
   if (entry) {
      GtkWidget *target = entry;

      if (GTK_IS_EDITABLE (entry)) {
         GtkEditable *delegate = gtk_editable_get_delegate (GTK_EDITABLE (entry));
         if (delegate) target = GTK_WIDGET (delegate);
      }
      if (GTK_IS_ENTRY (entry))
         gtk_entry_grab_focus_without_selecting (GTK_ENTRY (entry));
      else
         gtk_widget_grab_focus (entry);
      gtk_event_controller_key_forward (controller, target);
   }

   return TRUE;
}


static void
cb_ac_popover_closed (GtkPopover *popover, gpointer data)
{
   if (ac_entry && GTK_IS_ENTRY (ac_entry))
      gtk_entry_grab_focus_without_selecting (GTK_ENTRY (ac_entry));
}


static void ac_window_destroy (void);

static void
cb_ac_entry_destroy (GtkWidget *entry, gpointer data)
{
   if (entry == ac_entry)
      ac_window_destroy ();
}


static void
ac_window_destroy (void)
{
   if (ac_entry)
      g_signal_handlers_disconnect_by_func (ac_entry,
                                            (gpointer) cb_ac_entry_destroy,
                                            NULL);
   if (ac_window)
      gtk_widget_unparent (ac_window);   /* destroys the popover */

   if (ac_list_store)
      g_object_unref (ac_list_store);

   ac_window = NULL;
   ac_clist = NULL;
   ac_list_store = NULL;
   ac_entry = NULL;
}


static const gchar *
ac_basename (const gchar *path)
{
   const gchar *p = strrchr (path, '/');
   return p ? p + 1 : path;
}


static void
cb_tree_cursor_changed (GtkTreeView *treeview, gpointer data)
{
   GtkTreeSelection *selection;
   GtkTreeModel *model;
   GtkTreeIter iter;
   gchar *text, *full_path;

   /* GTK4: also emitted while the tree view is disposed (without model) */
   if (!gtk_tree_view_get_model (treeview)) return;

   g_return_if_fail (GTK_IS_TREE_VIEW (treeview));

   if (!ac_entry) return;

   selection = gtk_tree_view_get_selection (treeview);
   if (!gtk_tree_selection_get_selected (selection, &model, &iter)) return;
   gtk_tree_model_get (model, &iter,
                       0, &text,
                       -1);
   if (!text) return;

   full_path = g_strconcat (ac_dir, "/", text, NULL);
   gtk_editable_set_text (GTK_EDITABLE (ac_entry), full_path);

   g_free (text);
   g_free (full_path);

   gtk_editable_set_position (GTK_EDITABLE (ac_entry), -1);
}


/* displays a list of alternatives under the entry widget. */
void
auto_compl_show_alternatives (GtkWidget *entry)
{
   GList *scan;
   gint n;

   g_return_if_fail (GTK_IS_WIDGET (entry));

   /* the popover is attached to an entry: recreate it for another entry */
   if (ac_window && ac_entry != entry)
      ac_window_destroy ();

   if (ac_window == NULL) {
      GtkWidget *scroll;
      GtkEventController *key;

      ac_window = gtk_popover_new ();
      gtk_popover_set_has_arrow (GTK_POPOVER (ac_window), FALSE);
      gtk_popover_set_position (GTK_POPOVER (ac_window), GTK_POS_BOTTOM);
      gtk_popover_set_autohide (GTK_POPOVER (ac_window), TRUE);

      {
         GtkTreeViewColumn *col;
         GtkCellRenderer *render;

         ac_list_store = gtk_list_store_new (1, G_TYPE_STRING);
         ac_clist = gtk_tree_view_new_with_model (GTK_TREE_MODEL (ac_list_store));
         /* GTK4: gtk_tree_view_set_rules_hint () removed */
         gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (ac_clist), FALSE);
         gtk_tree_view_set_enable_search (GTK_TREE_VIEW (ac_clist), FALSE);

         col = gtk_tree_view_column_new();
         render = gtk_cell_renderer_text_new ();
         gtk_tree_view_column_pack_start (col, render, FALSE);
         gtk_tree_view_column_add_attribute (col, render, "text", 0);

         gtk_tree_view_append_column (GTK_TREE_VIEW (ac_clist), col);
      }

      scroll = gimv_scrolled_window_new (NULL, NULL);
      gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                      GTK_POLICY_AUTOMATIC,
                                      GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);

      gtk_popover_set_child (GTK_POPOVER (ac_window), scroll);
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), ac_clist);

      key = gtk_event_controller_key_new ();
      gtk_event_controller_set_propagation_phase (key, GTK_PHASE_CAPTURE);
      g_signal_connect (key, "key-pressed",
                        G_CALLBACK (ac_window_key_press_cb), NULL);
      gtk_widget_add_controller (ac_window, key);

      g_signal_connect (ac_window, "closed",
                        G_CALLBACK (cb_ac_popover_closed), NULL);

      g_signal_connect (G_OBJECT (ac_clist),
                        "cursor_changed",
                        G_CALLBACK (cb_tree_cursor_changed),
                        NULL);

      gtk_widget_set_parent (ac_window, entry);
      g_signal_connect (entry, "destroy",
                        G_CALLBACK (cb_ac_entry_destroy), NULL);
   }

   ac_entry = entry;
   n = 0;

   {
      GtkTreeIter iter;

      gtk_list_store_clear (ac_list_store);

      for (scan = ac_alternatives; scan; scan = scan->next) {
         gtk_list_store_append (ac_list_store, &iter);
         gtk_list_store_set (ac_list_store, &iter,
                             0, ac_basename (scan->data),
                             -1);

         if (n == 0) {
            GtkTreeSelection *selection;
            GtkTreePath *treepath;
            selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ac_clist));
            treepath = gtk_tree_model_get_path (GTK_TREE_MODEL (ac_list_store),
                                                &iter);
            gtk_tree_selection_select_path (selection, treepath);
            gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (ac_clist),
                                          treepath, NULL,
                                          TRUE, 0.0, 0.0);
            gtk_tree_path_free (treepath);
         }

         n++;
      }
   }

   /* GTK4: the popover is placed under the entry (no window positioning) */
   gtk_widget_set_size_request (ac_window,
                                MAX (gtk_widget_get_width (entry), 100), -1);
   gtk_widget_set_size_request (gtk_popover_get_child (GTK_POPOVER (ac_window)),
                                -1, 200);
   gtk_popover_popup (GTK_POPOVER (ac_window));

   /* focusing the tree view may move its cursor: don't touch the entry */
   g_signal_handlers_block_by_func (ac_clist, (gpointer) cb_tree_cursor_changed,
                                    NULL);
   gtk_widget_grab_focus (ac_clist);
   g_signal_handlers_unblock_by_func (ac_clist,
                                      (gpointer) cb_tree_cursor_changed, NULL);
}


void
auto_compl_hide_alternatives (void)
{
   if (ac_window && gtk_widget_get_visible (GTK_WIDGET (ac_window)))
      gtk_popover_popdown (GTK_POPOVER (ac_window));
}
