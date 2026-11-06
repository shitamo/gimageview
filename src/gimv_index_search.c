/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView
 * Copyright (C) 2026 shitamo
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * The search window (gimv_index_search.h).
 *
 * Decided with the user (2026-11): a window of its own; the conditions as
 * tags put one after the other ("all of them"), each can be turned into
 * "not this"; under them the tags of the images found with their numbers
 * (narrowing: no condition that finds nothing), filtered by what is typed;
 * a period.  No "or": it would make it harder to understand.  The images
 * found go to a tab of the thumbnail window, made again at each change.
 * The searches kept ("bookmarks") are in ~/.gimv/searches.
 *
 * The keys of the tags are those of the tag tab: "kyoto" a tag put by a
 * person, "person/NAME" a person in the image, ",person/TAG" a tag of the
 * people in it, ",folder/..." etc. a tag of a rule.
 */

#include "gimageview.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gio/gio.h>

#include "fileload.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_search.h"
#include "gimv_index_view.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"

#define NAME_TAG        "person/"
#define NAME_TAG_LEN    7
#define SEARCH_DELAY_MS 250
/* the tags of a kind shown when nothing is typed (the rest by typing) */
#define TAGS_SHOWN      40
#define TAGS_SHOWN_MAX  400
#define SAVED_FILE      "searches"

typedef struct {
   gchar   *key;
   gboolean exclude;
} Cond;

typedef struct {
   gchar *key;
   gint   kind;      /* gimv_index_tag_kind () */
   gint   count;
} Facet;

typedef struct {
   GPtrArray *conds;    /* Cond (copies) */
   gchar     *from, *to;
} Query;

typedef struct {
   gint        total;   /* the images found (all the images: no condition) */
   gboolean    any;     /* conditions were given: the images listed */
   GPtrArray  *facets;  /* Facet */
   GList      *paths;
   GHashTable *stats;   /* path -> struct stat */
} Result;

static struct {
   GtkWidget     *window;
   GimvThumbWin  *tw;          /* weak */
   GimvThumbView *tv;          /* weak: the tab of the results */
   GtkWidget     *conds_box;   /* the conditions (a flow box) */
   GtkWidget     *none_label;
   GtkWidget     *entry;       /* to filter the tags shown */
   GtkWidget     *from_entry, *to_entry;
   GtkWidget     *count_label;
   GtkWidget     *tags_list;   /* a list box */
   GtkWidget     *saved_drop;
   GtkStringList *saved_names;
   GtkWidget     *delete_button;
   GPtrArray     *conds;       /* Cond */
   GPtrArray     *facets;      /* of the last search */
   GCancellable  *cancel;
   guint          timeout;
   guint          watch;
   gboolean       loading_saved;
} sw;


static void schedule_search (void);
static void rebuild_conds (void);
static void rebuild_tags (void);


static void
cond_free (Cond *c)
{
   g_free (c->key);
   g_free (c);
}


static void
facet_free (Facet *f)
{
   g_free (f->key);
   g_free (f);
}


static void
query_free (Query *q)
{
   g_ptr_array_unref (q->conds);
   g_free (q->from);
   g_free (q->to);
   g_free (q);
}


static void
result_free (Result *r)
{
   if (r->facets) g_ptr_array_unref (r->facets);
   g_list_free_full (r->paths, g_free);   /* the keys of stats too */
   if (r->stats) g_hash_table_unref (r->stats);
   g_free (r);
}


/* how a condition is shown */
static gchar *
cond_label (const Cond *c)
{
   gchar *l = gimv_index_tag_label (c->key), *out;
   if (gimv_index_tag_kind (c->key) == 1) {
      gchar *t = g_strdup_printf (_("Person: %s"), l);
      g_free (l);
      l = t;
   }
   out = c->exclude ? g_strdup_printf (_("not %s"), l) : g_strdup (l);
   g_free (l);
   return out;
}


/******************************************************************************
 *  the search (a worker thread, a connection of its own, read only)
 ******************************************************************************/

/* "f.id IN (...)" for a key; the parameters (strings) appended to args */
static gchar *
cond_sql (const Cond *c, GPtrArray *args)
{
   const gchar *k = gimv_index_plain_key (c->key);
   const gchar *in = c->exclude ? "NOT IN" : "IN";
   gint kind = gimv_index_tag_kind (k);

   switch (kind) {
   case 1:   /* a person: named on a face, or the name in the comment file */
      g_ptr_array_add (args, g_strdup (k + NAME_TAG_LEN));
      g_ptr_array_add (args, g_strdup (k));
      return g_strdup_printf ("f.id %s (SELECT fa.file_id FROM faces fa JOIN persons p "
                              "ON p.id = fa.person_id WHERE p.name = ? UNION "
                              "SELECT file_id FROM file_tags WHERE auto = 0 AND tag_id = "
                              "(SELECT id FROM tags WHERE name = ?))", in);
   case 2:   /* a tag of the people in it */
      g_ptr_array_add (args, g_strdup (k + 1 + NAME_TAG_LEN));
      return g_strdup_printf ("f.id %s (SELECT fa.file_id FROM faces fa JOIN person_tags pt "
                              "ON pt.person_id = fa.person_id WHERE pt.tag = ?)", in);
   case 3:   /* a tag of a rule */
      g_ptr_array_add (args, g_strdup (k + 1));
      return g_strdup_printf ("f.id %s (SELECT file_id FROM file_tags WHERE auto = 1 AND "
                              "tag_id = (SELECT id FROM tags WHERE name = ?))", in);
   default:  /* put by a person, or a keyword of another program */
      g_ptr_array_add (args, g_strdup (k));
      g_ptr_array_add (args, g_strdup (k));
      return g_strdup_printf ("f.id %s (" GIMV_INDEX_TAG_FILES_SQL ")", in);
   }
}


