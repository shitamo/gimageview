/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the "Date" (and later "Tag") tree shown below the
 * directory tree.  Years, months and days come from the index database;
 * selecting one opens its images in a collection tab.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>

#include "fileload.h"
#include "gimv_index.h"
#include "gimv_index_view.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"

typedef enum {
   KIND_DATE_ROOT,
   KIND_YEAR,
   KIND_MONTH,
   KIND_DAY,
   KIND_DUMMY,
} RowKind;

enum {
   COL_LABEL,
   COL_KIND,
   COL_KEY,        /* "2019", "2019-05", "2019-05-03" */
   N_COLS
};

typedef struct {
   GimvThumbWin  *tw;
   GtkWidget     *scroll;
   GtkWidget     *tree;
   GtkTreeStore  *store;
   guint          watch_id;
   guint          open_id;
   GimvThumbView *tv;          /* the tab of the last result, weak */
   gboolean       filling;
} IndexView;


/******************************************************************************
 *  database queries
 ******************************************************************************/

/* "date" is "YYYY-MM-DD HH:MM:SS": every date starting with key sorts in
 * [key, key + "~") */
static void
add_children (IndexView *iv, GtkTreeIter *parent, RowKind kind, const gchar *key)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   gint len;
   const gchar *sql;

   if (!db) return;

   switch (kind) {
   case KIND_YEAR:  len = 4;  break;
   case KIND_MONTH: len = 7;  break;
   case KIND_DAY:   len = 10; break;
   default: return;
   }

   if (key)
      sql = "SELECT substr(date, 1, ?1) AS k, count(*) FROM files "
            "WHERE date >= ?2 AND date < ?2 || '~' GROUP BY k ORDER BY k";
   else
      sql = "SELECT substr(date, 1, ?1) AS k, count(*) FROM files "
            "WHERE date IS NOT NULL GROUP BY k ORDER BY k DESC";

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK)
      return;
   sqlite3_bind_int (st, 1, len);
   if (key)
      sqlite3_bind_text (st, 2, key, -1, SQLITE_TRANSIENT);

   while (sqlite3_step (st) == SQLITE_ROW) {
      const gchar *k = (const gchar *) sqlite3_column_text (st, 0);
      gint count = sqlite3_column_int (st, 1);
      GtkTreeIter iter, dummy;
      gchar *label;

      if (!k || (gint) strlen (k) < len) continue;
      /* the last part of the key: "2019", "05", "03" */
      label = g_strdup_printf ("%s (%d)", k + (len == 4 ? 0 : len - 2), count);
      gtk_tree_store_append (iv->store, &iter, parent);
      gtk_tree_store_set (iv->store, &iter,
                          COL_LABEL, label, COL_KIND, kind, COL_KEY, k, -1);
      g_free (label);

      if (kind != KIND_DAY) {
         gtk_tree_store_append (iv->store, &dummy, &iter);
         gtk_tree_store_set (iv->store, &dummy, COL_KIND, KIND_DUMMY, -1);
      }
   }
   sqlite3_finalize (st);
}


static GList *
files_for_key (const gchar *key)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   GList *list = NULL;

   if (!db) return NULL;
   if (sqlite3_prepare_v2 (db,
          "SELECT d.path, f.name FROM files f JOIN dirs d ON d.id = f.dir_id "
          "WHERE f.date >= ?1 AND f.date < ?1 || '~' ORDER BY f.date, f.name",
          -1, &st, NULL) != SQLITE_OK)
      return NULL;
   sqlite3_bind_text (st, 1, key, -1, SQLITE_TRANSIENT);
   while (sqlite3_step (st) == SQLITE_ROW) {
      const gchar *dir  = (const gchar *) sqlite3_column_text (st, 0);
      const gchar *name = (const gchar *) sqlite3_column_text (st, 1);
      list = g_list_prepend (list, g_build_filename (dir, name, NULL));
   }
   sqlite3_finalize (st);
   return g_list_reverse (list);
}


/******************************************************************************
 *  tree
 ******************************************************************************/

static gboolean
find_date_root (IndexView *iv, GtkTreeIter *iter)
{
   return gtk_tree_model_get_iter_first (GTK_TREE_MODEL (iv->store), iter);
}


