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
#include "gimv_icon_stock.h"
#include "gimv_index.h"
#include "menu.h"
#include "prefs.h"
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
   GtkWidget     *popover;
   gchar         *menu_key;    /* row of the right button menu */
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
show_page (GimvThumbWin *tw, GtkWidget *container)
{
   gint page = gtk_notebook_page_num (GTK_NOTEBOOK (tw->notebook), container);
   if (page >= 0)
      gtk_notebook_set_current_page (GTK_NOTEBOOK (tw->notebook), page);
}


/* new_tab FALSE: replace what the result tab shows (the usual way) */
static void
open_key (IndexView *iv, const gchar *key, gboolean new_tab)
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

   tv = new_tab ? NULL : iv->tv;
   if (!tv || tv->tw != iv->tw || tv->progress) {
      tv = gimv_thumb_win_create_new_tab (iv->tw);
      /* a tab opened on request stays as it is: the next date goes to a
       * tab of its own only when asked again */
      if (!new_tab || !iv->tv)
         set_result_tab (iv, tv);
   }
   show_page (iv->tw, tv->container);

   title = g_strdup_printf (_("%s (Date)"), key);
   gimv_thumb_view_reset_tab_label (tv, title);
   g_free (title);

   open_image_files_in_thumbnail_view (files, iv->tw, tv);
   files_loader_delete (files);
}


static gboolean
row_key (IndexView *iv, GtkTreeIter *iter, gchar **key)
{
   gint kind = 0;

   *key = NULL;
   gtk_tree_model_get (GTK_TREE_MODEL (iv->store), iter,
                       COL_KIND, &kind, COL_KEY, key, -1);
   if (*key && (kind == KIND_YEAR || kind == KIND_MONTH || kind == KIND_DAY))
      return TRUE;
   g_free (*key);
   *key = NULL;
   return FALSE;
}


static gboolean
idle_open_selected (gpointer data)
{
   IndexView *iv = data;
   GtkTreeSelection *sel;
   GtkTreeIter iter;
   gchar *key;

   iv->open_id = 0;
   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   if (gtk_tree_selection_get_selected (sel, NULL, &iter)
       && row_key (iv, &iter, &key))
   {
      open_key (iv, key, FALSE);
      g_free (key);
   }
   return G_SOURCE_REMOVE;
}


static void
cb_selection_changed (GtkTreeSelection *sel, IndexView *iv)
{
   if (iv->filling || iv->open_id) return;
   /* after the click has been handled */
   iv->open_id = g_idle_add (idle_open_selected, iv);
}


/* right button: a menu for the row under the pointer, middle button: open
 * it in a new tab.  Neither changes the selection. */
static gchar *
key_at (IndexView *iv, gdouble x, gdouble y)
{
   GtkTreePath *path = NULL;
   GtkTreeIter iter;
   gint bx, by;
   gchar *key = NULL;

   gtk_tree_view_convert_widget_to_bin_window_coords (GTK_TREE_VIEW (iv->tree),
                                                      x, y, &bx, &by);
   if (!gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (iv->tree), bx, by,
                                       &path, NULL, NULL, NULL))
      return NULL;
   if (gtk_tree_model_get_iter (GTK_TREE_MODEL (iv->store), &iter, path))
      row_key (iv, &iter, &key);
   gtk_tree_path_free (path);
   return key;
}


static void
cb_action_open (GSimpleAction *action, GVariant *param, gpointer data)
{
   IndexView *iv = data;
   if (iv->menu_key)
      open_key (iv, iv->menu_key, !strcmp (g_action_get_name (G_ACTION (action)),
                                           "open-new-tab"));
}


static void
cb_tree_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                 IndexView *iv)
{
   guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
   gchar *key;

   /* the left button is left to the tree itself */
   if (button != GDK_BUTTON_MIDDLE && button != GDK_BUTTON_SECONDARY) return;
   key = key_at (iv, x, y);
   if (!key) return;
   gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

   g_free (iv->menu_key);
   iv->menu_key = key;

   if (button == GDK_BUTTON_MIDDLE) {
      open_key (iv, key, TRUE);
   } else if (button == GDK_BUTTON_SECONDARY) {
      GdkRectangle rect = { (gint) x, (gint) y, 1, 1 };
      if (!iv->popover) {
         GMenu *menu = g_menu_new ();
         g_menu_append (menu, _("Open"), "index.open");
         g_menu_append (menu, _("Open in New Tab"), "index.open-new-tab");
         iv->popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
         gtk_widget_set_parent (iv->popover, iv->tree);
         gtk_popover_set_has_arrow (GTK_POPOVER (iv->popover), FALSE);
         g_object_unref (menu);
      }
      gtk_popover_set_pointing_to (GTK_POPOVER (iv->popover), &rect);
      gtk_popover_popup (GTK_POPOVER (iv->popover));
   }
}


/******************************************************************************
 *  widget
 ******************************************************************************/

static void
cb_destroy (GtkWidget *widget, IndexView *iv)
{
   if (iv->watch_id) gimv_index_remove_watch (iv->watch_id);
   if (iv->open_id) g_source_remove (iv->open_id);
   if (iv->popover) gtk_widget_unparent (iv->popover);
   set_result_tab (iv, NULL);
   g_clear_object (&iv->store);
   g_free (iv->menu_key);
   g_free (iv);
}