static void
add_facets (sqlite3 *db, const gchar *sql, gint kind_auto, GPtrArray *out)
{
   sqlite3_stmt *st;

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) return;
   while (sqlite3_step (st) == SQLITE_ROW) {
      const gchar *name = (const gchar *) sqlite3_column_text (st, 0);
      Facet *f;
      if (!name) continue;
      f = g_new0 (Facet, 1);
      if (kind_auto < 0) {
         /* tags of file_tags: column 2 says auto */
         gboolean is_auto = sqlite3_column_int (st, 2) != 0;
         if (!is_auto && g_str_has_prefix (name, NAME_TAG)) {   /* the names apart */
            g_free (f);
            continue;
         }
         f->key = is_auto ? g_strconcat (",", name, NULL) : g_strdup (name);
      } else if (kind_auto == 1) {
         f->key = g_strconcat (NAME_TAG, name, NULL);
      } else {
         f->key = g_strconcat (",", NAME_TAG, name, NULL);
      }
      f->kind = gimv_index_tag_kind (f->key);
      f->count = sqlite3_column_int (st, 1);
      g_ptr_array_add (out, f);
   }
   sqlite3_finalize (st);
}


static gint
cmp_facets (gconstpointer a, gconstpointer b)
{
   const Facet *x = *(const Facet **) a, *y = *(const Facet **) b;
   if (x->kind != y->kind) return x->kind - y->kind;
   if (x->count != y->count) return y->count - x->count;
   return g_utf8_collate (x->key, y->key);
}


static void
thread_search (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   Query *q = data;
   Result *r = g_new0 (Result, 1);
   gchar *dbfile = gimv_index_db_default_path ();
   sqlite3 *db = NULL;
   sqlite3_stmt *st;
   GString *sql = g_string_new ("CREATE TEMP TABLE r AS SELECT f.id AS id FROM files f WHERE 1");
   GPtrArray *args = g_ptr_array_new_with_free_func (g_free);
   guint i;

   r->facets = g_ptr_array_new_with_free_func ((GDestroyNotify) facet_free);
   r->any = q->conds->len > 0 || q->from || q->to;

   for (i = 0; i < q->conds->len; i++) {
      gchar *c = cond_sql (g_ptr_array_index (q->conds, i), args);
      g_string_append_printf (sql, " AND %s", c);
      g_free (c);
   }
   /* the dates of the index are "YYYY-MM-DD HH:MM:SS": a prefix is a span */
   if (q->from) {
      g_string_append (sql, " AND f.date >= ?");
      g_ptr_array_add (args, g_strdup (q->from));
   }
   if (q->to) {
      g_string_append (sql, " AND f.date < ? || '~'");
      g_ptr_array_add (args, g_strdup (q->to));
   }

   if (gimv_index_db_open (dbfile, FALSE, &db, NULL) != GIMV_INDEX_DB_OK || !db) {
      g_free (dbfile);
      g_string_free (sql, TRUE);
      g_ptr_array_unref (args);
      if (db) sqlite3_close (db);
      g_task_return_pointer (task, r, (GDestroyNotify) result_free);
      return;
   }
   g_free (dbfile);

   if (sqlite3_prepare_v2 (db, sql->str, -1, &st, NULL) == SQLITE_OK) {
      for (i = 0; i < args->len; i++)
         sqlite3_bind_text (st, i + 1, g_ptr_array_index (args, i), -1, SQLITE_TRANSIENT);
      sqlite3_step (st);
      sqlite3_finalize (st);
   }
   g_string_free (sql, TRUE);
   g_ptr_array_unref (args);

   if (sqlite3_prepare_v2 (db, "SELECT count(*) FROM r", -1, &st, NULL) == SQLITE_OK) {
      if (sqlite3_step (st) == SQLITE_ROW) r->total = sqlite3_column_int (st, 0);
      sqlite3_finalize (st);
   }

   /* the tags of the images found */
   if (!g_cancellable_is_cancelled (cancel)) {
      /* the keywords as the tags put by a person of the same name */
      add_facets (db, "SELECT n, count(DISTINCT id), a FROM ("
                      " SELECT " GIMV_INDEX_TAG_NAME_SQL " AS n, ft.file_id AS id,"
                      "  " GIMV_INDEX_TAG_AUTO_SQL " AS a FROM r"
                      "  JOIN file_tags ft ON ft.file_id = r.id JOIN tags t ON t.id = ft.tag_id)"
                      " GROUP BY n, a", -1, r->facets);
      add_facets (db, "SELECT n, count(DISTINCT id) FROM ("
                      " SELECT p.name AS n, fa.file_id AS id FROM r JOIN faces fa ON fa.file_id = r.id"
                      "  JOIN persons p ON p.id = fa.person_id"
                      " UNION SELECT substr (t.name, 8), ft.file_id FROM r JOIN file_tags ft"
                      "  ON ft.file_id = r.id JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 0"
                      "  AND t.name >= 'person/' AND t.name < 'person0') GROUP BY n", 1, r->facets);
      add_facets (db, "SELECT pt.tag, count(DISTINCT fa.file_id) FROM r JOIN faces fa"
                      " ON fa.file_id = r.id JOIN person_tags pt ON pt.person_id = fa.person_id"
                      " GROUP BY pt.tag", 2, r->facets);
      g_ptr_array_sort (r->facets, cmp_facets);
   }

   /* the images, when there is a condition */
   if (r->any && !g_cancellable_is_cancelled (cancel)
       && sqlite3_prepare_v2 (db, "SELECT d.path, f.name, f.size, f.mtime FROM r "
                                  "JOIN files f ON f.id = r.id JOIN dirs d ON d.id = f.dir_id "
                                  "ORDER BY f.date, f.name", -1, &st, NULL) == SQLITE_OK)
   {
      /* as the index view: the paths owned by the list (the loader frees both) */
      r->stats = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
      while (sqlite3_step (st) == SQLITE_ROW) {
         gchar *path = g_build_filename ((const gchar *) sqlite3_column_text (st, 0),
                                         (const gchar *) sqlite3_column_text (st, 1), NULL);
         struct stat *sb = g_new0 (struct stat, 1);
         sb->st_mode  = S_IFREG | 0644;
         sb->st_size  = sqlite3_column_int64 (st, 2);
         sb->st_mtime = sqlite3_column_int64 (st, 3);
         sb->st_ctime = sb->st_atime = sb->st_mtime;
         sb->st_uid   = getuid ();
         sb->st_gid   = getgid ();
         r->paths = g_list_prepend (r->paths, path);
         g_hash_table_insert (r->stats, path, sb);
      }
      sqlite3_finalize (st);
      r->paths = g_list_reverse (r->paths);
   }
   sqlite3_close (db);
   g_task_return_pointer (task, r, (GDestroyNotify) result_free);
}


