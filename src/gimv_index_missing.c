/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the window of the missing images.
 *
 * The indexer writes the images with comment files (comments, tags) that
 * are gone to logs in ~/.local/share/gimageview/missing-*.log (path, last
 * seen, tags, and for a move nobody answered, where the image went).  This
 * window lists a log and lets the user put each comment file on the image
 * where it went or on another image, delete it, or just take the line out
 * of the log.  A handled line leaves the log; an empty log is deleted.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>

#include "gimv_index.h"
#include "gimv_list_view.h"
#include "gimv_index_missing.h"

enum {
   COL_PATH,
   COL_LAST,
   COL_TAGS,
   COL_MOVED,
   COL_STATE,
   N_COLS
};

#define MOVED_PREFIX "moved to "

typedef struct {
   GtkWidget    *window;
   GtkWidget    *logs;       /* GtkDropDown */
   GtkStringList *log_names;
   GtkWidget    *tree;
   GListStore   *store;       /* GimvRow */
   GtkWidget    *info;
   GtkWidget    *btn_moved, *btn_other, *btn_delete, *btn_drop;
   gchar        *log_path;   /* shown */
   GPtrArray    *header;     /* the "#" lines of the log */
} MissingWin;

static MissingWin *the_win = NULL;


static gchar *
log_dir (void)
{
   return g_build_filename (g_get_user_data_dir (), "gimageview", NULL);
}


static gchar *
comment_path (const gchar *img)
{
   /* the same place as gimv_comment_get_path () */
   return g_strconcat (g_getenv ("HOME"), "/.gimv/comment", img, ".txt", NULL);
}


static void
set_info (MissingWin *mw, const gchar *text)
{
   gtk_label_set_text (GTK_LABEL (mw->info), text ? text : "");
}


/******************************************************************************
 *  the logs
 ******************************************************************************/

static gint
compare_desc (gconstpointer a, gconstpointer b)
{
   return strcmp (*(const gchar **) b, *(const gchar **) a);
}


/* missing-*.log names, the newest first */
static GPtrArray *
list_logs (void)
{
   gchar *dir = log_dir ();
   GDir *d = g_dir_open (dir, 0, NULL);
   GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
   const gchar *name;

   if (d) {
      while ((name = g_dir_read_name (d)))
         if (g_str_has_prefix (name, "missing-") && g_str_has_suffix (name, ".log"))
            g_ptr_array_add (names, g_strdup (name));
      g_dir_close (d);
   }
   g_free (dir);
   g_ptr_array_sort (names, compare_desc);
   return names;
}


static const gchar *
state_of (const gchar *path)
{
   gchar *c = comment_path (path);
   gboolean has_comment = g_file_test (c, G_FILE_TEST_IS_REGULAR);
   g_free (c);

   if (!has_comment)
      return _("No comment file (done)");
   if (g_file_test (path, G_FILE_TEST_EXISTS))
      return _("The image is back");
   return _("Comment file left");
}


static void
load_log (MissingWin *mw, const gchar *path)
{
   gchar *text = NULL, **lines;
   guint i;

   g_list_store_remove_all (mw->store);
   g_ptr_array_set_size (mw->header, 0);
   g_free (mw->log_path);
   mw->log_path = path ? g_strdup (path) : NULL;
   set_info (mw, NULL);
   if (!path || !g_file_get_contents (path, &text, NULL, NULL)) return;

   lines = g_strsplit (text, "\n", -1);
   for (i = 0; lines[i]; i++) {
      gchar **f;
      GimvRow *row;
      const gchar *moved = NULL;

      if (!*lines[i]) continue;
      if (lines[i][0] == '#') {
         g_ptr_array_add (mw->header, g_strdup (lines[i]));
         continue;
      }
      f = g_strsplit (lines[i], "\t", 4);
      if (!f[0] || !*f[0]) {
         g_strfreev (f);
         continue;
      }
      if (f[1] && f[2] && f[3] && g_str_has_prefix (f[3], MOVED_PREFIX))
         moved = f[3] + strlen (MOVED_PREFIX);
      row = gimv_row_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                          G_TYPE_STRING, G_TYPE_STRING);
      g_list_store_append (mw->store, row);
      g_object_unref (row);
      gimv_row_set (row,
                          COL_PATH,  f[0],
                          COL_LAST,  f[1] ? f[1] : "",
                          COL_TAGS,  (f[1] && f[2]) ? f[2] : "",
                          COL_MOVED, moved ? moved : "",
                          COL_STATE, state_of (f[0]),
                          -1);
      g_strfreev (f);
   }
   g_strfreev (lines);
   g_free (text);
}


