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
   GtkListStore *store;
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

   gtk_list_store_clear (mw->store);
   g_ptr_array_set_size (mw->header, 0);
   g_free (mw->log_path);
   mw->log_path = path ? g_strdup (path) : NULL;
   set_info (mw, NULL);
   if (!path || !g_file_get_contents (path, &text, NULL, NULL)) return;

   lines = g_strsplit (text, "\n", -1);
   for (i = 0; lines[i]; i++) {
      gchar **f;
      GtkTreeIter iter;
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
      gtk_list_store_append (mw->store, &iter);
      gtk_list_store_set (mw->store, &iter,
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
   GtkTreeModel *model = GTK_TREE_MODEL (mw->store);
   GtkTreeIter iter;
   GString *s;
   gboolean any = FALSE, valid;
   guint i;

   if (!mw->log_path) return;

   s = g_string_new (NULL);
   for (i = 0; i < mw->header->len; i++)
      g_string_append_printf (s, "%s\n", (gchar *) g_ptr_array_index (mw->header, i));

   for (valid = gtk_tree_model_get_iter_first (model, &iter); valid;
        valid = gtk_tree_model_iter_next (model, &iter))
   {
      gchar *path, *last, *tags, *moved;
      gtk_tree_model_get (model, &iter, COL_PATH, &path, COL_LAST, &last,
                          COL_TAGS, &tags, COL_MOVED, &moved, -1);
      g_string_append_printf (s, "%s\t%s\t%s", path, last, tags);
      if (moved && *moved) g_string_append_printf (s, "\t" MOVED_PREFIX "%s", moved);
      g_string_append_c (s, '\n');
      g_free (path); g_free (last); g_free (tags); g_free (moved);
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


static void
apply_to_selected (MissingWin *mw, Action act)
{
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->tree));
   GtkTreeModel *model;
   GList *rows = gtk_tree_selection_get_selected_rows (sel, &model), *node;
   GList *refs = NULL;
   GString *errors = g_string_new (NULL);
   guint done = 0;

   for (node = rows; node; node = g_list_next (node))
      refs = g_list_append (refs, gtk_tree_row_reference_new (model, node->data));
   g_list_free_full (rows, (GDestroyNotify) gtk_tree_path_free);

   for (node = refs; node; node = g_list_next (node)) {
      GtkTreePath *tp = gtk_tree_row_reference_get_path (node->data);
      GtkTreeIter iter;
      gchar *path = NULL, *moved = NULL;
      gboolean handled = FALSE;

      if (!tp || !gtk_tree_model_get_iter (model, &iter, tp)) {
         if (tp) gtk_tree_path_free (tp);
         continue;
      }
      gtk_tree_model_get (model, &iter, COL_PATH, &path, COL_MOVED, &moved, -1);

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
         gtk_list_store_remove (mw->store, &iter);
         done++;
      }
      g_free (path);
      g_free (moved);
      gtk_tree_path_free (tp);
   }
   g_list_free_full (refs, (GDestroyNotify) gtk_tree_row_reference_free);

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
   GtkTreeSelection *sel;
   GtkTreeModel *model;
   GList *rows;
   gchar *to;

   if (!file || mw != the_win) {
      if (file) g_object_unref (file);
      return;
   }
   to = g_file_get_path (file);
   g_object_unref (file);

   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->tree));
   rows = gtk_tree_selection_get_selected_rows (sel, &model);
   if (to && rows && !rows->next) {
      GtkTreeIter iter;
      if (gtk_tree_model_get_iter (model, &iter, rows->data)) {
         gchar *path = NULL;
         GString *errors = g_string_new (NULL);
         gtk_tree_model_get (model, &iter, COL_PATH, &path, -1);
         if (reattach (mw, path, to, errors)) {
            gtk_list_store_remove (mw->store, &iter);
            save_log (mw);
            if (mw->log_path && !g_file_test (mw->log_path, G_FILE_TEST_EXISTS))
               fill_logs (mw, NULL);
            set_info (mw, _("1 line done."));
         } else {
            set_info (mw, errors->str);
         }
         g_string_free (errors, TRUE);
         g_free (path);
      }
   }
   g_list_free_full (rows, (GDestroyNotify) gtk_tree_path_free);
   g_free (to);
}