/******************************************************************************
 *  the results in a tab of the thumbnail window
 ******************************************************************************/

static void
cb_tv_gone (gpointer data, GObject *where)
{
   sw.tv = NULL;
}


static void
set_result_tab (GimvThumbView *tv)
{
   if (sw.tv) g_object_weak_unref (G_OBJECT (sw.tv), cb_tv_gone, NULL);
   sw.tv = tv;
   if (tv) g_object_weak_ref (G_OBJECT (tv), cb_tv_gone, NULL);
}


static gchar *
results_title (void)
{
   GString *s = g_string_new (NULL);
   guint i;

   for (i = 0; i < sw.conds->len; i++) {
      gchar *l = cond_label (g_ptr_array_index (sw.conds, i));
      if (s->len) g_string_append (s, ", ");
      g_string_append (s, l);
      g_free (l);
   }
   if (!s->len) g_string_append (s, _("Period"));
   g_string_prepend (s, _("Search: "));
   return g_string_free (s, FALSE);
}


/* FALSE: the files are being loaded elsewhere, later */
static gboolean
show_results (Result *r)
{
   FilesLoader *files;
   GimvThumbView *tv = sw.tv;
   gchar *title;
   gint page;

   if (!sw.tw || !r->paths) return TRUE;
   if (files_loader_query_loading ()) return FALSE;

   files = files_loader_new ();
   files->filelist = r->paths;
   files->known_stats = r->stats;
   r->paths = NULL;   /* the loader owns them now */
   r->stats = NULL;
   files->status = END;
   files->thumb_load_type = LOAD_CACHE;

   if (!tv || tv->tw != sw.tw || tv->progress) {
      tv = gimv_thumb_win_create_new_tab (sw.tw);
      set_result_tab (tv);
   }
   /* the name alone (the path in the tooltip and the status bar) */
   g_object_set_data (G_OBJECT (tv), "gimv-name-only", GINT_TO_POINTER (TRUE));
   gimv_thumb_view_set_windowed (tv, TRUE);
   page = gtk_notebook_page_num (GTK_NOTEBOOK (sw.tw->notebook), tv->container);
   if (page >= 0) gtk_notebook_set_current_page (GTK_NOTEBOOK (sw.tw->notebook), page);
   title = results_title ();
   gimv_thumb_view_reset_tab_label (tv, title);
   g_free (title);
   open_image_files_in_thumbnail_view (files, sw.tw, tv);
   files_loader_delete (files);
   /* the search window stays in front, to go on narrowing */
   if (sw.window) gtk_window_present (GTK_WINDOW (sw.window));
   return TRUE;
}


static void
cb_searched (GObject *source, GAsyncResult *res, gpointer data)
{
   GTask *task = G_TASK (res);
   Result *r;
   gchar *text;

   if (g_cancellable_is_cancelled (g_task_get_cancellable (task))) return;
   r = g_task_propagate_pointer (task, NULL);
   if (!r || !sw.window) {
      if (r) result_free (r);
      return;
   }

   if (r->any)
      text = g_strdup_printf (ngettext ("%'d image found", "%'d images found", r->total),
                              r->total);
   else
      text = g_strdup_printf (ngettext ("%'d image in the index: choose tags below",
                                        "%'d images in the index: choose tags below", r->total),
                              r->total);
   gtk_label_set_text (GTK_LABEL (sw.count_label), text);
   g_free (text);

   if (sw.facets) g_ptr_array_unref (sw.facets);
   sw.facets = g_ptr_array_ref (r->facets);
   rebuild_tags ();

   if (r->any && !show_results (r)) {
      /* a folder being opened: once more a little later */
      result_free (r);
      schedule_search ();
      return;
   }
   result_free (r);
}