static void
collect_expanded (GtkTreeView *tree, GtkTreePath *path, gpointer data)
{
   GPtrArray *keys = data;
   GtkTreeModel *model = gtk_tree_view_get_model (tree);
   GtkTreeIter iter;
   gchar *key = NULL;

   if (gtk_tree_model_get_iter (model, &iter, path))
      gtk_tree_model_get (model, &iter, COL_KEY, &key, -1);
   if (key) g_ptr_array_add (keys, key);
}


static gboolean
find_key (GtkTreeModel *model, GtkTreeIter *parent, const gchar *key,
          GtkTreeIter *found)
{
   GtkTreeIter iter;

   if (!gtk_tree_model_iter_children (model, &iter, parent))
      return FALSE;
   do {
      gchar *k = NULL;
      gtk_tree_model_get (model, &iter, COL_KEY, &k, -1);
      if (k && g_str_has_prefix (key, k)) {
         gboolean exact = !strcmp (k, key);
         g_free (k);
         if (exact) {
            *found = iter;
            return TRUE;
         }
         return find_key (model, &iter, key, found);
      }
      g_free (k);
   } while (gtk_tree_model_iter_next (model, &iter));
   return FALSE;
}


/* rebuild the years and open again what was open */
static void
refill (IndexView *iv)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter root, child;
   GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
   gboolean root_open;
   GtkTreePath *path;
   gchar *selected = NULL;
   GtkTreeSelection *sel;
   guint i;

   if (!find_date_root (iv, &root)) return;

   path = gtk_tree_model_get_path (model, &root);
   root_open = gtk_tree_view_row_expanded (GTK_TREE_VIEW (iv->tree), path);
   gtk_tree_path_free (path);
   gtk_tree_view_map_expanded_rows (GTK_TREE_VIEW (iv->tree), collect_expanded, keys);

   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   if (gtk_tree_selection_get_selected (sel, NULL, &child))
      gtk_tree_model_get (model, &child, COL_KEY, &selected, -1);

   iv->filling = TRUE;
   while (gtk_tree_model_iter_children (model, &child, &root))
      gtk_tree_store_remove (iv->store, &child);
   add_children (iv, &root, KIND_YEAR, NULL);

   if (root_open) {
      path = gtk_tree_model_get_path (model, &root);
      gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
      gtk_tree_path_free (path);
   }
   /* shorter keys first: a month row exists only after its year is open */
   for (i = 0; i < keys->len; i++) {
      if (find_key (model, &root, g_ptr_array_index (keys, i), &child)) {
         path = gtk_tree_model_get_path (model, &child);
         gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
         gtk_tree_path_free (path);
      }
   }
   if (selected && find_key (model, &root, selected, &child))
      gtk_tree_selection_select_iter (sel, &child);
   iv->filling = FALSE;

   g_free (selected);
   g_ptr_array_unref (keys);
}


static void
cb_index_changed (gpointer data)
{
   refill (data);
}


static void
cb_row_expanded (GtkTreeView *tree, GtkTreeIter *iter, GtkTreePath *path,
                 IndexView *iv)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter child;
   gint kind = 0, child_kind = KIND_DUMMY;
   gchar *key = NULL;

   if (!gtk_tree_model_iter_children (model, &child, iter)) return;
   gtk_tree_model_get (model, &child, COL_KIND, &child_kind, -1);
   if (child_kind != KIND_DUMMY) return;   /* already filled */

   gtk_tree_model_get (model, iter, COL_KIND, &kind, COL_KEY, &key, -1);
   if (kind == KIND_YEAR)
      add_children (iv, iter, KIND_MONTH, key);
   else if (kind == KIND_MONTH)
      add_children (iv, iter, KIND_DAY, key);
   gtk_tree_store_remove (iv->store, &child);
   g_free (key);
}


/******************************************************************************
 *  opening a result
 ******************************************************************************/

static void
cb_tv_gone (gpointer data, GObject *where)
{
   IndexView *iv = data;
   iv->tv = NULL;
}


static void
set_result_tab (IndexView *iv, GimvThumbView *tv)
{
   if (iv->tv) g_object_weak_unref (G_OBJECT (iv->tv), cb_tv_gone, iv);
   iv->tv = tv;
   if (tv) g_object_weak_ref (G_OBJECT (tv), cb_tv_gone, iv);
}


