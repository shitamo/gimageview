/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the window of the bookmarks whose file or folder is not
 * found (moved, renamed or deleted while gimv was not looking: gimv not
 * running, or another program outside the indexed folders; or a drive
 * not mounted).
 *
 * As the window of the missing images of the index (gimv_index_missing.c):
 * a list of them, with where each may have gone, and buttons to put the
 * bookmark (and its note) on it or on another file, or to remove it.
 * Nothing is removed by itself: a drive not mounted comes back.
 *
 * Where it may have gone: the files (folders) of the same name in the
 * index that are there.  The index is read in a thread, with a connection
 * of its own.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>

#include "gimv_bookmark.h"
#include "gimv_bookmark_missing.h"
#include "gimv_bookmark_cand.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_list_view.h"

enum {
   COL_PATH,
   COL_ADDED,
   COL_NOTE,
   COL_CAND,        /* shown: the candidate, or how many */
   COL_CAND_PATH,   /* the one candidate ("" none or more than one) */
   COL_CANDS,       /* all of them, one a line (the tooltip) */
   N_COLS
};

typedef struct {
   GtkWidget    *window;
   GtkWidget    *tree;
   GListStore   *store;       /* GimvRow */
   GtkWidget    *info;
   GtkWidget    *btn_cand, *btn_other, *btn_remove;
   GCancellable *cancel;      /* the search of the candidates */
   guint         watch;
   gboolean      own_change;  /* a change made here: the list is kept */
} MissWin;

static MissWin *the_win = NULL;

static void fill (MissWin *mw);
static void update_buttons (MissWin *mw);


static void
set_info (MissWin *mw, const gchar *text)
{
   gtk_label_set_text (GTK_LABEL (mw->info), text ? text : "");
}


/******************************************************************************
 *  the candidates
 ******************************************************************************/

typedef struct {
   gchar     *dbfile;
   GPtrArray *paths;      /* in */
   GPtrArray *results;    /* out: a GPtrArray of candidates a path */
} SearchData;


static void
search_data_free (SearchData *sd)
{
   g_free (sd->dbfile);
   g_ptr_array_unref (sd->paths);
   if (sd->results) g_ptr_array_unref (sd->results);
   g_free (sd);
}


static void
search_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   SearchData *sd = data;
   sqlite3 *db = NULL;
   guint i;

   sd->results = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);
   if (sd->dbfile && g_file_test (sd->dbfile, G_FILE_TEST_IS_REGULAR)
       && sqlite3_open_v2 (sd->dbfile, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
   {
      sqlite3_busy_timeout (db, 2000);
   } else if (db) {
      sqlite3_close (db);
      db = NULL;
   }
   for (i = 0; i < sd->paths->len; i++) {
      if (g_cancellable_is_cancelled (cancel)) break;
      g_ptr_array_add (sd->results,
                       gimv_bookmark_missing_candidates (db, g_ptr_array_index (sd->paths, i)));
   }
   if (db) sqlite3_close (db);
   g_task_return_boolean (task, TRUE);
}


static void
search_done (GObject *source, GAsyncResult *res, gpointer data)
{
   SearchData *sd = g_task_get_task_data (G_TASK (res));
   MissWin *mw = data;
   GListModel *model;
   guint i, n;

   if (g_task_had_error (G_TASK (res))
       || g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (res))))
      return;
   /* the window may be gone (then the cancellable was cancelled) */
   if (mw != the_win) return;

   model = G_LIST_MODEL (mw->store);
   n = g_list_model_get_n_items (model);
   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (model, i);
      const gchar *path = gimv_row_peek_string (row, COL_PATH);
      GPtrArray *cands = NULL;
      guint j;

      for (j = 0; j < sd->paths->len && j < sd->results->len; j++)
         if (!strcmp (g_ptr_array_index (sd->paths, j), path)) {
            cands = g_ptr_array_index (sd->results, j);
            break;
         }
      if (!cands) continue;

      if (cands->len == 1) {
         gimv_row_set (row, COL_CAND, g_ptr_array_index (cands, 0),
                            COL_CAND_PATH, g_ptr_array_index (cands, 0),
                            COL_CANDS, g_ptr_array_index (cands, 0), -1);
      } else if (cands->len > 1) {
         gchar *text = g_strdup_printf (ngettext ("%'u candidate", "%'u candidates",
                                                  cands->len), cands->len);
         gchar *all;

         g_ptr_array_add (cands, NULL);
         all = g_strjoinv ("\n", (gchar **) cands->pdata);
         g_ptr_array_remove_index (cands, cands->len - 1);
         gimv_row_set (row, COL_CAND, text, COL_CAND_PATH, "", COL_CANDS, all, -1);
         g_free (all);
         g_free (text);
      } else {
         gimv_row_set (row, COL_CAND, _("None"), COL_CAND_PATH, "", COL_CANDS, "", -1);
      }
      gimv_row_changed (row);
   }
   update_buttons (mw);
   set_info (mw, NULL);
}