static gboolean
cb_timeout (gpointer data)
{
   Query *q;
   GTask *task;
   guint i;
   const gchar *from, *to;

   sw.timeout = 0;
   if (!sw.window) return G_SOURCE_REMOVE;
   if (sw.cancel) {
      g_cancellable_cancel (sw.cancel);
      g_clear_object (&sw.cancel);
   }
   q = g_new0 (Query, 1);
   q->conds = g_ptr_array_new_with_free_func ((GDestroyNotify) cond_free);
   for (i = 0; i < sw.conds->len; i++) {
      Cond *c = g_ptr_array_index (sw.conds, i), *d = g_new0 (Cond, 1);
      d->key = g_strdup (c->key);
      d->exclude = c->exclude;
      g_ptr_array_add (q->conds, d);
   }
   from = gtk_editable_get_text (GTK_EDITABLE (sw.from_entry));
   to = gtk_editable_get_text (GTK_EDITABLE (sw.to_entry));
   q->from = from && *from ? g_strstrip (g_strdup (from)) : NULL;
   q->to = to && *to ? g_strstrip (g_strdup (to)) : NULL;
   if (q->from && !*q->from) g_clear_pointer (&q->from, g_free);
   if (q->to && !*q->to) g_clear_pointer (&q->to, g_free);

   sw.cancel = g_cancellable_new ();
   task = g_task_new (NULL, sw.cancel, cb_searched, NULL);
   g_task_set_task_data (task, q, (GDestroyNotify) query_free);
   g_task_run_in_thread (task, thread_search);
   g_object_unref (task);
   return G_SOURCE_REMOVE;
}


static void
schedule_search (void)
{
   /* changed by hand: no longer the search kept shown */
   if (!sw.loading_saved && sw.saved_drop
       && gtk_drop_down_get_selected (GTK_DROP_DOWN (sw.saved_drop)) != 0) {
      sw.loading_saved = TRUE;
      gtk_drop_down_set_selected (GTK_DROP_DOWN (sw.saved_drop), 0);
      gtk_widget_set_sensitive (sw.delete_button, FALSE);
      sw.loading_saved = FALSE;
   }
   if (sw.timeout) g_source_remove (sw.timeout);
   sw.timeout = g_timeout_add (SEARCH_DELAY_MS, cb_timeout, NULL);
}


/******************************************************************************
 *  the conditions
 ******************************************************************************/

static gint
cond_index (const gchar *key)
{
   guint i;
   for (i = 0; i < sw.conds->len; i++)
      if (!strcmp (((Cond *) g_ptr_array_index (sw.conds, i))->key, key)) return i;
   return -1;
}


static void
add_cond (const gchar *key, gboolean exclude)
{
   gint i = cond_index (key);
   Cond *c;

   if (i >= 0) {
      c = g_ptr_array_index (sw.conds, i);
      c->exclude = exclude;
   } else {
      c = g_new0 (Cond, 1);
      c->key = g_strdup (key);
      c->exclude = exclude;
      g_ptr_array_add (sw.conds, c);
   }
   /* the text typed has done its work */
   gtk_editable_set_text (GTK_EDITABLE (sw.entry), "");
   rebuild_conds ();
   schedule_search ();
}


static void
cb_cond_toggle (GtkButton *button, gpointer data)
{
   gint i = cond_index (g_object_get_data (G_OBJECT (button), "key"));
   if (i < 0) return;
   ((Cond *) g_ptr_array_index (sw.conds, i))->exclude ^= TRUE;
   rebuild_conds ();
   schedule_search ();
}


static void
cb_cond_remove (GtkButton *button, gpointer data)
{
   gint i = cond_index (g_object_get_data (G_OBJECT (button), "key"));
   if (i < 0) return;
   g_ptr_array_remove_index (sw.conds, i);
   rebuild_conds ();
   schedule_search ();
}


static void
rebuild_conds (void)
{
   GtkWidget *child;
   guint i;

   while ((child = gtk_widget_get_first_child (sw.conds_box)))
      gtk_flow_box_remove (GTK_FLOW_BOX (sw.conds_box), child);
   gtk_widget_set_visible (sw.none_label, sw.conds->len == 0);

   for (i = 0; i < sw.conds->len; i++) {
      Cond *c = g_ptr_array_index (sw.conds, i);
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
      GtkWidget *label_button = gtk_button_new ();
      GtkWidget *label = gtk_label_new (NULL);
      GtkWidget *x = gtk_button_new_from_icon_name ("window-close-symbolic");
      gchar *text = cond_label (c);

      gtk_widget_add_css_class (box, "linked");
      gtk_label_set_text (GTK_LABEL (label), text);
      /* "not ..." in red */
      if (c->exclude) gtk_widget_add_css_class (label_button, "destructive-action");
      gtk_button_set_child (GTK_BUTTON (label_button), label);
      gtk_widget_set_tooltip_text (label_button,
                                   c->exclude ? _("Excluded: click to look for it again")
                                              : _("Click to exclude the images with it"));
      g_object_set_data_full (G_OBJECT (label_button), "key", g_strdup (c->key), g_free);
      g_signal_connect (label_button, "clicked", G_CALLBACK (cb_cond_toggle), NULL);
      gtk_widget_set_tooltip_text (x, _("Remove the condition"));
      g_object_set_data_full (G_OBJECT (x), "key", g_strdup (c->key), g_free);
      g_signal_connect (x, "clicked", G_CALLBACK (cb_cond_remove), NULL);
      gtk_box_append (GTK_BOX (box), label_button);
      gtk_box_append (GTK_BOX (box), x);
      gtk_flow_box_append (GTK_FLOW_BOX (sw.conds_box), box);
      g_free (text);
   }
}