/* write back the lines still in the list; delete the log when none is */
static void
save_log (MissingWin *mw)
{
   GListModel *model = G_LIST_MODEL (mw->store);
   GString *s;
   gboolean any = FALSE;
   guint i, n;

   if (!mw->log_path) return;

   s = g_string_new (NULL);
   for (i = 0; i < mw->header->len; i++)
      g_string_append_printf (s, "%s\n", (gchar *) g_ptr_array_index (mw->header, i));

   n = g_list_model_get_n_items (model);
   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (model, i);
      const gchar *moved = gimv_row_peek_string (row, COL_MOVED);
      g_string_append_printf (s, "%s\t%s\t%s", gimv_row_peek_string (row, COL_PATH),
                              gimv_row_peek_string (row, COL_LAST),
                              gimv_row_peek_string (row, COL_TAGS));
      if (moved && *moved) g_string_append_printf (s, "\t" MOVED_PREFIX "%s", moved);
      g_string_append_c (s, '\n');
      any = TRUE;
   }

   if (any)
      g_file_set_contents (mw->log_path, s->str, s->len, NULL);
   else
      g_unlink (mw->log_path);
   g_string_free (s, TRUE);
}


static void fill_logs (MissingWin *mw, const gchar *select);


/******************************************************************************
 *  actions
 ******************************************************************************/

/* the comment file of "from" goes to the image "to" */
static gboolean
reattach (MissingWin *mw, const gchar *from_img, const gchar *to_img, GString *errors)
{
   gchar *from = comment_path (from_img), *to = comment_path (to_img), *dir;
   gboolean ok = FALSE;

   if (!g_file_test (from, G_FILE_TEST_IS_REGULAR)) {
      ok = TRUE;                        /* nothing left to move */
   } else if (g_file_test (to, G_FILE_TEST_EXISTS)) {
      g_string_append_printf (errors, _("%s already has a comment file.\n"), to_img);
   } else {
      dir = g_path_get_dirname (to);
      g_mkdir_with_parents (dir, 0700);
      g_free (dir);
      if (g_rename (from, to) == 0) {
         ok = TRUE;
         gimv_index_tags_changed (to_img);
      } else {
         g_string_append_printf (errors, "%s: %s\n", to_img, g_strerror (errno));
      }
   }
   g_free (from);
   g_free (to);
   return ok;
}


typedef enum {
   ACT_MOVED,     /* to where the image went */
   ACT_DELETE,
   ACT_DROP,      /* only out of the log */
} Action;


/* the rows selected (referenced), in order */
static GPtrArray *
selected_rows (MissingWin *mw)
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
remove_row (MissingWin *mw, GimvRow *row)
{
   guint pos;
   if (gimv_row_store_find (G_LIST_MODEL (mw->store), row, &pos))
      g_list_store_remove (mw->store, pos);
}