static void
search_candidates (MissWin *mw)
{
   GListModel *model = G_LIST_MODEL (mw->store);
   SearchData *sd;
   GTask *task;
   guint i, n = g_list_model_get_n_items (model);

   if (mw->cancel) {
      g_cancellable_cancel (mw->cancel);
      g_clear_object (&mw->cancel);
   }
   if (!n) return;

   sd = g_new0 (SearchData, 1);
   sd->dbfile = gimv_index_is_configured () ? gimv_index_db_default_path () : NULL;
   sd->paths = g_ptr_array_new_with_free_func (g_free);
   for (i = 0; i < n; i++)
      g_ptr_array_add (sd->paths,
                       g_strdup (gimv_row_peek_string (gimv_row_store_peek (model, i), COL_PATH)));

   mw->cancel = g_cancellable_new ();
   task = g_task_new (NULL, mw->cancel, search_done, mw);
   g_task_set_task_data (task, sd, (GDestroyNotify) search_data_free);
   g_task_run_in_thread (task, search_thread);
   g_object_unref (task);
   set_info (mw, _("Looking for where they went in the index..."));
}


/******************************************************************************
 *  actions
 ******************************************************************************/

/* the rows selected (referenced), in order */
static GPtrArray *
selected_rows (MissWin *mw)
{
   GtkSelectionModel *sel = gimv_list_view_get_selection (mw->tree);
   GtkBitset *set = gtk_selection_model_get_selection (sel);
   GtkBitsetIter it;
   GPtrArray *rows = g_ptr_array_new_with_free_func (g_object_unref);
   guint pos;

   if (gtk_bitset_iter_init_first (&it, set, &pos)) {
      do {
         GimvRow *row = gimv_row_store_peek (G_LIST_MODEL (mw->store), pos);
         if (row) g_ptr_array_add (rows, g_object_ref (row));
      } while (gtk_bitset_iter_next (&it, &pos));
   }
   gtk_bitset_unref (set);
   return rows;
}


static void
remove_row (MissWin *mw, GimvRow *row)
{
   guint pos;
   if (gimv_row_store_find (G_LIST_MODEL (mw->store), row, &pos))
      g_list_store_remove (mw->store, pos);
}


static void
done_message (MissWin *mw, guint done, guint kept)
{
   update_buttons (mw);
   if (!g_list_model_get_n_items (G_LIST_MODEL (mw->store))) {
      set_info (mw, _("Every bookmarked file and folder is there."));
      return;
   }
   gchar *text = g_strdup_printf (ngettext ("%'u bookmark done.", "%'u bookmarks done.", done),
                                  done);
   if (kept) {
      gchar *more = g_strdup_printf (ngettext ("%'u kept its own bookmark (the note of the missing one is not taken).",
                                               "%'u kept their own bookmarks (the notes of the missing ones are not taken).",
                                               kept), kept);
      gchar *all = g_strconcat (text, " ", more, NULL);
      set_info (mw, all);
      g_free (all);
      g_free (more);
   } else {
      set_info (mw, text);
   }
   g_free (text);
}


/* the bookmark of from (and its note) goes to to; TRUE when to had one of
 * its own already (it is kept, from is let go of) */