/******************************************************************************
 *  the tags of the images found
 ******************************************************************************/

static const gchar *
kind_title (gint kind)
{
   switch (kind) {
   case 0:  return _("Tags");
   case 1:  return _("People");
   case 2:  return _("Tags of People");
   default: return _("Automatic Tags");
   }
}


static void
cb_tag_add (GtkButton *button, gpointer data)
{
   add_cond (g_object_get_data (G_OBJECT (button), "key"), GPOINTER_TO_INT (data));
}


static void
cb_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
   const gchar *key = g_object_get_data (G_OBJECT (row), "key");
   if (key) add_cond (key, FALSE);
}


static void
rebuild_tags (void)
{
   GtkWidget *child;
   const gchar *typed = gtk_editable_get_text (GTK_EDITABLE (sw.entry));
   gchar *fold = typed && *typed ? g_utf8_casefold (typed, -1) : NULL;
   gint kind = -1, shown = 0, hidden = 0, all_shown = 0;
   guint i;

   while ((child = gtk_widget_get_first_child (sw.tags_list)))
      gtk_list_box_remove (GTK_LIST_BOX (sw.tags_list), child);
   if (!sw.facets) {
      g_free (fold);
      return;
   }

   for (i = 0; i < sw.facets->len; i++) {
      Facet *f = g_ptr_array_index (sw.facets, i);
      GtkWidget *row, *box, *label, *minus;
      gchar *l, *text;

      if (cond_index (f->key) >= 0) continue;   /* a condition already */
      l = gimv_index_tag_label (f->key);
      if (fold) {
         gchar *lf = g_utf8_casefold (l, -1);
         gboolean match = strstr (lf, fold) != NULL;
         g_free (lf);
         if (!match) {
            g_free (l);
            continue;
         }
      }
      if (f->kind != kind) {
         if (hidden) {
            gchar *m = g_strdup_printf (ngettext ("%'d more: type a part of the name",
                                                  "%'d more: type a part of the name", hidden),
                                        hidden);
            GtkWidget *more = gtk_label_new (m);
            gtk_widget_add_css_class (more, "dim-label");
            gtk_label_set_xalign (GTK_LABEL (more), 0.0);
            gtk_widget_set_margin_start (more, 12);
            gtk_list_box_append (GTK_LIST_BOX (sw.tags_list), more);
            gtk_list_box_row_set_activatable (
               GTK_LIST_BOX_ROW (gtk_widget_get_parent (more)), FALSE);
            g_free (m);
         }
         kind = f->kind;
         shown = hidden = 0;
         label = gtk_label_new (kind_title (kind));
         gtk_widget_add_css_class (label, "heading");
         gtk_label_set_xalign (GTK_LABEL (label), 0.0);
         gtk_widget_set_margin_top (label, 6);
         gtk_list_box_append (GTK_LIST_BOX (sw.tags_list), label);
         row = gtk_widget_get_parent (label);
         gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
         gtk_list_box_row_set_selectable (GTK_LIST_BOX_ROW (row), FALSE);
      }
      if (shown >= (fold ? TAGS_SHOWN_MAX : TAGS_SHOWN) || all_shown >= TAGS_SHOWN_MAX) {
         hidden++;
         g_free (l);
         continue;
      }
      shown++;
      all_shown++;

      box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
      text = g_strdup_printf ("%s (%'d)", l, f->count);
      label = gtk_label_new (text);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand (label, TRUE);
      gtk_widget_set_margin_start (label, 12);
      if (f->kind == 1)
         gtk_box_append (GTK_BOX (box), gtk_image_new_from_icon_name ("avatar-default-symbolic"));
      gtk_box_append (GTK_BOX (box), label);
      minus = gtk_button_new_with_label (_("Exclude"));
      gtk_button_set_has_frame (GTK_BUTTON (minus), FALSE);
      gtk_widget_add_css_class (minus, "caption");
      gtk_widget_set_tooltip_text (minus, _("Look for the images without it"));
      g_object_set_data_full (G_OBJECT (minus), "key", g_strdup (f->key), g_free);
      g_signal_connect (minus, "clicked", G_CALLBACK (cb_tag_add), GINT_TO_POINTER (TRUE));
      gtk_box_append (GTK_BOX (box), minus);
      gtk_list_box_append (GTK_LIST_BOX (sw.tags_list), box);
      row = gtk_widget_get_parent (box);
      g_object_set_data_full (G_OBJECT (row), "key", g_strdup (f->key), g_free);
      gtk_widget_set_tooltip_text (row, _("Click: only the images with it"));
      g_free (text);
      g_free (l);
   }
   if (hidden) {
      gchar *m = g_strdup_printf (ngettext ("%'d more: type a part of the name",
                                            "%'d more: type a part of the name", hidden), hidden);
      GtkWidget *more = gtk_label_new (m);
      gtk_widget_add_css_class (more, "dim-label");
      gtk_label_set_xalign (GTK_LABEL (more), 0.0);
      gtk_widget_set_margin_start (more, 12);
      gtk_list_box_append (GTK_LIST_BOX (sw.tags_list), more);
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (gtk_widget_get_parent (more)), FALSE);
      g_free (m);
   }
   g_free (fold);
}


static void
cb_entry_changed (GtkSearchEntry *entry, gpointer data)
{
   rebuild_tags ();
}