static void
apply_to_selected (MissingWin *mw, Action act)
{
   GPtrArray *rows = selected_rows (mw);
   GString *errors = g_string_new (NULL);
   guint done = 0, i;

   for (i = 0; i < rows->len; i++) {
      GimvRow *row = g_ptr_array_index (rows, i);
      const gchar *path = gimv_row_peek_string (row, COL_PATH);
      const gchar *moved = gimv_row_peek_string (row, COL_MOVED);
      gboolean handled = FALSE;

      switch (act) {
      case ACT_MOVED:
         if (moved && *moved)
            handled = reattach (mw, path, moved, errors);
         break;
      case ACT_DELETE: {
         gchar *c = comment_path (path);
         handled = !g_file_test (c, G_FILE_TEST_EXISTS) || g_unlink (c) == 0;
         if (!handled)
            g_string_append_printf (errors, "%s: %s\n", c, g_strerror (errno));
         g_free (c);
         break;
      }
      case ACT_DROP:
         handled = TRUE;
         break;
      }
      if (handled) {
         remove_row (mw, row);
         done++;
      }
   }
   g_ptr_array_unref (rows);

   if (done) save_log (mw);

   /* the log is gone when it is empty */
   if (done && mw->log_path && !g_file_test (mw->log_path, G_FILE_TEST_EXISTS))
      fill_logs (mw, NULL);

   {
      gchar *text = g_strdup_printf (ngettext ("%'u line done.", "%'u lines done.", done), done);
      if (errors->len) {
         gchar *all = g_strconcat (text, "\n", errors->str, NULL);
         g_strchomp (all);
         set_info (mw, all);
         g_free (all);
      } else {
         set_info (mw, text);
      }
      g_free (text);
   }
   g_string_free (errors, TRUE);
}


static void
cb_moved (GtkButton *b, MissingWin *mw)  { apply_to_selected (mw, ACT_MOVED); }
static void
cb_delete (GtkButton *b, MissingWin *mw) { apply_to_selected (mw, ACT_DELETE); }
static void
cb_drop (GtkButton *b, MissingWin *mw)   { apply_to_selected (mw, ACT_DROP); }


static void
cb_other_chosen (GObject *source, GAsyncResult *res, gpointer data)
{
   MissingWin *mw = data;
   GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (source), res, NULL);
   GPtrArray *rows;
   gchar *to;

   if (!file || mw != the_win) {
      if (file) g_object_unref (file);
      return;
   }
   to = g_file_get_path (file);
   g_object_unref (file);

   rows = selected_rows (mw);
   if (to && rows->len == 1) {
      GimvRow *row = g_ptr_array_index (rows, 0);
      GString *errors = g_string_new (NULL);
      if (reattach (mw, gimv_row_peek_string (row, COL_PATH), to, errors)) {
         remove_row (mw, row);
         save_log (mw);
         if (mw->log_path && !g_file_test (mw->log_path, G_FILE_TEST_EXISTS))
            fill_logs (mw, NULL);
         set_info (mw, _("1 line done."));
      } else {
         set_info (mw, errors->str);
      }
      g_string_free (errors, TRUE);
   }
   g_ptr_array_unref (rows);
   g_free (to);
}


static void
cb_other (GtkButton *b, MissingWin *mw)
{
   GtkFileDialog *dialog = gtk_file_dialog_new ();
   GPtrArray *rows = selected_rows (mw);

   /* start in the folder the image was in */
   if (rows->len) {
      const gchar *path = gimv_row_peek_string (g_ptr_array_index (rows, 0), COL_PATH);
      if (path) {
         gchar *dir = g_path_get_dirname (path);
         if (g_file_test (dir, G_FILE_TEST_IS_DIR)) {
            GFile *f = g_file_new_for_path (dir);
            gtk_file_dialog_set_initial_folder (dialog, f);
            g_object_unref (f);
         }
         g_free (dir);
      }
   }
   g_ptr_array_unref (rows);

   gtk_file_dialog_set_title (dialog, _("The Image to Put the Comment On"));
   gtk_file_dialog_open (dialog, GTK_WINDOW (mw->window), NULL, cb_other_chosen, mw);
   g_object_unref (dialog);
}


static void
update_buttons (MissingWin *mw)
{
   GPtrArray *rows = selected_rows (mw);
   gboolean any_moved = FALSE;
   guint n = rows->len, i;

   for (i = 0; i < n && !any_moved; i++) {
      const gchar *moved = gimv_row_peek_string (g_ptr_array_index (rows, i), COL_MOVED);
      any_moved = moved && *moved;
   }
   g_ptr_array_unref (rows);

   gtk_widget_set_sensitive (mw->btn_moved, any_moved);
   gtk_widget_set_sensitive (mw->btn_other, n == 1);
   gtk_widget_set_sensitive (mw->btn_delete, n > 0);
   gtk_widget_set_sensitive (mw->btn_drop, n > 0);
}