static gboolean
put_on (MissWin *mw, const gchar *from, const gchar *to)
{
   gboolean had = gimv_bookmark_has (to);

   mw->own_change = TRUE;
   if (had)
      gimv_bookmark_remove (from);
   else
      gimv_bookmark_move (from, to);
   mw->own_change = FALSE;
   return had;
}


static void
cb_put_on_candidate (GtkButton *b, MissWin *mw)
{
   GPtrArray *rows = selected_rows (mw);
   guint done = 0, kept = 0, i;

   for (i = 0; i < rows->len; i++) {
      GimvRow *row = g_ptr_array_index (rows, i);
      const gchar *to = gimv_row_peek_string (row, COL_CAND_PATH);

      if (!to || !*to) continue;
      if (put_on (mw, gimv_row_peek_string (row, COL_PATH), to)) kept++;
      remove_row (mw, row);
      done++;
   }
   g_ptr_array_unref (rows);
   done_message (mw, done, kept);
}


static void
cb_remove (GtkButton *b, MissWin *mw)
{
   GPtrArray *rows = selected_rows (mw);
   guint i;

   mw->own_change = TRUE;
   for (i = 0; i < rows->len; i++) {
      GimvRow *row = g_ptr_array_index (rows, i);
      gimv_bookmark_remove (gimv_row_peek_string (row, COL_PATH));
      remove_row (mw, row);
   }
   mw->own_change = FALSE;
   done_message (mw, rows->len, 0);
   g_ptr_array_unref (rows);
}


static void
other_chosen (MissWin *mw, GFile *file)
{
   GPtrArray *rows;
   gchar *to;

   if (!file || mw != the_win) return;
   to = g_file_get_path (file);
   rows = selected_rows (mw);
   if (to && rows->len == 1) {
      GimvRow *row = g_ptr_array_index (rows, 0);
      gboolean kept = put_on (mw, gimv_row_peek_string (row, COL_PATH), to);
      remove_row (mw, row);
      done_message (mw, 1, kept ? 1 : 0);
   }
   g_ptr_array_unref (rows);
   g_free (to);
}


static void
cb_file_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
   GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
   other_chosen (data, file);
   if (file) g_object_unref (file);
}


static void
cb_folder_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
   GFile *file = gtk_file_dialog_select_folder_finish (GTK_FILE_DIALOG (source), res, NULL);
   other_chosen (data, file);
   if (file) g_object_unref (file);
}


/* a name without an extension is taken for a folder */
static gboolean
looks_like_folder (const gchar *path)
{
   g_autofree gchar *name = g_path_get_basename (path);
   const gchar *dot = strrchr (name, '.');
   return !dot || dot == name || !dot[1];
}


static void
cb_other (GtkButton *b, MissWin *mw)
{
   GtkFileDialog *dialog;
   GPtrArray *rows = selected_rows (mw);
   const gchar *path, *cands;
   gchar *start = NULL;
   gboolean folder;

   if (rows->len != 1) {
      g_ptr_array_unref (rows);
      return;
   }
   path = gimv_row_peek_string (g_ptr_array_index (rows, 0), COL_PATH);
   cands = gimv_row_peek_string (g_ptr_array_index (rows, 0), COL_CANDS);
   folder = looks_like_folder (path);

   /* start at the first candidate, or the folder it was in (or above) */
   if (cands && *cands) {
      gchar *first = g_strndup (cands, strcspn (cands, "\n"));
      start = g_path_get_dirname (first);
      g_free (first);
   } else {
      start = g_path_get_dirname (path);
   }
   while (start && !g_file_test (start, G_FILE_TEST_IS_DIR) && strcmp (start, "/")) {
      gchar *up = g_path_get_dirname (start);
      g_free (start);
      start = up;
   }

   dialog = gtk_file_dialog_new ();
   if (start && g_file_test (start, G_FILE_TEST_IS_DIR)) {
      GFile *f = g_file_new_for_path (start);
      gtk_file_dialog_set_initial_folder (dialog, f);
      g_object_unref (f);
   }
   g_free (start);
   g_ptr_array_unref (rows);

   gtk_file_dialog_set_title (dialog, _("Where the Bookmark Goes"));
   if (folder)
      gtk_file_dialog_select_folder (dialog, GTK_WINDOW (mw->window), NULL,
                                     cb_folder_chosen, mw);
   else
      gtk_file_dialog_open (dialog, GTK_WINDOW (mw->window), NULL, cb_file_chosen, mw);
   g_object_unref (dialog);
}