/* Enter: the first tag shown */
static void
cb_entry_activate (GtkSearchEntry *entry, gpointer data)
{
   GtkWidget *row;

   for (row = gtk_widget_get_first_child (sw.tags_list); row;
        row = gtk_widget_get_next_sibling (row)) {
      const gchar *key = g_object_get_data (G_OBJECT (row), "key");
      if (key) {
         add_cond (key, FALSE);
         return;
      }
   }
}


static void
cb_date_changed (GtkEditable *editable, gpointer data)
{
   schedule_search ();
}


/******************************************************************************
 *  the searches kept (~/.gimv/searches)
 ******************************************************************************/

static gchar *
saved_path (void)
{
   const gchar *home = g_getenv ("HOME");
   return g_build_filename (home ? home : g_get_home_dir (), GIMV_RC_DIR, SAVED_FILE, NULL);
}


static GKeyFile *
saved_load (void)
{
   GKeyFile *kf = g_key_file_new ();
   gchar *path = saved_path ();
   g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);
   g_free (path);
   return kf;
}


static void
saved_store (GKeyFile *kf)
{
   gchar *path = saved_path ();
   g_key_file_save_to_file (kf, path, NULL);
   g_free (path);
}


static void
saved_refresh (const gchar *select)
{
   GKeyFile *kf = saved_load ();
   gchar **groups = g_key_file_get_groups (kf, NULL);
   guint n = g_list_model_get_n_items (G_LIST_MODEL (sw.saved_names)), i, pos = 0;

   sw.loading_saved = TRUE;
   gtk_string_list_splice (sw.saved_names, 1, n - 1, NULL);
   for (i = 0; groups[i]; i++) {
      gtk_string_list_append (sw.saved_names, groups[i]);
      if (select && !strcmp (select, groups[i])) pos = i + 1;
   }
   gtk_drop_down_set_selected (GTK_DROP_DOWN (sw.saved_drop), pos);
   gtk_widget_set_sensitive (sw.delete_button, pos > 0);
   sw.loading_saved = FALSE;
   g_strfreev (groups);
   g_key_file_unref (kf);
}


static void
cb_saved_selected (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
   guint pos = gtk_drop_down_get_selected (drop);
   GKeyFile *kf;
   const gchar *name;
   gchar **inc, **exc, *from, *to;
   guint i;

   gtk_widget_set_sensitive (sw.delete_button, pos > 0 && pos != GTK_INVALID_LIST_POSITION);
   if (sw.loading_saved || pos == 0 || pos == GTK_INVALID_LIST_POSITION) return;
   name = gtk_string_list_get_string (sw.saved_names, pos);
   kf = saved_load ();
   sw.loading_saved = TRUE;
   inc = g_key_file_get_string_list (kf, name, "include", NULL, NULL);
   exc = g_key_file_get_string_list (kf, name, "exclude", NULL, NULL);
   from = g_key_file_get_string (kf, name, "from", NULL);
   to = g_key_file_get_string (kf, name, "to", NULL);
   g_ptr_array_set_size (sw.conds, 0);
   for (i = 0; inc && inc[i]; i++) {
      Cond *c = g_new0 (Cond, 1);
      c->key = g_strdup (inc[i]);
      g_ptr_array_add (sw.conds, c);
   }
   for (i = 0; exc && exc[i]; i++) {
      Cond *c = g_new0 (Cond, 1);
      c->key = g_strdup (exc[i]);
      c->exclude = TRUE;
      g_ptr_array_add (sw.conds, c);
   }
   gtk_editable_set_text (GTK_EDITABLE (sw.from_entry), from ? from : "");
   gtk_editable_set_text (GTK_EDITABLE (sw.to_entry), to ? to : "");
   g_strfreev (inc);
   g_strfreev (exc);
   g_free (from);
   g_free (to);
   g_key_file_unref (kf);
   rebuild_conds ();
   schedule_search ();
   sw.loading_saved = FALSE;
}


typedef struct {
   GtkWidget *window, *entry;
} NameAsk;


static void
save_as (const gchar *name)
{
   GKeyFile *kf = saved_load ();
   GPtrArray *inc = g_ptr_array_new (), *exc = g_ptr_array_new ();
   const gchar *from = gtk_editable_get_text (GTK_EDITABLE (sw.from_entry));
   const gchar *to = gtk_editable_get_text (GTK_EDITABLE (sw.to_entry));
   guint i;

   for (i = 0; i < sw.conds->len; i++) {
      Cond *c = g_ptr_array_index (sw.conds, i);
      g_ptr_array_add (c->exclude ? exc : inc, c->key);
   }
   g_key_file_remove_group (kf, name, NULL);
   g_key_file_set_string_list (kf, name, "include", (const gchar * const *) inc->pdata, inc->len);
   g_key_file_set_string_list (kf, name, "exclude", (const gchar * const *) exc->pdata, exc->len);
   if (from && *from) g_key_file_set_string (kf, name, "from", from);
   if (to && *to) g_key_file_set_string (kf, name, "to", to);
   saved_store (kf);
   g_key_file_unref (kf);
   g_ptr_array_unref (inc);
   g_ptr_array_unref (exc);
   saved_refresh (name);
}


static void
cb_name_ok (GtkWidget *widget, NameAsk *ask)
{
   gchar *name = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (ask->entry))));
   /* a group name of the key file: no brackets, no new lines */
   if (*name && !strpbrk (name, "[]\n\r")) {
      if (sw.window) save_as (name);
      gtk_window_destroy (GTK_WINDOW (ask->window));
   } else {
      gtk_widget_error_bell (ask->entry);
   }
   g_free (name);
}