static void
cb_selection_changed (GtkSelectionModel *sel, guint pos, guint n, MissingWin *mw)
{
   update_buttons (mw);
}


/******************************************************************************
 *  window
 ******************************************************************************/

static void
cb_log_selected (GObject *dropdown, GParamSpec *pspec, MissingWin *mw)
{
   guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));
   const gchar *name = i != GTK_INVALID_LIST_POSITION
                       ? gtk_string_list_get_string (mw->log_names, i) : NULL;
   gchar *dir, *path;

   if (!name) {
      load_log (mw, NULL);
      update_buttons (mw);
      return;
   }
   dir = log_dir ();
   path = g_build_filename (dir, name, NULL);
   load_log (mw, path);
   update_buttons (mw);
   g_free (path);
   g_free (dir);
}


/* select: the log to show (a full path), NULL: the newest */
static void
fill_logs (MissingWin *mw, const gchar *select)
{
   GPtrArray *names = list_logs ();
   gchar *base = select ? g_path_get_basename (select) : NULL;
   guint i, pos = 0;

   g_signal_handlers_block_by_func (mw->logs, cb_log_selected, mw);
   gtk_string_list_splice (mw->log_names, 0,
                           g_list_model_get_n_items (G_LIST_MODEL (mw->log_names)), NULL);
   for (i = 0; i < names->len; i++) {
      gtk_string_list_append (mw->log_names, g_ptr_array_index (names, i));
      if (base && !strcmp (base, g_ptr_array_index (names, i))) pos = i;
   }
   g_signal_handlers_unblock_by_func (mw->logs, cb_log_selected, mw);

   if (names->len) {
      gtk_drop_down_set_selected (GTK_DROP_DOWN (mw->logs), pos);
      cb_log_selected (G_OBJECT (mw->logs), NULL, mw);
   } else {
      load_log (mw, NULL);
      update_buttons (mw);
      set_info (mw, _("There is no log of missing images."));
   }
   g_free (base);
   g_ptr_array_unref (names);
}


static void
cb_destroy (GtkWidget *w, MissingWin *mw)
{
   if (the_win == mw) the_win = NULL;
   g_object_unref (mw->store);
   g_ptr_array_unref (mw->header);
   g_free (mw->log_path);
   g_free (mw);
}


static GtkWidget *
cell_setup (gpointer data)
{
   GtkWidget *label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   if (GPOINTER_TO_INT (data))
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
   return label;
}


static void
cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   gint col = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (cell), "gimv-missing-col"));
   const gchar *text = gimv_row_peek_string (row, col);
   gtk_label_set_text (GTK_LABEL (gimv_cell_get_content (cell)), text ? text : "");
   gtk_widget_set_tooltip_text (cell, GPOINTER_TO_INT (data) ? text : NULL);
}


static GtkWidget *
cell_setup_col (gpointer data)
{
   /* data: (col << 1) | ellipsize */
   GtkWidget *label = cell_setup (GINT_TO_POINTER (GPOINTER_TO_INT (data) & 1));
   g_object_set_data (G_OBJECT (label), "gimv-missing-col",
                      GINT_TO_POINTER (GPOINTER_TO_INT (data) >> 1));
   return label;
}


static void
cell_bind_col (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   cell_bind (cell, row, item, GINT_TO_POINTER (GPOINTER_TO_INT (data) & 1));
}


static void
add_column (MissingWin *mw, const gchar *title, gint col, gint width)
{
   gpointer data = GINT_TO_POINTER ((col << 1) | (width > 0));
   GtkColumnViewColumn *c
      = gimv_column_view_append (GTK_COLUMN_VIEW (mw->tree), title,
                                 gimv_cell_factory_new (cell_setup_col, cell_bind_col,
                                                        data, NULL, FALSE));
   gtk_column_view_column_set_resizable (c, TRUE);
   if (width > 0)
      gtk_column_view_column_set_fixed_width (c, width);
}