static void
open_key (IndexView *iv, const gchar *key)
{
   FilesLoader *files;
   GimvThumbView *tv;
   gchar *title;

   if (files_loader_query_loading ()) return;

   files = files_loader_new ();
   files->filelist = files_for_key (key);
   if (!files->filelist) {
      files_loader_delete (files);
      return;
   }
   files->status = END;
   files->thumb_load_type = LOAD_CACHE;

   /* the same tab for every date, while it is there */
   tv = iv->tv;
   if (!tv || tv->tw != iv->tw || tv->progress) {
      tv = gimv_thumb_win_create_new_tab (iv->tw);
      set_result_tab (iv, tv);
   } else {
      gint page = gtk_notebook_page_num (GTK_NOTEBOOK (iv->tw->notebook),
                                         tv->container);
      gtk_notebook_set_current_page (GTK_NOTEBOOK (iv->tw->notebook), page);
   }

   title = g_strdup_printf (_("%s (Date)"), key);
   gimv_thumb_view_reset_tab_label (tv, title);
   g_free (title);

   open_image_files_in_thumbnail_view (files, iv->tw, tv);
   files_loader_delete (files);
}


static gboolean
idle_open_selected (gpointer data)
{
   IndexView *iv = data;
   GtkTreeSelection *sel;
   GtkTreeIter iter;
   gint kind = 0;
   gchar *key = NULL;

   iv->open_id = 0;
   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   if (!gtk_tree_selection_get_selected (sel, NULL, &iter))
      return G_SOURCE_REMOVE;
   gtk_tree_model_get (GTK_TREE_MODEL (iv->store), &iter,
                       COL_KIND, &kind, COL_KEY, &key, -1);
   if (key && (kind == KIND_YEAR || kind == KIND_MONTH || kind == KIND_DAY))
      open_key (iv, key);
   g_free (key);
   return G_SOURCE_REMOVE;
}


static void
cb_selection_changed (GtkTreeSelection *sel, IndexView *iv)
{
   if (iv->filling || iv->open_id) return;
   /* after the click has been handled */
   iv->open_id = g_idle_add (idle_open_selected, iv);
}


/******************************************************************************
 *  widget
 ******************************************************************************/

static void
cb_destroy (GtkWidget *widget, IndexView *iv)
{
   if (iv->watch_id) gimv_index_remove_watch (iv->watch_id);
   if (iv->open_id) g_source_remove (iv->open_id);
   set_result_tab (iv, NULL);
   g_clear_object (&iv->store);
   g_free (iv);
}


GtkWidget *
gimv_index_view_new (GimvThumbWin *tw)
{
   IndexView *iv = g_new0 (IndexView, 1);
   GtkTreeViewColumn *col;
   GtkCellRenderer *render;
   GtkTreeIter root;

   iv->tw = tw;
   iv->store = gtk_tree_store_new (N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);

   gtk_tree_store_append (iv->store, &root, NULL);
   gtk_tree_store_set (iv->store, &root,
                       COL_LABEL, _("Date"), COL_KIND, KIND_DATE_ROOT, -1);
   add_children (iv, &root, KIND_YEAR, NULL);

   iv->tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL (iv->store));
   gtk_widget_set_name (iv->tree, "IndexTree");
   gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (iv->tree), FALSE);
   col = gtk_tree_view_column_new ();
   render = gtk_cell_renderer_text_new ();
   gtk_tree_view_column_pack_start (col, render, TRUE);
   gtk_tree_view_column_add_attribute (col, render, "text", COL_LABEL);
   gtk_tree_view_append_column (GTK_TREE_VIEW (iv->tree), col);

   g_signal_connect (iv->tree, "row-expanded", G_CALLBACK (cb_row_expanded), iv);
   g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree)),
                     "changed", G_CALLBACK (cb_selection_changed), iv);

   iv->scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (iv->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (iv->scroll), TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (iv->scroll), iv->tree);
   gtk_widget_set_name (iv->scroll, "IndexView");

   g_signal_connect (iv->scroll, "destroy", G_CALLBACK (cb_destroy), iv);
   iv->watch_id = gimv_index_add_watch (cb_index_changed, iv);

   return iv->scroll;
}