static void
cb_name_cancel (GtkWidget *widget, NameAsk *ask)
{
   gtk_window_destroy (GTK_WINDOW (ask->window));
}


static void
cb_save (GtkButton *button, gpointer data)
{
   NameAsk *ask = g_new0 (NameAsk, 1);
   GtkWidget *box, *label, *buttons, *b;
   guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (sw.saved_drop));

   ask->window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (ask->window), _("Keep the Search"));
   gtk_window_set_transient_for (GTK_WINDOW (ask->window), GTK_WINDOW (sw.window));
   gtk_window_set_modal (GTK_WINDOW (ask->window), TRUE);
   g_object_set_data_full (G_OBJECT (ask->window), "ask", ask, g_free);
   box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (box, 12);
   gtk_widget_set_margin_end (box, 12);
   gtk_widget_set_margin_top (box, 12);
   gtk_widget_set_margin_bottom (box, 12);
   label = gtk_label_new (_("Name of the search (the same name: replaced):"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_box_append (GTK_BOX (box), label);
   ask->entry = gtk_entry_new ();
   gtk_editable_set_width_chars (GTK_EDITABLE (ask->entry), 30);
   if (pos > 0 && pos != GTK_INVALID_LIST_POSITION)
      gtk_editable_set_text (GTK_EDITABLE (ask->entry),
                             gtk_string_list_get_string (sw.saved_names, pos));
   g_signal_connect (ask->entry, "activate", G_CALLBACK (cb_name_ok), ask);
   gtk_box_append (GTK_BOX (box), ask->entry);
   buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   gtk_widget_set_halign (buttons, GTK_ALIGN_END);
   b = gtk_button_new_with_mnemonic (_("_Cancel"));
   g_signal_connect (b, "clicked", G_CALLBACK (cb_name_cancel), ask);
   gtk_box_append (GTK_BOX (buttons), b);
   b = gtk_button_new_with_mnemonic (_("_Keep"));
   gtk_widget_add_css_class (b, "suggested-action");
   g_signal_connect (b, "clicked", G_CALLBACK (cb_name_ok), ask);
   gtk_box_append (GTK_BOX (buttons), b);
   gtk_box_append (GTK_BOX (box), buttons);
   gtk_window_set_child (GTK_WINDOW (ask->window), box);
   gtk_window_present (GTK_WINDOW (ask->window));
}


static void
cb_delete (GtkButton *button, gpointer data)
{
   guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (sw.saved_drop));
   GKeyFile *kf;

   if (pos == 0 || pos == GTK_INVALID_LIST_POSITION) return;
   kf = saved_load ();
   g_key_file_remove_group (kf, gtk_string_list_get_string (sw.saved_names, pos), NULL);
   saved_store (kf);
   g_key_file_unref (kf);
   saved_refresh (NULL);
}


/******************************************************************************
 *  the window
 ******************************************************************************/

static void
cb_index_changed (GimvIndexChange what, gpointer data)
{
   /* new tags, names: the numbers again (not while it is not shown) */
   if (sw.window && (what & (GIMV_INDEX_CHANGED_TAGS | GIMV_INDEX_CHANGED_FACES))) {
      gboolean was = sw.loading_saved;
      sw.loading_saved = TRUE;   /* the same search: the one kept stays shown */
      schedule_search ();
      sw.loading_saved = was;
   }
}


static void
cb_tw_gone (gpointer data, GObject *where)
{
   sw.tw = NULL;
}


static void
cb_destroy (GtkWidget *widget, gpointer data)
{
   if (sw.timeout) g_source_remove (sw.timeout);
   sw.timeout = 0;
   if (sw.cancel) {
      g_cancellable_cancel (sw.cancel);
      g_clear_object (&sw.cancel);
   }
   if (sw.watch) gimv_index_remove_watch (sw.watch);
   sw.watch = 0;
   if (sw.tw) g_object_weak_unref (G_OBJECT (sw.tw), cb_tw_gone, NULL);
   sw.tw = NULL;
   set_result_tab (NULL);
   g_clear_pointer (&sw.conds, g_ptr_array_unref);
   g_clear_pointer (&sw.facets, g_ptr_array_unref);
   sw.window = NULL;
}


static GtkWidget *
labeled (const gchar *text, GtkWidget *child)
{
   GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   GtkWidget *label = gtk_label_new (text);
   gtk_box_append (GTK_BOX (box), label);
   gtk_box_append (GTK_BOX (box), child);
   return box;
}