static void
cb_other (GtkButton *b, MissingWin *mw)
{
   GtkFileDialog *dialog = gtk_file_dialog_new ();
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->tree));
   GtkTreeModel *model;
   GList *rows = gtk_tree_selection_get_selected_rows (sel, &model);

   /* start in the folder the image was in */
   if (rows) {
      GtkTreeIter iter;
      gchar *path = NULL;
      if (gtk_tree_model_get_iter (model, &iter, rows->data))
         gtk_tree_model_get (model, &iter, COL_PATH, &path, -1);
      if (path) {
         gchar *dir = g_path_get_dirname (path);
         if (g_file_test (dir, G_FILE_TEST_IS_DIR)) {
            GFile *f = g_file_new_for_path (dir);
            gtk_file_dialog_set_initial_folder (dialog, f);
            g_object_unref (f);
         }
         g_free (dir);
         g_free (path);
      }
   }
   g_list_free_full (rows, (GDestroyNotify) gtk_tree_path_free);

   gtk_file_dialog_set_title (dialog, _("The Image to Put the Comment On"));
   gtk_file_dialog_open (dialog, GTK_WINDOW (mw->window), NULL, cb_other_chosen, mw);
   g_object_unref (dialog);
}


static void
update_buttons (MissingWin *mw)
{
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->tree));
   GtkTreeModel *model;
   GList *rows = gtk_tree_selection_get_selected_rows (sel, &model), *node;
   gboolean any_moved = FALSE;
   guint n = g_list_length (rows);

   for (node = rows; node && !any_moved; node = g_list_next (node)) {
      GtkTreeIter iter;
      gchar *moved = NULL;
      if (gtk_tree_model_get_iter (model, &iter, node->data))
         gtk_tree_model_get (model, &iter, COL_MOVED, &moved, -1);
      any_moved = moved && *moved;
      g_free (moved);
   }
   g_list_free_full (rows, (GDestroyNotify) gtk_tree_path_free);

   gtk_widget_set_sensitive (mw->btn_moved, any_moved);
   gtk_widget_set_sensitive (mw->btn_other, n == 1);
   gtk_widget_set_sensitive (mw->btn_delete, n > 0);
   gtk_widget_set_sensitive (mw->btn_drop, n > 0);
}


static void
cb_selection_changed (GtkTreeSelection *sel, MissingWin *mw)
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


static void
add_column (MissingWin *mw, const gchar *title, gint col, gint width)
{
   GtkCellRenderer *r = gtk_cell_renderer_text_new ();
   GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes (title, r, "text", col, NULL);
   gtk_tree_view_column_set_resizable (c, TRUE);
   if (width > 0) {
      gtk_tree_view_column_set_sizing (c, GTK_TREE_VIEW_COLUMN_FIXED);
      gtk_tree_view_column_set_fixed_width (c, width);
      g_object_set (r, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
   }
   gtk_tree_view_append_column (GTK_TREE_VIEW (mw->tree), c);
}


static GtkWidget *
button_new (const gchar *label, GCallback cb, MissingWin *mw)
{
   GtkWidget *b = gtk_button_new_with_label (label);
   g_signal_connect (b, "clicked", cb, mw);
   return b;
}


void
gimv_index_missing_open (GtkWindow *parent, const gchar *log_path)
{
   MissingWin *mw;
   GtkWidget *vbox, *hbox, *scroll, *label, *b;
   GtkTreeSelection *sel;

   if (the_win) {
      fill_logs (the_win, log_path);
      gtk_window_present (GTK_WINDOW (the_win->window));
      return;
   }

   mw = the_win = g_new0 (MissingWin, 1);
   mw->header = g_ptr_array_new_with_free_func (g_free);
   mw->store = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING);

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

   mw->tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL (mw->store));
   add_column (mw, _("Path"), COL_PATH, 280);
   add_column (mw, _("Last Seen"), COL_LAST, 0);
   add_column (mw, _("Tags"), COL_TAGS, 90);
   add_column (mw, _("Moved To"), COL_MOVED, 200);
   add_column (mw, _("State"), COL_STATE, 0);
   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (mw->tree));
   gtk_tree_selection_set_mode (sel, GTK_SELECTION_MULTIPLE);
   g_signal_connect (sel, "changed", G_CALLBACK (cb_selection_changed), mw);

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