static void
update_buttons (MissWin *mw)
{
   GPtrArray *rows = selected_rows (mw);
   gboolean any_cand = FALSE;
   guint n = rows->len, i;

   for (i = 0; i < n && !any_cand; i++) {
      const gchar *c = gimv_row_peek_string (g_ptr_array_index (rows, i), COL_CAND_PATH);
      any_cand = c && *c;
   }
   g_ptr_array_unref (rows);

   gtk_widget_set_sensitive (mw->btn_cand, any_cand);
   gtk_widget_set_sensitive (mw->btn_other, n == 1);
   gtk_widget_set_sensitive (mw->btn_remove, n > 0);
}


static void
cb_selection_changed (GtkSelectionModel *sel, guint pos, guint n, MissWin *mw)
{
   update_buttons (mw);
}


/******************************************************************************
 *  window
 ******************************************************************************/

/* the bookmarks not found now */
static void
fill (MissWin *mw)
{
   GPtrArray *bms = gimv_bookmark_list ();
   guint i, n = 0;

   g_list_store_remove_all (mw->store);
   for (i = 0; i < bms->len; i++) {
      GimvBookmark *bm = g_ptr_array_index (bms, i);
      GDateTime *dt;
      gchar *added;
      GimvRow *row;

      if (g_file_test (bm->path, G_FILE_TEST_EXISTS)) continue;
      dt = g_date_time_new_from_unix_local (bm->added);
      added = dt ? g_date_time_format (dt, "%Y-%m-%d %H:%M") : g_strdup ("");
      if (dt) g_date_time_unref (dt);

      row = gimv_row_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
      g_list_store_append (mw->store, row);
      g_object_unref (row);
      gimv_row_set (row,
                    COL_PATH, bm->path,
                    COL_ADDED, added,
                    COL_NOTE, bm->note ? bm->note : "",
                    COL_CAND, "",
                    COL_CAND_PATH, "",
                    COL_CANDS, "",
                    -1);
      g_free (added);
      n++;
   }
   g_ptr_array_unref (bms);
   update_buttons (mw);

   if (!n) {
      set_info (mw, _("Every bookmarked file and folder is there."));
      return;
   }
   if (gimv_index_is_configured ())
      search_candidates (mw);
   else
      set_info (mw, _("Without the index, where they went is not looked for."));
}


/* changed elsewhere (another gimv, the indexer following a move) */
static void
cb_bookmarks_changed (gpointer data)
{
   MissWin *mw = data;
   if (!mw->own_change) fill (mw);
}


static void
cb_destroy (GtkWidget *w, MissWin *mw)
{
   if (the_win == mw) the_win = NULL;
   if (mw->cancel) {
      g_cancellable_cancel (mw->cancel);
      g_object_unref (mw->cancel);
   }
   if (mw->watch) gimv_bookmark_remove_watch (mw->watch);
   g_object_unref (mw->store);
   g_free (mw);
}


static GtkWidget *
cell_setup (gpointer data)
{
   GtkWidget *label = gtk_label_new (NULL);
   gint col = GPOINTER_TO_INT (data);

   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   if (col == COL_PATH || col == COL_NOTE || col == COL_CAND)
      gtk_label_set_ellipsize (GTK_LABEL (label),
                               col == COL_NOTE ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_MIDDLE);
   g_object_set_data (G_OBJECT (label), "gimv-bm-missing-col", data);
   return label;
}


static void
cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   gint col = GPOINTER_TO_INT (data);
   const gchar *text = gimv_row_peek_string (row, col);
   const gchar *tip = text;

   gtk_label_set_text (GTK_LABEL (gimv_cell_get_content (cell)), text ? text : "");
   if (col == COL_CAND) tip = gimv_row_peek_string (row, COL_CANDS);
   if (col == COL_ADDED || (tip && !*tip)) tip = NULL;
   gtk_widget_set_tooltip_text (cell, tip);
}