static GtkWidget *
index_view_new (GimvThumbWin *tw)
{
   static const GActionEntry actions[] = {
      { "open",         cb_action_open, NULL, NULL, NULL },
      { "open-new-tab", cb_action_open, NULL, NULL, NULL },
   };
   IndexView *iv = g_new0 (IndexView, 1);
   GtkTreeViewColumn *col;
   GtkCellRenderer *render;
   GtkTreeIter root;
   GtkGesture *click;
   GSimpleActionGroup *group;

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
   /* the years are shown from the start */
   {
      GtkTreePath *path = gtk_tree_path_new_first ();
      gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
      gtk_tree_path_free (path);
   }

   g_signal_connect (iv->tree, "row-expanded", G_CALLBACK (cb_row_expanded), iv);
   g_signal_connect (gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree)),
                     "changed", G_CALLBACK (cb_selection_changed), iv);

   click = gtk_gesture_click_new ();
   gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
   gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                               GTK_PHASE_CAPTURE);
   g_signal_connect (click, "pressed", G_CALLBACK (cb_tree_pressed), iv);
   gtk_widget_add_controller (iv->tree, GTK_EVENT_CONTROLLER (click));

   group = g_simple_action_group_new ();
   g_action_map_add_action_entries (G_ACTION_MAP (group), actions,
                                    G_N_ELEMENTS (actions), iv);
   gtk_widget_insert_action_group (iv->tree, "index", G_ACTION_GROUP (group));
   g_object_unref (group);

   iv->scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (iv->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (iv->scroll), iv->tree);
   gtk_widget_set_name (iv->scroll, "IndexView");

   g_signal_connect (iv->scroll, "destroy", G_CALLBACK (cb_destroy), iv);
   iv->watch_id = gimv_index_add_watch (cb_index_changed, iv);

   return iv->scroll;
}


/******************************************************************************
 *  the "Date" tab of the thumbnail window
 ******************************************************************************/

#define DATE_TAB_KEY "gimv-index-date-tab"

GtkWidget *
gimv_index_tab_get_page (GimvThumbWin *tw)
{
   return g_object_get_data (G_OBJECT (tw), DATE_TAB_KEY);
}


static void
sync_menu (GimvThumbWin *tw)
{
   if (tw->menuitem.index_date_tab)
      gimv_menu_item_set_active (GIMV_MENU_ITEM (tw->menuitem.index_date_tab),
                                 gimv_index_tab_get_page (tw) != NULL);
}


static void
cb_page_destroy (GtkWidget *page, GimvThumbWin *tw)
{
   if (gimv_index_tab_get_page (tw) == page)
      g_object_set_data (G_OBJECT (tw), DATE_TAB_KEY, NULL);
}


static void
cb_close_clicked (GtkWidget *button, GimvThumbWin *tw)
{
   gimv_index_tab_close (tw);
}


void
gimv_index_tab_open (GimvThumbWin *tw)
{
   GtkWidget *page, *hbox, *label, *button;

   g_return_if_fail (GIMV_IS_THUMB_WIN (tw));
   if (gimv_index_tab_get_page (tw) || !tw->notebook) return;

   page = index_view_new (tw);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   g_object_set_data (G_OBJECT (hbox), "page-container", page);
   label = gtk_label_new (_("Date"));
   gtk_widget_set_name (label, "TabLabel");
   gtk_widget_add_css_class (label, "gimv-tab-label");
   gtk_widget_set_margin_start (label, 4);
   gtk_widget_set_margin_end (label, 4);
   gtk_box_append (GTK_BOX (hbox), label);

   button = gtk_button_new ();
   gtk_button_set_has_frame (GTK_BUTTON (button), FALSE);
   gtk_button_set_child (GTK_BUTTON (button), gimv_icon_stock_get_widget ("small_close"));
   gimv_widget_set_size (button, 16, 16);
   g_signal_connect (button, "clicked", G_CALLBACK (cb_close_clicked), tw);
   gtk_box_append (GTK_BOX (hbox), button);
   gtk_widget_set_visible (button, conf.thumbwin_show_tab_close);

   gtk_notebook_append_page (GTK_NOTEBOOK (tw->notebook), page, hbox);
   gtk_notebook_set_menu_label_text (GTK_NOTEBOOK (tw->notebook), page, _("Date"));
   gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (tw->notebook), page, TRUE);
   tw->pagenum++;

   g_object_set_data (G_OBJECT (tw), DATE_TAB_KEY, page);
   g_signal_connect (page, "destroy", G_CALLBACK (cb_page_destroy), tw);
   sync_menu (tw);
   gimv_thumb_win_set_statusbar_page_info (tw, GIMV_THUMB_WIN_CURRENT_PAGE);
}


void
gimv_index_tab_close (GimvThumbWin *tw)
{
   GtkWidget *page;
   gint num;

   g_return_if_fail (GIMV_IS_THUMB_WIN (tw));
   page = gimv_index_tab_get_page (tw);
   if (!page) return;

   g_object_set_data (G_OBJECT (tw), DATE_TAB_KEY, NULL);
   num = gtk_notebook_page_num (GTK_NOTEBOOK (tw->notebook), page);
   if (num >= 0) {
      gtk_notebook_remove_page (GTK_NOTEBOOK (tw->notebook), num);
      if (tw->pagenum > 0) tw->pagenum--;
   }
   sync_menu (tw);
   gimv_thumb_win_set_statusbar_page_info (tw, GIMV_THUMB_WIN_CURRENT_PAGE);
}