static GtkWidget *
button_new (const gchar *label, GCallback cb, MissingWin *mw)
{
   GtkWidget *b = gtk_button_new_with_label (label);
   if (cb) g_signal_connect (b, "clicked", cb, mw);
   return b;
}


void
gimv_index_missing_open (GtkWindow *parent, const gchar *log_path)
{
   MissingWin *mw;
   GtkWidget *vbox, *hbox, *scroll, *label, *b;

   if (the_win) {
      fill_logs (the_win, log_path);
      gtk_window_present (GTK_WINDOW (the_win->window));
      return;
   }

   mw = the_win = g_new0 (MissingWin, 1);
   mw->header = g_ptr_array_new_with_free_func (g_free);
   mw->store = gimv_row_store_new ();

   mw->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (mw->window), _("Missing Images"));
   gtk_window_set_default_size (GTK_WINDOW (mw->window), 900, 480);
   if (parent) gtk_window_set_transient_for (GTK_WINDOW (mw->window), parent);
   g_signal_connect (mw->window, "destroy", G_CALLBACK (cb_destroy), mw);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
   gtk_widget_set_margin_start (vbox, 8);
   gtk_widget_set_margin_end (vbox, 8);
   gtk_widget_set_margin_top (vbox, 8);
   gtk_widget_set_margin_bottom (vbox, 8);
   gtk_window_set_child (GTK_WINDOW (mw->window), vbox);

   label = gtk_label_new (_("Images with comments (tags) that went missing. Their comment "
                            "files are kept: put them on the image where it went or on "
                            "another image, or delete them."));
   gtk_label_set_wrap (GTK_LABEL (label), TRUE);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (vbox), label);

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_box_append (GTK_BOX (hbox), gtk_label_new (_("Log:")));
   mw->log_names = gtk_string_list_new (NULL);
   mw->logs = gtk_drop_down_new (G_LIST_MODEL (mw->log_names), NULL);
   g_signal_connect (mw->logs, "notify::selected", G_CALLBACK (cb_log_selected), mw);
   gtk_box_append (GTK_BOX (hbox), mw->logs);
   gtk_box_append (GTK_BOX (vbox), hbox);

   mw->tree = gimv_column_view_new (G_LIST_MODEL (g_object_ref (mw->store)),
                                    GTK_SELECTION_MULTIPLE);
   add_column (mw, _("Path"), COL_PATH, 280);
   add_column (mw, _("Last Seen"), COL_LAST, 0);
   add_column (mw, _("Tags"), COL_TAGS, 90);
   add_column (mw, _("Moved To"), COL_MOVED, 200);
   add_column (mw, _("State"), COL_STATE, 0);
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
   mw->btn_moved  = button_new (_("Put on Where It Went"), G_CALLBACK (cb_moved), mw);
   mw->btn_other  = button_new (_("Put on Another Image..."), G_CALLBACK (cb_other), mw);
   mw->btn_delete = button_new (_("Delete the Comment"), G_CALLBACK (cb_delete), mw);
   mw->btn_drop   = button_new (_("Remove from the Log"), G_CALLBACK (cb_drop), mw);
   gtk_box_append (GTK_BOX (hbox), mw->btn_moved);
   gtk_box_append (GTK_BOX (hbox), mw->btn_other);
   gtk_box_append (GTK_BOX (hbox), mw->btn_delete);
   gtk_box_append (GTK_BOX (hbox), mw->btn_drop);
   b = button_new (_("Close"), NULL, mw);
   g_signal_connect_swapped (b, "clicked", G_CALLBACK (gtk_window_destroy), mw->window);
   gtk_widget_set_hexpand (b, TRUE);
   gtk_widget_set_halign (b, GTK_ALIGN_END);
   gtk_box_append (GTK_BOX (hbox), b);
   gtk_box_append (GTK_BOX (vbox), hbox);

   fill_logs (mw, log_path);
   gtk_window_present (GTK_WINDOW (mw->window));
}