static void
add_column (MissWin *mw, const gchar *title, gint col, gint width)
{
   GtkColumnViewColumn *c
      = gimv_column_view_append (GTK_COLUMN_VIEW (mw->tree), title,
                                 gimv_cell_factory_new (cell_setup, cell_bind,
                                                        GINT_TO_POINTER (col), NULL, FALSE));
   gtk_column_view_column_set_resizable (c, TRUE);
   if (width > 0)
      gtk_column_view_column_set_fixed_width (c, width);
}


static GtkWidget *
button_new (const gchar *label, GCallback cb, MissWin *mw)
{
   GtkWidget *b = gtk_button_new_with_label (label);
   if (cb) g_signal_connect (b, "clicked", cb, mw);
   return b;
}


void
gimv_bookmark_missing_open (GtkWindow *parent)
{
   MissWin *mw;
   GtkWidget *vbox, *hbox, *scroll, *label, *b;

   if (the_win) {
      fill (the_win);
      gtk_window_present (GTK_WINDOW (the_win->window));
      return;
   }

   mw = the_win = g_new0 (MissWin, 1);
   mw->store = gimv_row_store_new ();

   mw->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (mw->window), _("Bookmarks Not Found"));
   gtk_window_set_default_size (GTK_WINDOW (mw->window), 900, 420);
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (mw->window), parent);
   g_signal_connect (mw->window, "destroy", G_CALLBACK (cb_destroy), mw);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 8);
   gtk_widget_set_margin_end (vbox, 8);
   gtk_widget_set_margin_top (vbox, 8);
   gtk_widget_set_margin_bottom (vbox, 8);
   gtk_window_set_child (GTK_WINDOW (mw->window), vbox);

   label = gtk_label_new (_("Bookmarked files and folders that are not found (moved, renamed "
                            "or deleted while gimv was not looking, or on a drive not "
                            "mounted). Put the bookmark on where it went, or remove it."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   mw->tree = gimv_column_view_new (G_LIST_MODEL (g_object_ref (mw->store)),
                                    GTK_SELECTION_MULTIPLE);
   add_column (mw, _("Path"), COL_PATH, 300);
   add_column (mw, _("Added"), COL_ADDED, 0);
   add_column (mw, _("Note of the Bookmark"), COL_NOTE, 140);
   add_column (mw, _("Where It May Have Gone"), COL_CAND, 260);
   gimv_list_view_enable_search (mw->tree, COL_PATH, NULL, NULL);
   g_signal_connect (gimv_list_view_get_selection (mw->tree), "selection-changed",
                     G_CALLBACK (cb_selection_changed), mw);

   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), mw->tree);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_box_append (GTK_BOX (vbox), scroll);

   mw->info = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (mw->info), 0.0);
   gtk_label_set_wrap (GTK_LABEL (mw->info), TRUE);
   gtk_box_append (GTK_BOX (vbox), mw->info);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   mw->btn_cand   = button_new (_("Put on Where It Went"), G_CALLBACK (cb_put_on_candidate), mw);
   mw->btn_other  = button_new (_("Put on Another..."), G_CALLBACK (cb_other), mw);
   mw->btn_remove = button_new (_("Remove the Bookmark"), G_CALLBACK (cb_remove), mw);
   gtk_box_append (GTK_BOX (hbox), mw->btn_cand);
   gtk_box_append (GTK_BOX (hbox), mw->btn_other);
   gtk_box_append (GTK_BOX (hbox), mw->btn_remove);
   b = button_new (_("Close"), NULL, mw);
   g_signal_connect_swapped (b, "clicked", G_CALLBACK (gtk_window_destroy), mw->window);
   gtk_widget_set_hexpand (b, TRUE);
   gtk_widget_set_halign (b, GTK_ALIGN_END);
   gtk_box_append (GTK_BOX (hbox), b);
   gtk_box_append (GTK_BOX (vbox), hbox);

   mw->watch = gimv_bookmark_add_watch (cb_bookmarks_changed, mw);
   fill (mw);
   gtk_window_present (GTK_WINDOW (mw->window));
}