static void
create_window (void)
{
   GtkWidget *vbox, *hbox, *scroll, *label, *b;
   const gchar *none[] = { NULL };

   sw.conds = g_ptr_array_new_with_free_func ((GDestroyNotify) cond_free);
   sw.window = gtk_window_new ();
   gtk_window_set_title (GTK_WINDOW (sw.window), _("Search Images"));
   gtk_window_set_default_size (GTK_WINDOW (sw.window), 460, 640);
   g_signal_connect (sw.window, "destroy", G_CALLBACK (cb_destroy), NULL);

   vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
   gtk_widget_set_margin_start (vbox, 10);
   gtk_widget_set_margin_end (vbox, 10);
   gtk_widget_set_margin_top (vbox, 10);
   gtk_widget_set_margin_bottom (vbox, 10);
   gtk_window_set_child (GTK_WINDOW (sw.window), vbox);

   /* the searches kept */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
   sw.saved_names = gtk_string_list_new (none);
   gtk_string_list_append (sw.saved_names, _("(searches kept)"));
   sw.saved_drop = gtk_drop_down_new (G_LIST_MODEL (sw.saved_names), NULL);
   gtk_widget_set_hexpand (sw.saved_drop, TRUE);
   gtk_widget_set_tooltip_text (sw.saved_drop, _("A search kept: its conditions again"));
   g_signal_connect (sw.saved_drop, "notify::selected", G_CALLBACK (cb_saved_selected), NULL);
   gtk_box_append (GTK_BOX (hbox), sw.saved_drop);
   b = gtk_button_new_with_mnemonic (_("_Keep..."));
   gtk_widget_set_tooltip_text (b, _("Keep these conditions under a name"));
   g_signal_connect (b, "clicked", G_CALLBACK (cb_save), NULL);
   gtk_box_append (GTK_BOX (hbox), b);
   sw.delete_button = gtk_button_new_from_icon_name ("user-trash-symbolic");
   gtk_widget_set_tooltip_text (sw.delete_button, _("Forget the search kept"));
   g_signal_connect (sw.delete_button, "clicked", G_CALLBACK (cb_delete), NULL);
   gtk_box_append (GTK_BOX (hbox), sw.delete_button);
   gtk_box_append (GTK_BOX (vbox), hbox);

   /* the conditions */
   label = gtk_label_new (_("Images with all of these:"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "heading");
   gtk_box_append (GTK_BOX (vbox), label);
   sw.none_label = gtk_label_new (_("(none yet: click a tag below)"));
   gtk_widget_add_css_class (sw.none_label, "dim-label");
   gtk_label_set_xalign (GTK_LABEL (sw.none_label), 0.0);
   gtk_box_append (GTK_BOX (vbox), sw.none_label);
   sw.conds_box = gtk_flow_box_new ();
   gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (sw.conds_box), GTK_SELECTION_NONE);
   gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (sw.conds_box), 20);
   gtk_box_append (GTK_BOX (vbox), sw.conds_box);

   /* the period */
   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
   sw.from_entry = gtk_entry_new ();
   gtk_entry_set_placeholder_text (GTK_ENTRY (sw.from_entry), "2019-05-01");
   gtk_editable_set_width_chars (GTK_EDITABLE (sw.from_entry), 11);
   g_signal_connect (sw.from_entry, "changed", G_CALLBACK (cb_date_changed), NULL);
   sw.to_entry = gtk_entry_new ();
   gtk_entry_set_placeholder_text (GTK_ENTRY (sw.to_entry), "2019-12");
   gtk_editable_set_width_chars (GTK_EDITABLE (sw.to_entry), 11);
   g_signal_connect (sw.to_entry, "changed", G_CALLBACK (cb_date_changed), NULL);
   gtk_box_append (GTK_BOX (hbox), labeled (_("Taken from"), sw.from_entry));
   gtk_box_append (GTK_BOX (hbox), labeled (_("to"), sw.to_entry));
   gtk_widget_set_tooltip_text (hbox, _("The date taken: a year (2019), a month (2019-05) or a "
                                        "day (2019-05-03); empty: any"));
   gtk_box_append (GTK_BOX (vbox), hbox);

   sw.count_label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (sw.count_label), 0.0);
   gtk_widget_add_css_class (sw.count_label, "dim-label");
   gtk_box_append (GTK_BOX (vbox), sw.count_label);

   gtk_box_append (GTK_BOX (vbox), gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

   /* the tags of the images found */
   label = gtk_label_new (_("Tags of these images (click to narrow down):"));
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   gtk_widget_add_css_class (label, "heading");
   gtk_box_append (GTK_BOX (vbox), label);
   sw.entry = gtk_search_entry_new ();
   gtk_search_entry_set_placeholder_text (GTK_SEARCH_ENTRY (sw.entry),
                                          _("A part of a tag or a name"));
   g_signal_connect (sw.entry, "search-changed", G_CALLBACK (cb_entry_changed), NULL);
   g_signal_connect (sw.entry, "activate", G_CALLBACK (cb_entry_activate), NULL);
   gtk_box_append (GTK_BOX (vbox), sw.entry);

   sw.tags_list = gtk_list_box_new ();
   gtk_list_box_set_selection_mode (GTK_LIST_BOX (sw.tags_list), GTK_SELECTION_NONE);
   gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (sw.tags_list), TRUE);
   g_signal_connect (sw.tags_list, "row-activated", G_CALLBACK (cb_row_activated), NULL);
   scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroll), TRUE);
   gtk_widget_set_vexpand (scroll, TRUE);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), sw.tags_list);
   gtk_box_append (GTK_BOX (vbox), scroll);

   saved_refresh (NULL);
   rebuild_conds ();
   sw.watch = gimv_index_add_watch (cb_index_changed, NULL);
}


void
gimv_index_search_open (GimvThumbWin *tw)
{
   if (!sw.window) create_window ();
   if (sw.tw != tw) {
      if (sw.tw) g_object_weak_unref (G_OBJECT (sw.tw), cb_tw_gone, NULL);
      sw.tw = tw;
      if (tw) g_object_weak_ref (G_OBJECT (tw), cb_tw_gone, NULL);
   }
   sw.loading_saved = TRUE;
   schedule_search ();
   sw.loading_saved = FALSE;
   gtk_window_present (GTK_WINDOW (sw.window));
   gtk_widget_grab_focus (sw.entry);
}
