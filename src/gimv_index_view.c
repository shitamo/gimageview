/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the "Date" and "Tag" tabs next to the directory
 * view (the side notebook of the thumbnail window).
 * Years, months and days, or tag names, come from the index database;
 * selecting one opens its images in a collection tab.
 *
 * The database may hold millions of files, so every query runs in a worker
 * thread with a connection of its own: the day counts once per refresh
 * (the whole tree is built from them), and the file list when a row is
 * opened.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "gimageview.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gio/gio.h>

#include "fileload.h"
#include "gtkutils.h"
#include "gimv_icon_stock.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_faces.h"
#include "gimv_index_faces_win.h"
#include "menu.h"
#include "prefs.h"
#include "gimv_index_view.h"
#include "gimv_thumb_view.h"
#include "gimv_thumb_win.h"

/* a collection tab made from the index shows file names, not full paths */
#define NAME_ONLY_KEY "gimv-name-only"

typedef enum {
   KIND_ROOT,
   KIND_YEAR,
   KIND_MONTH,
   KIND_DAY,
   KIND_TAG,
   KIND_GROUP,     /* the tags of a rule: "Folders", "Cameras", "Shape" */
   KIND_FACES,     /* people: a person, the guesses, a group of faces, ... */
   KIND_HEADER,    /* people: "Unnamed groups" */
} RowKind;

enum {
   COL_LABEL,
   COL_KIND,
   COL_KEY,        /* "2019", "2019-05", "2019-05-03", or the tag name;
                    * a tag put by a rule starts with "," (a tag never
                    * has a comma), a group of them is ",folder/" */
   COL_COLOR,      /* the tags put by rules have a color of their own */
   COL_TITLE,      /* people: the title of the result tab */
   N_COLS
};

typedef struct {
   gchar day[11];  /* "YYYY-MM-DD" */
   gint  count;
} DayCount;

typedef struct {
   gchar   *name;
   gint     count;
   gboolean auto_tag;   /* put by a rule (file_tags.auto) */
} TagCount;

#define AUTO_KEY_MARK  ','
#define AUTO_TAG_COLOR "#3584e4"

/* the kinds of the tags put by rules (gimv_indexer.c) */
static const struct {
   const gchar *prefix;
   const gchar *group;     /* the group row */
   const gchar *title;     /* the result tab: "%s" is the rest */
} auto_kinds[] = {
   { "folder/", N_("Folders"), N_("Folder: %s") },
   { "camera/", N_("Cameras"), N_("Camera: %s") },
   { "shape/",  N_("Shape"),   N_("Shape: %s") },
   /* not put by a rule, but kept in the index only too: the tags of the
    * people named in the image (person_tags) */
   { "person/", N_("Tags of People"), N_("People: %s") },
};


/* "shape/portrait" -> "Portrait", "folder/2019 Kyoto" -> "2019 Kyoto" */
static const gchar *
auto_tag_value (const gchar *name, gint *kind_ret)
{
   guint i;

   for (i = 0; i < G_N_ELEMENTS (auto_kinds); i++) {
      if (g_str_has_prefix (name, auto_kinds[i].prefix)) {
         const gchar *v = name + strlen (auto_kinds[i].prefix);
         if (kind_ret) *kind_ret = i;
         if (!strcmp (auto_kinds[i].prefix, "shape/")) {
            if (!strcmp (v, "portrait"))  return _("Portrait");
            if (!strcmp (v, "landscape")) return _("Landscape");
            if (!strcmp (v, "square"))    return _("Square shape");
         }
         return v;
      }
   }
   if (kind_ret) *kind_ret = -1;
   return name;
}


/* a row of the "People" tab */
typedef struct {
   gchar *key;     /* gimv_index_faces_key_where () */
   gchar *label;
   gchar *title;
   gint   depth;   /* 0: below the root, 1: below the row before at 0 */
   RowKind kind;
} PeopleRow;


static void
people_row_free (PeopleRow *r)
{
   g_free (r->key);
   g_free (r->label);
   g_free (r->title);
   g_free (r);
}


static void
add_people_row (GPtrArray *rows, gint depth, RowKind kind, gchar *key,
                gchar *label, gchar *title)
{
   PeopleRow *r = g_new0 (PeopleRow, 1);
   r->depth = depth;
   r->kind = kind;
   r->key = key;
   r->label = label;
   r->title = title;
   g_ptr_array_add (rows, r);
}


/* the title of a result tab of the tag (or the key of a tag row) */
static gchar *
tag_title (const gchar *key)
{
   gint kind;
   const gchar *v;

   if (key[0] != AUTO_KEY_MARK)
      return g_strdup_printf (_("%s (Tag)"), key);
   v = auto_tag_value (key + 1, &kind);
   if (kind < 0)
      return g_strdup_printf (_("%s (Tag)"), key + 1);
   return g_strdup_printf (_(auto_kinds[kind].title), v);
}

typedef struct {
   GimvIndexTabType type;
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
   gchar         *click_key;   /* row clicked with the left button */
   gchar         *shown_key;   /* what the result tab shows */

   GCancellable  *cancel;      /* cancelled when the view goes away */
   gboolean       counting;    /* a day count query is running */
   gboolean       count_again; /* the database changed meanwhile */
   gboolean       listing;     /* a file list query is running */
   gboolean       filled;      /* refilled once: what is open is kept */
   GtkCellRenderer    *render;
   GtkTreeViewColumn  *col;
   GSimpleActionGroup *actions;
   gchar         *editing_key; /* the tag being renamed in the tree */
   guint          rename_id;
   gboolean       click_open;  /* open_key () from a click on the tree */
   GtkWidget     *search_bar;  /* Ctrl+F: a row by its name */
   GtkWidget     *search_entry;
} IndexView;


static gboolean key_is_manual_tag (IndexView *iv, const gchar *key);

typedef struct {
   GimvIndexTabType type;
   gchar   *key;
   gchar   *title;     /* people */
   gboolean new_tab;
   gboolean keep_focus; /* opened by a click on the tree: the keys stay there
                         * (F2, Ctrl+C, the arrows) */
} OpenRequest;


/******************************************************************************
 *  queries (worker threads)
 ******************************************************************************/

static sqlite3 *
open_db (void)
{
   gchar *path = gimv_index_db_default_path ();
   sqlite3 *db = NULL;

   gimv_index_db_open (path, FALSE, &db, NULL);
   g_free (path);
   return db;
}


static void
thread_count_days (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   sqlite3 *db = open_db ();
   sqlite3_stmt *st;
   GArray *days = g_array_new (FALSE, TRUE, sizeof (DayCount));

   /* the index on files(date) covers this query */
   if (db && sqlite3_prepare_v2 (db,
          "SELECT substr(date, 1, 10) AS d, count(*) FROM files "
          "WHERE date IS NOT NULL GROUP BY d ORDER BY d DESC",
          -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *d = (const gchar *) sqlite3_column_text (st, 0);
         DayCount dc;
         if (!d || strlen (d) != 10) continue;
         memcpy (dc.day, d, 11);
         dc.count = sqlite3_column_int (st, 1);
         g_array_append_val (days, dc);
      }
      sqlite3_finalize (st);
   }
   if (db) sqlite3_close (db);
   g_task_return_pointer (task, days, (GDestroyNotify) g_array_unref);
}


static void
tag_count_free (TagCount *tc)
{
   g_free (tc->name);
   g_free (tc);
}


static void
thread_count_tags (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   sqlite3 *db = open_db ();
   sqlite3_stmt *st;
   GPtrArray *tags = g_ptr_array_new_with_free_func ((GDestroyNotify) tag_count_free);

   /* the tags put by a person and those put by rules apart (the same
    * name can be both; it is not usual: the rules make "folder/...") */
   if (db && sqlite3_prepare_v2 (db,
          "SELECT t.name, ft.auto, count(*) FROM file_tags ft "
          "JOIN tags t ON t.id = ft.tag_id GROUP BY t.id, ft.auto ORDER BY t.name",
          -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW) {
         TagCount *tc = g_new0 (TagCount, 1);
         tc->name     = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
         tc->auto_tag = sqlite3_column_int (st, 1) != 0;
         tc->count    = sqlite3_column_int (st, 2);
         g_ptr_array_add (tags, tc);
      }
      sqlite3_finalize (st);
   }
   /* the tags of the people: the images any of them is named in */
   if (db && sqlite3_prepare_v2 (db,
          "SELECT pt.tag, count(DISTINCT fa.file_id) FROM person_tags pt "
          "JOIN faces fa ON fa.person_id = pt.person_id GROUP BY pt.tag ORDER BY pt.tag",
          -1, &st, NULL) == SQLITE_OK)
   {
      while (sqlite3_step (st) == SQLITE_ROW) {
         TagCount *tc = g_new0 (TagCount, 1);
         tc->name     = g_strdup_printf ("person/%s", (const gchar *) sqlite3_column_text (st, 0));
         tc->auto_tag = TRUE;
         tc->count    = sqlite3_column_int (st, 1);
         g_ptr_array_add (tags, tc);
      }
      sqlite3_finalize (st);
   }
   if (db) sqlite3_close (db);
   g_task_return_pointer (task, tags, (GDestroyNotify) g_ptr_array_unref);
}


static gint64
query_count (sqlite3 *db, const gchar *sql)
{
   sqlite3_stmt *st;
   gint64 n = 0;

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
   if (sqlite3_step (st) == SQLITE_ROW) n = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   return n;
}


/* the named people (and the faces that look like them), the groups of
 * unnamed faces that look alike (the biggest first), the rest */
static void
thread_count_people (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   sqlite3 *db = open_db ();
   sqlite3_stmt *st;
   GPtrArray *rows = g_ptr_array_new_with_free_func ((GDestroyNotify) people_row_free);
   gint64 n;

   if (!db) {
      g_task_return_pointer (task, rows, (GDestroyNotify) g_ptr_array_unref);
      return;
   }

   /* the people: first by their tags (a person with two tags is under
    * both), then those without a tag */
   {
      typedef struct { gint64 id; gchar *name; gint named, guessed; gboolean tagged; } Person;
      GArray *people = g_array_new (FALSE, TRUE, sizeof (Person));
      GHashTable *by_id = g_hash_table_new (g_int64_hash, g_int64_equal);
      guint i;

      if (sqlite3_prepare_v2 (db,
             "SELECT p.id, p.name, "
             "  (SELECT count(*) FROM faces WHERE person_id = p.id), "
             "  (SELECT count(*) FROM faces WHERE guess = p.id AND person_id IS NULL "
             "   AND ignored = 0) "
             "FROM persons p ORDER BY p.name", -1, &st, NULL) == SQLITE_OK)
      {
         while (sqlite3_step (st) == SQLITE_ROW) {
            Person pr = { 0 };
            pr.id = sqlite3_column_int64 (st, 0);
            pr.name = g_strdup ((const gchar *) sqlite3_column_text (st, 1));
            pr.named = sqlite3_column_int (st, 2);
            pr.guessed = sqlite3_column_int (st, 3);
            if (!pr.named && !pr.guessed) {
               g_free (pr.name);
               continue;
            }
            g_array_append_val (people, pr);
         }
         sqlite3_finalize (st);
      }
      for (i = 0; i < people->len; i++) {
         Person *pr = &g_array_index (people, Person, i);
         g_hash_table_insert (by_id, &pr->id, pr);
      }

#define ADD_PERSON(pr, depth) G_STMT_START { \
         add_people_row (rows, (depth), KIND_FACES, \
                         g_strdup_printf ("p:%" G_GINT64_FORMAT, (pr)->id), \
                         g_strdup_printf ("%s (%'d)", (pr)->name, (pr)->named), \
                         g_strdup ((pr)->name)); \
         if ((pr)->guessed) \
            add_people_row (rows, (depth) + 1, KIND_FACES, \
                            g_strdup_printf ("g:%" G_GINT64_FORMAT, (pr)->id), \
                            g_strdup_printf (_("Maybe (%'d)"), (pr)->guessed), \
                            g_strdup_printf (_("%s?"), (pr)->name)); \
      } G_STMT_END

      if (sqlite3_prepare_v2 (db,
             "SELECT pt.tag, pt.person_id FROM person_tags pt "
             "JOIN persons p ON p.id = pt.person_id ORDER BY pt.tag, p.name",
             -1, &st, NULL) == SQLITE_OK)
      {
         gchar *cur = NULL;
         GPtrArray *members = g_ptr_array_new ();

         for (;;) {
            gboolean more = sqlite3_step (st) == SQLITE_ROW;
            const gchar *tag = more ? (const gchar *) sqlite3_column_text (st, 0) : NULL;
            gint64 pid = more ? sqlite3_column_int64 (st, 1) : 0;
            Person *pr = more ? g_hash_table_lookup (by_id, &pid) : NULL;

            if (cur && (!more || g_strcmp0 (tag, cur))) {
               /* the tag before: its row and its people */
               if (members->len) {
                  guint j;
                  add_people_row (rows, 0, KIND_FACES, g_strdup_printf ("t:%s", cur),
                                  g_strdup_printf (_("%s (%'d people)"), cur, members->len),
                                  g_strdup (cur));
                  for (j = 0; j < members->len; j++)
                     ADD_PERSON ((Person *) g_ptr_array_index (members, j), 1);
               }
               g_ptr_array_set_size (members, 0);
               g_clear_pointer (&cur, g_free);
            }
            if (!more) break;
            if (!cur) cur = g_strdup (tag);
            if (pr) {
               g_ptr_array_add (members, pr);
               pr->tagged = TRUE;
            }
         }
         sqlite3_finalize (st);
         g_ptr_array_unref (members);
      }
      for (i = 0; i < people->len; i++) {
         Person *pr = &g_array_index (people, Person, i);
         if (!pr->tagged) ADD_PERSON (pr, 0);
      }
#undef ADD_PERSON
      for (i = 0; i < people->len; i++)
         g_free (g_array_index (people, Person, i).name);
      g_hash_table_unref (by_id);
      g_array_unref (people);
   }

   n = query_count (db, "SELECT count(DISTINCT cluster) FROM faces WHERE cluster IS NOT NULL "
                        "AND person_id IS NULL AND guess IS NULL AND ignored = 0");
   if (sqlite3_prepare_v2 (db,
          "SELECT cluster, count(*) AS n FROM faces WHERE cluster IS NOT NULL "
          "AND person_id IS NULL AND guess IS NULL AND ignored = 0 "
          "GROUP BY cluster ORDER BY n DESC, cluster", -1, &st, NULL) == SQLITE_OK)
   {
      gboolean header = FALSE;
      while (sqlite3_step (st) == SQLITE_ROW) {
         gint c = sqlite3_column_int (st, 0), count = sqlite3_column_int (st, 1);
         if (!header) {
            add_people_row (rows, 0, KIND_HEADER, g_strdup ("c:"),
                            g_strdup_printf (_("Unnamed groups (%'d)"), (gint) n), NULL);
            header = TRUE;
         }
         add_people_row (rows, 1, KIND_FACES, g_strdup_printf ("c:%d", c),
                         g_strdup_printf (_("Group %d (%'d)"), c, count),
                         g_strdup_printf (_("Group %d"), c));
      }
      sqlite3_finalize (st);
   }

   n = query_count (db, "SELECT count(*) FROM faces WHERE person_id IS NULL AND guess IS NULL "
                        "AND cluster IS NULL AND ignored = 0");
   if (n)
      add_people_row (rows, 0, KIND_FACES, g_strdup ("u"),
                      g_strdup_printf (_("Other faces (%'d)"), (gint) n),
                      g_strdup (_("Other faces")));
   n = query_count (db, "SELECT count(*) FROM faces WHERE ignored = 1");
   if (n)
      add_people_row (rows, 0, KIND_FACES, g_strdup ("i"),
                      g_strdup_printf (_("Ignored (%'d)"), (gint) n),
                      g_strdup (_("Ignored faces")));

   sqlite3_close (db);
   g_task_return_pointer (task, rows, (GDestroyNotify) g_ptr_array_unref);
}


/* the result of a file list query */
typedef struct {
   GList      *paths;
   GHashTable *stats;   /* path (in paths) -> struct stat from the index */
} FileList;


static void
file_list_free (FileList *fl)
{
   if (fl->stats) g_hash_table_unref (fl->stats);
   g_list_free_full (fl->paths, g_free);
   g_free (fl);
}


/* "date" is "YYYY-MM-DD HH:MM:SS": every date starting with key sorts in
 * [key, key + "~") */
static void
thread_list_files (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   OpenRequest *req = data;
   const gchar *key = req->key;
   sqlite3 *db = open_db ();
   sqlite3_stmt *st;
   GList *list = NULL;
   const gchar *sql;

   FileList *fl = g_new0 (FileList, 1);

   /* size and mtime too: the tab is filled without looking at every file
    * on the disk (tens of thousands of them on a HDD) */
   gchar *people_sql = NULL;

   if (req->type == GIMV_INDEX_TAB_PERSON) {
      gchar *where = gimv_index_faces_key_where (key);
      if (where)
         people_sql = g_strdup_printf (
            "SELECT d.path, f.name, f.size, f.mtime FROM files f "
            "JOIN dirs d ON d.id = f.dir_id WHERE f.id IN "
            "(SELECT fa.file_id FROM faces fa WHERE %s) ORDER BY f.date, f.name", where);
      g_free (where);
      sql = people_sql ? people_sql : "SELECT 1 WHERE 0";
   } else if (req->type == GIMV_INDEX_TAB_TAG && g_str_has_prefix (key, ",person/")) {
      /* a tag of people: the images they are named in (?1, ?2 as for tags) */
      sql = "SELECT d.path, f.name, f.size, f.mtime, f.date FROM files f "
            "JOIN dirs d ON d.id = f.dir_id WHERE f.id IN (SELECT fa.file_id FROM faces fa "
            "JOIN person_tags pt ON pt.person_id = fa.person_id "
            "WHERE 'person/' || pt.tag = ?1 AND ?2 = 1) ORDER BY f.date, f.name";
   } else if (req->type == GIMV_INDEX_TAB_TAG)
      sql = "SELECT d.path, f.name, f.size, f.mtime, f.date FROM file_tags ft "
            "JOIN tags t ON t.id = ft.tag_id JOIN files f ON f.id = ft.file_id "
            "JOIN dirs d ON d.id = f.dir_id WHERE t.name = ?1 AND ft.auto = ?2 "
            "ORDER BY f.date, f.name";
   else
      sql = "SELECT d.path, f.name, f.size, f.mtime FROM files f JOIN dirs d ON d.id = f.dir_id "
            "WHERE f.date >= ?1 AND f.date < ?1 || '~' ORDER BY f.date, f.name";

   fl->stats = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
   if (db && sqlite3_prepare_v2 (db, sql, -1, &st, NULL) == SQLITE_OK)
   {
      if (req->type == GIMV_INDEX_TAB_TAG) {
         gboolean is_auto = key[0] == AUTO_KEY_MARK;
         sqlite3_bind_text (st, 1, is_auto ? key + 1 : key, -1, SQLITE_TRANSIENT);
         sqlite3_bind_int  (st, 2, is_auto);
      } else if (req->type == GIMV_INDEX_TAB_DATE) {
         sqlite3_bind_text (st, 1, key, -1, SQLITE_TRANSIENT);
      }
      while (sqlite3_step (st) == SQLITE_ROW) {
         const gchar *dir  = (const gchar *) sqlite3_column_text (st, 0);
         const gchar *name = (const gchar *) sqlite3_column_text (st, 1);
         gchar *path = g_build_filename (dir, name, NULL);
         struct stat *sb = g_new0 (struct stat, 1);

         sb->st_mode  = S_IFREG | 0644;
         sb->st_size  = sqlite3_column_int64 (st, 2);
         sb->st_mtime = sqlite3_column_int64 (st, 3);
         sb->st_ctime = sb->st_mtime;
         sb->st_atime = sb->st_mtime;
         sb->st_uid   = getuid ();
         sb->st_gid   = getgid ();
         list = g_list_prepend (list, path);
         g_hash_table_insert (fl->stats, path, sb);
      }
      sqlite3_finalize (st);
   }
   if (db) sqlite3_close (db);
   g_free (people_sql);
   fl->paths = g_list_reverse (list);
   g_task_return_pointer (task, fl, (GDestroyNotify) file_list_free);
}


/******************************************************************************
 *  tree
 ******************************************************************************/

static void
append_row (IndexView *iv, GtkTreeIter *iter, GtkTreeIter *parent,
            RowKind kind, const gchar *key, gint len, gint count)
{
   gchar *k = g_strndup (key, len);
   /* the last part of the key: "2019", "05", "03" */
   gchar *label = g_strdup_printf ("%s (%'d)", k + (len == 4 ? 0 : len - 2), count);

   gtk_tree_store_append (iv->store, iter, parent);
   gtk_tree_store_set (iv->store, iter,
                       COL_LABEL, label, COL_KIND, kind, COL_KEY, k, -1);
   g_free (label);
   g_free (k);
}


static void
set_count (IndexView *iv, GtkTreeIter *iter, gint len, gint count)
{
   gchar *k = NULL, *label;

   gtk_tree_model_get (GTK_TREE_MODEL (iv->store), iter, COL_KEY, &k, -1);
   label = g_strdup_printf ("%s (%'d)", k + (len == 4 ? 0 : len - 2), count);
   gtk_tree_store_set (iv->store, iter, COL_LABEL, label, -1);
   g_free (label);
   g_free (k);
}


/* days come newest first; years are shown newest first, months and days
 * inside a year oldest first */
static void
build_tree (IndexView *iv, GtkTreeIter *root, GArray *days)
{
   GtkTreeIter year, month, day;
   gchar cur_year[5] = "", cur_month[8] = "";
   gint year_count = 0, month_count = 0;
   gint i;

   for (i = 0; i < (gint) days->len; i++) {
      DayCount *dc = &g_array_index (days, DayCount, i);

      if (strncmp (dc->day, cur_year, 4)) {
         if (*cur_month) set_count (iv, &month, 7, month_count);
         if (*cur_year) set_count (iv, &year, 4, year_count);
         append_row (iv, &year, root, KIND_YEAR, dc->day, 4, 0);
         memcpy (cur_year, dc->day, 4);
         year_count = 0;
         *cur_month = '\0';
      }
      if (strncmp (dc->day, cur_month, 7)) {
         if (*cur_month) set_count (iv, &month, 7, month_count);
         /* months oldest first: put each one before the previous */
         gtk_tree_store_prepend (iv->store, &month, &year);
         {
            gchar *k = g_strndup (dc->day, 7);
            gtk_tree_store_set (iv->store, &month, COL_KIND, KIND_MONTH, COL_KEY, k, -1);
            g_free (k);
         }
         memcpy (cur_month, dc->day, 7);
         month_count = 0;
      }
      gtk_tree_store_prepend (iv->store, &day, &month);
      gtk_tree_store_set (iv->store, &day, COL_KIND, KIND_DAY, COL_KEY, dc->day, -1);
      set_count (iv, &day, 10, dc->count);

      year_count  += dc->count;
      month_count += dc->count;
   }
   if (*cur_month) set_count (iv, &month, 7, month_count);
   if (*cur_year)  set_count (iv, &year, 4, year_count);
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
      if (k && !strcmp (k, key)) {
         g_free (k);
         *found = iter;
         return TRUE;
      }
      /* dates: "2019-05" is below "2019"; tags: ",folder/x" below ",folder/" */
      if (k && g_str_has_prefix (key, k)
          && (key[strlen (k)] == '-' || (k[0] == AUTO_KEY_MARK && g_str_has_suffix (k, "/"))))
      {
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


/* the tags put by a person at the top, then a group for each rule */
static void
build_tag_list (IndexView *iv, GtkTreeIter *root, GPtrArray *tags)
{
   GtkTreeIter groups[G_N_ELEMENTS (auto_kinds)];
   gboolean has_group[G_N_ELEMENTS (auto_kinds)] = { FALSE };
   guint i, k;

   for (i = 0; i < tags->len; i++) {
      TagCount *tc = g_ptr_array_index (tags, i);
      GtkTreeIter iter;
      gchar *label;

      if (tc->auto_tag) continue;
      label = g_strdup_printf ("%s (%'d)", tc->name, tc->count);
      gtk_tree_store_append (iv->store, &iter, root);
      gtk_tree_store_set (iv->store, &iter, COL_LABEL, label, COL_KIND, KIND_TAG,
                          COL_KEY, tc->name, -1);
      g_free (label);
   }

   for (k = 0; k < G_N_ELEMENTS (auto_kinds); k++) {
      for (i = 0; i < tags->len; i++) {
         TagCount *tc = g_ptr_array_index (tags, i);
         GtkTreeIter iter;
         gchar *label, *key;
         gint kind;
         const gchar *v;

         if (!tc->auto_tag) continue;
         v = auto_tag_value (tc->name, &kind);
         if (kind != (gint) k) continue;

         if (!has_group[k]) {
            gchar *gkey = g_strdup_printf ("%c%s", AUTO_KEY_MARK, auto_kinds[k].prefix);
            gtk_tree_store_append (iv->store, &groups[k], root);
            gtk_tree_store_set (iv->store, &groups[k], COL_LABEL, _(auto_kinds[k].group),
                                COL_KIND, KIND_GROUP, COL_KEY, gkey,
                                COL_COLOR, AUTO_TAG_COLOR, -1);
            g_free (gkey);
            has_group[k] = TRUE;
         }
         label = g_strdup_printf ("%s (%'d)", v, tc->count);
         key = g_strdup_printf ("%c%s", AUTO_KEY_MARK, tc->name);
         gtk_tree_store_append (iv->store, &iter, &groups[k]);
         gtk_tree_store_set (iv->store, &iter, COL_LABEL, label, COL_KIND, KIND_TAG,
                             COL_KEY, key, COL_COLOR, AUTO_TAG_COLOR, -1);
         g_free (key);
         g_free (label);
      }
   }
}


static void
build_people_list (IndexView *iv, GtkTreeIter *root, GPtrArray *rows)
{
   /* the last row at each depth: the parent of the rows below it */
   GtkTreeIter at[4], iter;
   gint have = -1;
   guint i;

   for (i = 0; i < rows->len; i++) {
      PeopleRow *r = g_ptr_array_index (rows, i);
      gint depth = MIN (r->depth, have + 1);
      GtkTreeIter *parent = depth > 0 ? &at[depth - 1] : root;

      depth = MIN (depth, (gint) G_N_ELEMENTS (at) - 1);
      gtk_tree_store_append (iv->store, &iter, parent);
      gtk_tree_store_set (iv->store, &iter, COL_LABEL, r->label, COL_KIND, r->kind,
                          COL_KEY, r->key, COL_TITLE, r->title,
                          COL_COLOR, r->kind == KIND_FACES && r->key[0] != 'p'
                                     && r->key[0] != 't' ? AUTO_TAG_COLOR : NULL, -1);
      at[depth] = iter;
      have = depth;
   }
}


/* anywhere in the tree (people: the keys say nothing of the place) */
static gboolean
find_key_any (GtkTreeModel *model, GtkTreeIter *parent, const gchar *key,
              GtkTreeIter *found)
{
   GtkTreeIter iter;

   if (!gtk_tree_model_iter_children (model, &iter, parent))
      return FALSE;
   do {
      gchar *k = NULL;
      gboolean same;
      gtk_tree_model_get (model, &iter, COL_KEY, &k, -1);
      same = k && !strcmp (k, key);
      g_free (k);
      if (same) {
         *found = iter;
         return TRUE;
      }
      if (find_key_any (model, &iter, key, found)) return TRUE;
   } while (gtk_tree_model_iter_next (model, &iter));
   return FALSE;
}


/* rebuild the tree from new counts and open again what was open;
 * result: a GArray of DayCount or a GPtrArray of TagCount */
static void
refill (IndexView *iv, gpointer result)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter root, child;
   GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
   GtkTreePath *path;
   gchar *selected = NULL;
   GtkTreeSelection *sel;
   guint i;

   if (!gtk_tree_model_get_iter_first (model, &root)) return;

   gtk_tree_view_map_expanded_rows (GTK_TREE_VIEW (iv->tree), collect_expanded, keys);
   sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   if (gtk_tree_selection_get_selected (sel, NULL, &child))
      gtk_tree_model_get (model, &child, COL_KEY, &selected, -1);

   iv->filling = TRUE;
   while (gtk_tree_model_iter_children (model, &child, &root))
      gtk_tree_store_remove (iv->store, &child);
   if (iv->type == GIMV_INDEX_TAB_TAG)
      build_tag_list (iv, &root, result);
   else if (iv->type == GIMV_INDEX_TAB_PERSON)
      build_people_list (iv, &root, result);
   else
      build_tree (iv, &root, result);

   /* the root is always open */
   path = gtk_tree_model_get_path (model, &root);
   gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
   gtk_tree_path_free (path);
   /* shorter keys first: a month row is shown only when its year is open */
   for (i = 0; i < keys->len; i++) {
      if (iv->type == GIMV_INDEX_TAB_PERSON
          ? find_key_any (model, &root, g_ptr_array_index (keys, i), &child)
          : find_key (model, &root, g_ptr_array_index (keys, i), &child)) {
         path = gtk_tree_model_get_path (model, &child);
         gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
         gtk_tree_path_free (path);
      }
   }
   /* people: the groups and the guesses open the first time */
   if (!iv->filled && iv->type == GIMV_INDEX_TAB_PERSON
       && gtk_tree_model_iter_children (model, &child, &root)) {
      do {
         if (!gtk_tree_model_iter_has_child (model, &child)) continue;
         path = gtk_tree_model_get_path (model, &child);
         gtk_tree_view_expand_row (GTK_TREE_VIEW (iv->tree), path, FALSE);
         gtk_tree_path_free (path);
      } while (gtk_tree_model_iter_next (model, &child));
   }
   iv->filled = TRUE;
   if (selected && (iv->type == GIMV_INDEX_TAB_PERSON
                    ? find_key_any (model, &root, selected, &child)
                    : find_key (model, &root, selected, &child)))
      gtk_tree_selection_select_iter (sel, &child);
   iv->filling = FALSE;

   g_free (selected);
   g_ptr_array_unref (keys);
}


static void start_count (IndexView *iv);

static void
cb_counted (GObject *source, GAsyncResult *res, gpointer data)
{
   GTask *task = G_TASK (res);
   IndexView *iv = data;
   gpointer result;

   /* the view is gone */
   if (g_cancellable_is_cancelled (g_task_get_cancellable (task))) return;

   result = g_task_propagate_pointer (task, NULL);
   iv->counting = FALSE;
   if (result) {
      refill (iv, result);
      if (iv->type != GIMV_INDEX_TAB_DATE)
         g_ptr_array_unref (result);
      else
         g_array_unref (result);
   }
   if (iv->count_again) start_count (iv);
}


static void
start_count (IndexView *iv)
{
   GTask *task;

   /* not while a tag is renamed in the tree: the row would go */
   if (iv->counting || iv->editing_key) {
      iv->count_again = TRUE;
      return;
   }
   iv->counting = TRUE;
   iv->count_again = FALSE;
   task = g_task_new (NULL, iv->cancel, cb_counted, iv);
   g_task_run_in_thread (task, iv->type == GIMV_INDEX_TAB_TAG ? thread_count_tags
                               : iv->type == GIMV_INDEX_TAB_PERSON ? thread_count_people
                               : thread_count_days);
   g_object_unref (task);
}


static void
cb_index_changed (GimvIndexChange what, gpointer data)
{
   IndexView *iv = data;

   /* the tag tab has the tags of people too (faces) */
   if (what & (iv->type == GIMV_INDEX_TAB_TAG ? GIMV_INDEX_CHANGED_TAGS | GIMV_INDEX_CHANGED_FACES
               : iv->type == GIMV_INDEX_TAB_PERSON ? GIMV_INDEX_CHANGED_FACES
               : GIMV_INDEX_CHANGED_DATES))
      start_count (iv);
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


static void
open_request_free (OpenRequest *req)
{
   g_free (req->key);
   g_free (req->title);
   g_free (req);
}


static void
open_files (IndexView *iv, OpenRequest *req, FileList *fl)
{
   GList *filelist = fl->paths;
   FilesLoader *files;
   GimvThumbView *tv;
   gchar *title;

   if (!filelist || files_loader_query_loading ()) {
      file_list_free (fl);
      return;
   }

   files = files_loader_new ();
   files->filelist = filelist;
   files->known_stats = fl->stats;
   /* the loader owns them now */
   fl->paths = NULL;
   fl->stats = NULL;
   file_list_free (fl);
   files->status = END;
   files->thumb_load_type = LOAD_CACHE;

   tv = req->new_tab ? NULL : iv->tv;
   if (!tv || tv->tw != iv->tw || tv->progress) {
      tv = gimv_thumb_win_create_new_tab (iv->tw);
      /* a tab opened on request stays as it is */
      if (!req->new_tab || !iv->tv)
         set_result_tab (iv, tv);
   }
   g_object_set_data (G_OBJECT (tv), NAME_ONLY_KEY, GINT_TO_POINTER (TRUE));
   /* tens of thousands of images: keep only the thumbnails near the
    * visible part */
   gimv_thumb_view_set_windowed (tv, TRUE);
   show_page (iv->tw, tv->container);

   if (tv == iv->tv) {
      g_free (iv->shown_key);
      iv->shown_key = g_strdup (req->key);
   }

   if (req->type == GIMV_INDEX_TAB_PERSON)
      title = g_strdup_printf (_("%s (People)"), req->title ? req->title : req->key);
   else if (req->type == GIMV_INDEX_TAB_TAG)
      title = tag_title (req->key);
   else
      title = g_strdup_printf (_("%s (Date)"), req->key);
   gimv_thumb_view_reset_tab_label (tv, title);
   g_free (title);

   open_image_files_in_thumbnail_view (files, iv->tw, tv);
   files_loader_delete (files);
   if (req->keep_focus)
      gtk_widget_grab_focus (iv->tree);
}


static void
cb_listed (GObject *source, GAsyncResult *res, gpointer data)
{
   GTask *task = G_TASK (res);
   IndexView *iv = data;
   FileList *fl;

   if (g_cancellable_is_cancelled (g_task_get_cancellable (task))) return;

   iv->listing = FALSE;
   fl = g_task_propagate_pointer (task, NULL);
   if (fl) open_files (iv, g_task_get_task_data (task), fl);
}


/* people: the title of the row of the key */
static gchar *
row_title (IndexView *iv, const gchar *key)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter root, iter;
   gchar *title = NULL;

   if (iv->type != GIMV_INDEX_TAB_PERSON) return NULL;
   if (gtk_tree_model_get_iter_first (model, &root)
       && find_key_any (model, &root, key, &iter))
      gtk_tree_model_get (model, &iter, COL_TITLE, &title, -1);
   return title;
}


/* new_tab FALSE: replace what the result tab shows (the usual way) */
static void
open_key (IndexView *iv, const gchar *key, gboolean new_tab)
{
   OpenRequest *req;
   GTask *task;

   /* the faces window, when open, shows the faces of the row too */
   if (iv->type == GIMV_INDEX_TAB_PERSON && gimv_index_faces_win_is_open ()) {
      gchar *title = row_title (iv, key);
      gimv_index_faces_win_show (iv->tw, key, title);
      g_free (title);
   }

   /* one at a time: clicks while the list is read are dropped */
   if (iv->listing || files_loader_query_loading ()) return;

   /* already shown (the second click of a double click, Enter) */
   if (!new_tab && iv->tv && iv->shown_key && !strcmp (iv->shown_key, key)) {
      show_page (iv->tw, iv->tv->container);
      return;
   }

   req = g_new0 (OpenRequest, 1);
   req->type = iv->type;
   req->key = g_strdup (key);
   req->title = row_title (iv, key);
   req->new_tab = new_tab;
   req->keep_focus = iv->click_open;

   iv->listing = TRUE;
   task = g_task_new (NULL, iv->cancel, cb_listed, iv);
   g_task_set_task_data (task, req, (GDestroyNotify) open_request_free);
   g_task_run_in_thread (task, thread_list_files);
   g_object_unref (task);
}


static gboolean
row_key (IndexView *iv, GtkTreeIter *iter, gchar **key)
{
   gint kind = 0;

   *key = NULL;
   gtk_tree_model_get (GTK_TREE_MODEL (iv->store), iter,
                       COL_KIND, &kind, COL_KEY, key, -1);
   if (*key && (kind == KIND_YEAR || kind == KIND_MONTH || kind == KIND_DAY
                || kind == KIND_TAG || kind == KIND_FACES))
      return TRUE;
   g_free (*key);
   *key = NULL;
   return FALSE;
}


static void
cb_row_activated (GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *col,
                  IndexView *iv)
{
   GtkTreeIter iter;
   gchar *key;

   if (gtk_tree_model_get_iter (GTK_TREE_MODEL (iv->store), &iter, path)
       && row_key (iv, &iter, &key))
   {
      open_key (iv, key, FALSE);
      g_free (key);
   }
}


static gboolean
idle_open_key (gpointer data)
{
   IndexView *iv = data;

   iv->open_id = 0;
   if (iv->click_key) {
      iv->click_open = TRUE;
      open_key (iv, iv->click_key, FALSE);
      iv->click_open = FALSE;
      g_clear_pointer (&iv->click_key, g_free);
   }
   return G_SOURCE_REMOVE;
}


/* right button: a menu for the row under the pointer, middle button: open
 * it in a new tab.  Neither changes the selection. */
/* on_text: set to whether the point is on the row text, not on the
 * expander arrow or the indentation before it */
static gchar *
key_at (IndexView *iv, gdouble x, gdouble y, gboolean *on_text)
{
   GtkTreePath *path = NULL;
   GtkTreeViewColumn *col = NULL;
   GtkTreeIter iter;
   gint bx, by;
   gchar *key = NULL;

   gtk_tree_view_convert_widget_to_bin_window_coords (GTK_TREE_VIEW (iv->tree),
                                                      x, y, &bx, &by);
   if (!gtk_tree_view_get_path_at_pos (GTK_TREE_VIEW (iv->tree), bx, by,
                                       &path, &col, NULL, NULL))
      return NULL;
   if (on_text) {
      GdkRectangle cell;
      gtk_tree_view_get_cell_area (GTK_TREE_VIEW (iv->tree), path, col, &cell);
      *on_text = bx >= cell.x;
   }
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
cb_action_faces (GSimpleAction *action, GVariant *param, gpointer data)
{
   IndexView *iv = data;
   gchar *title;

   if (!iv->menu_key) return;
   title = row_title (iv, iv->menu_key);
   gimv_index_faces_win_show (iv->tw, iv->menu_key, title);
   g_free (title);
}


static void
cb_tree_pressed (GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                 IndexView *iv)
{
   guint button = gtk_gesture_single_get_current_button (GTK_GESTURE_SINGLE (gesture));
   gboolean on_text = FALSE;
   gchar *key;

   if (button == GDK_BUTTON_PRIMARY) {
      /* a click on the text opens it; the arrow only opens the row of the
       * tree, and moving with the keys opens nothing (Enter does).  The
       * tree still handles the click itself (selection, arrow). */
      if (n_press != 1) return;
      key = key_at (iv, x, y, &on_text);
      if (!key) return;
      if (!on_text) {
         g_free (key);
         return;
      }
      g_free (iv->click_key);
      iv->click_key = key;
      if (!iv->open_id)
         iv->open_id = g_idle_add (idle_open_key, iv);
      return;
   }
   if (button != GDK_BUTTON_MIDDLE && button != GDK_BUTTON_SECONDARY) return;
   key = key_at (iv, x, y, NULL);
   if (!key) return;
   gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);

   g_free (iv->menu_key);
   iv->menu_key = key;

   if (button == GDK_BUTTON_MIDDLE) {
      open_key (iv, key, TRUE);
   } else if (button == GDK_BUTTON_SECONDARY) {
      GdkRectangle rect = { (gint) x, (gint) y, 1, 1 };
      graphene_point_t pt = GRAPHENE_POINT_INIT (x, y), out;
      /* on the scrolled window: a tree view does not take a popover as a
       * child well (a critical about its CSS nodes) */
      if (gtk_widget_compute_point (iv->tree, iv->scroll, &pt, &out)) {
         rect.x = (gint) out.x;
         rect.y = (gint) out.y;
      }
      if (!iv->popover) {
         GMenu *menu = g_menu_new ();
         g_menu_append (menu, _("Open"), "index.open");
         g_menu_append (menu, _("Open in New Tab"), "index.open-new-tab");
         if (iv->type == GIMV_INDEX_TAB_PERSON)
            g_menu_append (menu, _("Show the Faces..."), "index.faces");
         if (iv->type == GIMV_INDEX_TAB_TAG)
            g_menu_append (menu, _("Rename... (F2)"), "index.rename");
         iv->popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
         gtk_widget_set_parent (iv->popover, iv->scroll);
         gtk_popover_set_has_arrow (GTK_POPOVER (iv->popover), FALSE);
         g_object_unref (menu);
      }
      if (iv->type == GIMV_INDEX_TAB_TAG) {
         /* the tags of rules are not renamed */
         GAction *a = g_action_map_lookup_action (G_ACTION_MAP (iv->actions), "rename");
         g_simple_action_set_enabled (G_SIMPLE_ACTION (a), key_is_manual_tag (iv, key));
      }
      gtk_popover_set_pointing_to (GTK_POPOVER (iv->popover), &rect);
      gtk_popover_popup (GTK_POPOVER (iv->popover));
   }
}


/******************************************************************************
 *  renaming a tag in the tree (F2, or the menu); copying a row (Ctrl+C)
 ******************************************************************************/

/* a tag a person put (those of rules are made by the rules) */
static gboolean
key_is_manual_tag (IndexView *iv, const gchar *key)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter root, iter;
   gint kind = -1;

   if (iv->type != GIMV_INDEX_TAB_TAG || !key || !*key || key[0] == AUTO_KEY_MARK)
      return FALSE;
   if (!gtk_tree_model_get_iter_first (model, &root) || !find_key (model, &root, key, &iter))
      return FALSE;
   gtk_tree_model_get (model, &iter, COL_KIND, &kind, -1);
   return kind == KIND_TAG;
}


static void
start_rename (IndexView *iv, const gchar *key)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeIter root, iter;
   GtkTreePath *path;

   if (!key_is_manual_tag (iv, key)) return;
   if (!gtk_tree_model_get_iter_first (model, &root) || !find_key (model, &root, key, &iter))
      return;
   g_free (iv->editing_key);
   iv->editing_key = g_strdup (key);
   path = gtk_tree_model_get_path (model, &iter);
   g_object_set (iv->render, "editable", TRUE, NULL);
   gtk_tree_view_set_cursor (GTK_TREE_VIEW (iv->tree), path, iv->col, TRUE);
   gtk_tree_path_free (path);
}


static void
cb_editing_started (GtkCellRenderer *render, GtkCellEditable *editable, gchar *path,
                    IndexView *iv)
{
   /* the name alone, not "Kyoto (12)" */
   if (GTK_IS_EDITABLE (editable) && iv->editing_key) {
      gtk_editable_set_text (GTK_EDITABLE (editable), iv->editing_key);
      gtk_editable_select_region (GTK_EDITABLE (editable), 0, -1);
   }
}


static void
cb_editing_canceled (GtkCellRenderer *render, IndexView *iv)
{
   g_object_set (iv->render, "editable", FALSE, NULL);
   g_clear_pointer (&iv->editing_key, g_free);
   if (iv->count_again) start_count (iv);
}


static void
alert (IndexView *iv, const gchar *message, const gchar *detail)
{
   GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", message);
   GtkRoot *root = gtk_widget_get_root (iv->tree);
   if (detail) gtk_alert_dialog_set_detail (dialog, detail);
   gtk_alert_dialog_show (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
   g_object_unref (dialog);
}


typedef struct {
   IndexView *iv;      /* the view may be gone: checked by the cancellable */
   GCancellable *cancel;
   gchar *old, *new;
} RenameAsk;


static void
do_rename (IndexView *iv, const gchar *old, const gchar *new)
{
   if (!gimv_index_rename_tag (old, new))
      alert (iv, _("The tag cannot be renamed now."),
             _("The indexer does not run (the index is off, or another GImageView makes it)."));
}


static void
cb_merge_answer (GObject *source, GAsyncResult *res, gpointer data)
{
   RenameAsk *ask = data;
   gint button = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), res, NULL);

   if (button == 1 && !g_cancellable_is_cancelled (ask->cancel))
      do_rename (ask->iv, ask->old, ask->new);
   g_object_unref (ask->cancel);
   g_free (ask->old);
   g_free (ask->new);
   g_free (ask);
}


/* a tag a person put on some image (the name is used) */
static gboolean
manual_tag_exists (const gchar *name)
{
   sqlite3 *db = gimv_index_get_db ();
   sqlite3_stmt *st;
   gboolean found = FALSE;

   if (!db || sqlite3_prepare_v2 (db, "SELECT 1 FROM tags t JOIN file_tags ft "
                                      "ON ft.tag_id = t.id WHERE t.name = ?1 AND ft.auto = 0 "
                                      "LIMIT 1", -1, &st, NULL) != SQLITE_OK)
      return FALSE;
   sqlite3_bind_text (st, 1, name, -1, SQLITE_TRANSIENT);
   found = sqlite3_step (st) == SQLITE_ROW;
   sqlite3_finalize (st);
   return found;
}


static void
cb_edited (GtkCellRendererText *render, gchar *path, gchar *text, IndexView *iv)
{
   gchar *old = g_steal_pointer (&iv->editing_key);
   gchar *new = g_strstrip (g_strdup (text ? text : ""));

   g_object_set (iv->render, "editable", FALSE, NULL);
   if (!old || !*new || !strcmp (old, new)) {
      /* nothing */
   } else if (strchr (new, ',') || strpbrk (new, "\t\n")) {
      /* the comment files keep the tags separated by commas */
      alert (iv, _("A tag cannot have a comma in it."), NULL);
   } else if (manual_tag_exists (new)) {
      /* the same thing tagged with two names: one tag, after asking */
      GtkAlertDialog *dialog = gtk_alert_dialog_new (_("The tag \"%s\" is there already."), new);
      const gchar *buttons[] = { _("_Cancel"), _("_Merge Them"), NULL };
      gchar *detail = g_strdup_printf (_("The images with \"%s\" get \"%s\" instead, and "
                                         "\"%s\" goes."), old, new, old);
      RenameAsk *ask = g_new0 (RenameAsk, 1);
      GtkRoot *root = gtk_widget_get_root (iv->tree);

      ask->iv = iv;
      ask->cancel = g_object_ref (iv->cancel);
      ask->old = g_strdup (old);
      ask->new = g_strdup (new);
      gtk_alert_dialog_set_detail (dialog, detail);
      gtk_alert_dialog_set_buttons (dialog, buttons);
      gtk_alert_dialog_set_cancel_button (dialog, 0);
      gtk_alert_dialog_set_default_button (dialog, 1);
      gtk_alert_dialog_choose (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                               NULL, cb_merge_answer, ask);
      g_object_unref (dialog);
      g_free (detail);
   } else {
      do_rename (iv, old, new);
   }
   g_free (old);
   g_free (new);
   if (iv->count_again) start_count (iv);
}


static gboolean
idle_rename (gpointer data)
{
   IndexView *iv = data;
   iv->rename_id = 0;
   gtk_widget_grab_focus (iv->tree);
   start_rename (iv, iv->menu_key);
   return G_SOURCE_REMOVE;
}


static void
cb_action_rename (GSimpleAction *action, GVariant *param, gpointer data)
{
   IndexView *iv = data;
   /* after the menu has gone: its focus going back would end the editing */
   if (!iv->rename_id)
      iv->rename_id = g_timeout_add (50, idle_rename, iv);
}


/* the text of the row: a tag's name, a person's name, a date */
static gchar *
row_copy_text (IndexView *iv, GtkTreeIter *iter)
{
   gchar *key = NULL, *title = NULL, *text;
   gint kind = -1;

   gtk_tree_model_get (GTK_TREE_MODEL (iv->store), iter, COL_KEY, &key, COL_TITLE, &title,
                       COL_KIND, &kind, -1);
   if (!key || kind == KIND_ROOT || kind == KIND_GROUP || kind == KIND_HEADER) {
      text = NULL;
   } else if (iv->type == GIMV_INDEX_TAB_PERSON) {
      text = g_strdup (title);
   } else if (iv->type == GIMV_INDEX_TAB_TAG && key[0] == AUTO_KEY_MARK) {
      text = g_strdup (auto_tag_value (key + 1, NULL));
   } else {
      text = g_strdup (key);
   }
   g_free (key);
   g_free (title);
   return text;
}


/******************************************************************************
 *  Ctrl+F: a row by (a part of) its name, in the rows there are (the days
 *  of a month not opened yet are not looked at)
 ******************************************************************************/

/* the row after (forward) or before "iter" in the order shown, all levels;
 * FALSE at the end */
static gboolean
tree_step (GtkTreeModel *model, GtkTreeIter *iter, gboolean forward)
{
   GtkTreeIter it;

   if (forward) {
      GtkTreeIter parent;
      if (gtk_tree_model_iter_children (model, &it, iter)) { *iter = it; return TRUE; }
      it = *iter;
      for (;;) {
         GtkTreeIter next = it;
         if (gtk_tree_model_iter_next (model, &next)) { *iter = next; return TRUE; }
         if (!gtk_tree_model_iter_parent (model, &parent, &it)) return FALSE;
         it = parent;
      }
   } else {
      GtkTreeIter prev = *iter, parent;
      if (gtk_tree_model_iter_previous (model, &prev)) {
         gint n;
         /* the last row below it */
         while ((n = gtk_tree_model_iter_n_children (model, &prev)) > 0) {
            gtk_tree_model_iter_nth_child (model, &it, &prev, n - 1);
            prev = it;
         }
         *iter = prev;
         return TRUE;
      }
      if (gtk_tree_model_iter_parent (model, &parent, iter)) { *iter = parent; return TRUE; }
      return FALSE;
   }
}


static gboolean
row_matches (GtkTreeModel *model, GtkTreeIter *iter, const gchar *folded)
{
   gchar *label = NULL, *norm, *fold;
   gboolean match = FALSE;

   gtk_tree_model_get (model, iter, COL_LABEL, &label, -1);
   if (label) {
      norm = g_utf8_normalize (label, -1, G_NORMALIZE_ALL);
      fold = norm ? g_utf8_casefold (norm, -1) : NULL;
      match = fold && strstr (fold, folded);
      g_free (fold);
      g_free (norm);
   }
   g_free (label);
   return match;
}


/* from the row selected (itself too when "again" is FALSE) on, around at
 * the end; the row found is selected and shown */
static gboolean
search_row (IndexView *iv, gboolean forward, gboolean again)
{
   GtkTreeModel *model = GTK_TREE_MODEL (iv->store);
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (iv->search_entry));
   gchar *norm, *folded;
   GtkTreeIter start, iter;
   gboolean found = FALSE, wrapped = FALSE;

   if (!text || !*text) return FALSE;
   norm = g_utf8_normalize (text, -1, G_NORMALIZE_ALL);
   folded = norm ? g_utf8_casefold (norm, -1) : NULL;
   g_free (norm);
   if (!folded) return FALSE;

   if (!gtk_tree_selection_get_selected (sel, NULL, &start)
       && !gtk_tree_model_get_iter_first (model, &start))
   {
      g_free (folded);
      return FALSE;
   }
   iter = start;
   if (!again && row_matches (model, &iter, folded)) {
      found = TRUE;
   } else {
      for (;;) {
         if (!tree_step (model, &iter, forward)) {
            /* around: from the first (the last) row */
            if (wrapped) break;
            wrapped = TRUE;
            if (forward) {
               if (!gtk_tree_model_get_iter_first (model, &iter)) break;
            } else {
               GtkTreeIter last;
               gint n = gtk_tree_model_iter_n_children (model, NULL);
               if (n < 1 || !gtk_tree_model_iter_nth_child (model, &last, NULL, n - 1)) break;
               iter = last;
               while ((n = gtk_tree_model_iter_n_children (model, &iter)) > 0) {
                  gtk_tree_model_iter_nth_child (model, &last, &iter, n - 1);
                  iter = last;
               }
            }
         }
         if (row_matches (model, &iter, folded)) { found = TRUE; break; }
         if (wrapped && gtk_tree_store_iter_is_valid (iv->store, &iter)) {
            GtkTreePath *a = gtk_tree_model_get_path (model, &iter);
            GtkTreePath *b = gtk_tree_model_get_path (model, &start);
            gboolean back = !gtk_tree_path_compare (a, b);
            gtk_tree_path_free (a);
            gtk_tree_path_free (b);
            if (back) break;
         }
      }
   }
   g_free (folded);

   if (found) {
      GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
      gtk_tree_view_expand_to_path (GTK_TREE_VIEW (iv->tree), path);
      gtk_tree_view_set_cursor (GTK_TREE_VIEW (iv->tree), path, NULL, FALSE);
      gtk_tree_view_scroll_to_cell (GTK_TREE_VIEW (iv->tree), path, NULL, TRUE, 0.3, 0.0);
      gtk_tree_path_free (path);
   }
   /* not found: the entry in red */
   if (found || !*text)
      gtk_widget_remove_css_class (iv->search_entry, "error");
   else
      gtk_widget_add_css_class (iv->search_entry, "error");
   return found;
}


static void
search_start (IndexView *iv)
{
   gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (iv->search_bar), TRUE);
   gtk_widget_grab_focus (iv->search_entry);
   gtk_editable_select_region (GTK_EDITABLE (iv->search_entry), 0, -1);
}


static void
search_stop (IndexView *iv)
{
   gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (iv->search_bar), FALSE);
   gtk_widget_grab_focus (iv->tree);
}


static void
cb_search_changed (GtkSearchEntry *entry, IndexView *iv)
{
   search_row (iv, TRUE, FALSE);
}


static void
cb_search_next (GtkSearchEntry *entry, IndexView *iv)
{
   search_row (iv, TRUE, TRUE);
}


static void
cb_search_previous (GtkSearchEntry *entry, IndexView *iv)
{
   search_row (iv, FALSE, TRUE);
}


/* Enter: the row found is opened (as Enter on the tree) */
static void
cb_search_activate (GtkSearchEntry *entry, IndexView *iv)
{
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   GtkTreeIter iter;
   gchar *key;

   if (gtk_tree_selection_get_selected (sel, NULL, &iter) && row_key (iv, &iter, &key)) {
      open_key (iv, key, FALSE);
      g_free (key);
   }
   search_stop (iv);
}


static void
cb_search_stop (GtkSearchEntry *entry, IndexView *iv)
{
   search_stop (iv);
}


/* Down and Up: the next and the one before, as Ctrl+G and Shift+Ctrl+G */
static gboolean
cb_search_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
               IndexView *iv)
{
   if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
      search_row (iv, TRUE, TRUE);
      return TRUE;
   }
   if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
      search_row (iv, FALSE, TRUE);
      return TRUE;
   }
   /* the text, not the window's Ctrl+F (open a file) and Ctrl+A (select
    * all the thumbnails) */
   if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F || keyval == GDK_KEY_a || keyval == GDK_KEY_A)
       && (state & GDK_CONTROL_MASK))
   {
      gtk_editable_select_region (GTK_EDITABLE (iv->search_entry), 0, -1);
      return TRUE;
   }
   return FALSE;
}


static gboolean
cb_tree_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
             IndexView *iv)
{
   GtkTreeSelection *sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (iv->tree));
   GtkTreeIter iter;
   gchar *key = NULL, *text;

   /* Ctrl+F here: a row by its name (not the window's "open a file") */
   if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F)
       && (state & GDK_MODIFIER_MASK) == GDK_CONTROL_MASK)
   {
      search_start (iv);
      return TRUE;
   }
   if (!gtk_tree_selection_get_selected (sel, NULL, &iter)) return FALSE;

   if (keyval == GDK_KEY_F2 && !(state & GDK_MODIFIER_MASK)) {
      gtk_tree_model_get (GTK_TREE_MODEL (iv->store), &iter, COL_KEY, &key, -1);
      start_rename (iv, key);
      g_free (key);
      return TRUE;
   }
   if ((keyval == GDK_KEY_c || keyval == GDK_KEY_C) && (state & GDK_CONTROL_MASK)) {
      text = row_copy_text (iv, &iter);
      if (text)
         gdk_clipboard_set_text (gtk_widget_get_clipboard (iv->tree), text);
      g_free (text);
      return text != NULL;
   }
   return FALSE;
}


/******************************************************************************
 *  widget
 ******************************************************************************/

static void
cb_destroy (GtkWidget *widget, IndexView *iv)
{
   g_cancellable_cancel (iv->cancel);
   g_clear_object (&iv->cancel);
   if (iv->watch_id) gimv_index_remove_watch (iv->watch_id);
   if (iv->open_id) g_source_remove (iv->open_id);
   if (iv->rename_id) g_source_remove (iv->rename_id);
   if (iv->popover) gtk_widget_unparent (iv->popover);
   set_result_tab (iv, NULL);
   g_clear_object (&iv->store);
   g_free (iv->menu_key);
   g_free (iv->click_key);
   g_free (iv->shown_key);
   g_free (iv->editing_key);
   g_free (iv);
}


static const gchar *
tab_title (GimvIndexTabType type)
{
   switch (type) {
   case GIMV_INDEX_TAB_TAG:    return _("Tag");
   case GIMV_INDEX_TAB_PERSON: return _("People");
   default:                    return _("Date");
   }
}


/* the theme's, or the one of GImageView (etc/icons) */
static const gchar *
tab_icon (GimvIndexTabType type)
{
   switch (type) {
   case GIMV_INDEX_TAB_TAG:    return "tag-symbolic";
   case GIMV_INDEX_TAB_PERSON: return "avatar-default-symbolic";
   default:                    return "x-office-calendar-symbolic";
   }
}


static GtkWidget *
index_view_new (GimvThumbWin *tw, GimvIndexTabType type)
{
   static const GActionEntry actions[] = {
      { "open",         cb_action_open, NULL, NULL, NULL },
      { "open-new-tab", cb_action_open, NULL, NULL, NULL },
      { "faces",        cb_action_faces, NULL, NULL, NULL },
      { "rename",       cb_action_rename, NULL, NULL, NULL },
   };
   IndexView *iv = g_new0 (IndexView, 1);
   GtkTreeViewColumn *col;
   GtkCellRenderer *render;
   GtkTreeIter root;
   GtkGesture *click;
   GSimpleActionGroup *group;

   iv->type = type;
   iv->tw = tw;
   iv->store = gtk_tree_store_new (N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING);

   gtk_tree_store_append (iv->store, &root, NULL);
   gtk_tree_store_set (iv->store, &root,
                       COL_LABEL, tab_title (type),
                       COL_KIND, KIND_ROOT, -1);
   iv->cancel = g_cancellable_new ();

   iv->tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL (iv->store));
   gtk_widget_set_name (iv->tree, "IndexTree");
   gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (iv->tree), FALSE);
   /* no typing to search: the keys of the window (P: the faces panel) work
    * here as in the directory view, and the search popup of GTK 4 gave a
    * Gtk-CRITICAL (gtk_css_node_insert_after) on a tree without headers */
   gtk_tree_view_set_enable_search (GTK_TREE_VIEW (iv->tree), FALSE);
   col = gtk_tree_view_column_new ();
   render = gtk_cell_renderer_text_new ();
   gtk_tree_view_column_pack_start (col, render, TRUE);
   gtk_tree_view_column_add_attribute (col, render, "text", COL_LABEL);
   gtk_tree_view_column_add_attribute (col, render, "foreground", COL_COLOR);
   gtk_tree_view_append_column (GTK_TREE_VIEW (iv->tree), col);
   iv->render = render;
   iv->col = col;
   /* editable only while a tag is renamed (start_rename) */
   g_signal_connect (render, "editing-started", G_CALLBACK (cb_editing_started), iv);
   g_signal_connect (render, "editing-canceled", G_CALLBACK (cb_editing_canceled), iv);
   g_signal_connect (render, "edited", G_CALLBACK (cb_edited), iv);
   {
      GtkEventController *keys = gtk_event_controller_key_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_tree_key), iv);
      gtk_widget_add_controller (iv->tree, keys);
   }
   /* Enter (and a double click) */
   g_signal_connect (iv->tree, "row-activated", G_CALLBACK (cb_row_activated), iv);

   click = gtk_gesture_click_new ();
   gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), 0);
   gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click),
                                               GTK_PHASE_CAPTURE);
   g_signal_connect (click, "pressed", G_CALLBACK (cb_tree_pressed), iv);
   gtk_widget_add_controller (iv->tree, GTK_EVENT_CONTROLLER (click));


   iv->scroll = gtk_scrolled_window_new ();
   gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (iv->scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (iv->scroll), iv->tree);
   gtk_widget_set_name (iv->scroll, "IndexView");
   /* on the scrolled window, where the menu is */
   group = g_simple_action_group_new ();
   g_action_map_add_action_entries (G_ACTION_MAP (group), actions,
                                    G_N_ELEMENTS (actions), iv);
   gtk_widget_insert_action_group (iv->scroll, "index", G_ACTION_GROUP (group));
   iv->actions = group;   /* the scrolled window holds it */
   g_object_unref (group);

   g_signal_connect (iv->scroll, "destroy", G_CALLBACK (cb_destroy), iv);
   iv->watch_id = gimv_index_add_watch (cb_index_changed, iv);
   start_count (iv);

   /* Ctrl+F: a search entry above the tree (GTK's own typing search is
    * off: see above) */
   {
      GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
      GtkEventController *keys = gtk_event_controller_key_new ();

      iv->search_entry = gtk_search_entry_new ();
      gtk_widget_set_hexpand (iv->search_entry, TRUE);
      iv->search_bar = gtk_search_bar_new ();
      gtk_search_bar_set_child (GTK_SEARCH_BAR (iv->search_bar), iv->search_entry);
      gtk_search_bar_connect_entry (GTK_SEARCH_BAR (iv->search_bar),
                                    GTK_EDITABLE (iv->search_entry));
      gtk_search_bar_set_show_close_button (GTK_SEARCH_BAR (iv->search_bar), TRUE);
      g_signal_connect (iv->search_entry, "search-changed", G_CALLBACK (cb_search_changed), iv);
      g_signal_connect (iv->search_entry, "next-match", G_CALLBACK (cb_search_next), iv);
      g_signal_connect (iv->search_entry, "previous-match", G_CALLBACK (cb_search_previous), iv);
      g_signal_connect (iv->search_entry, "activate", G_CALLBACK (cb_search_activate), iv);
      g_signal_connect (iv->search_entry, "stop-search", G_CALLBACK (cb_search_stop), iv);
      gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_search_key), iv);
      gtk_widget_add_controller (iv->search_entry, keys);

      gtk_widget_set_vexpand (iv->scroll, TRUE);
      gtk_box_append (GTK_BOX (box), iv->search_bar);
      gtk_box_append (GTK_BOX (box), iv->scroll);
      return box;
   }
}


/******************************************************************************
 *  the "Date" and "Tag" tabs next to the directory view
 ******************************************************************************/

static const gchar *tab_keys[] = { "gimv-index-date-tab", "gimv-index-tag-tab",
                                   "gimv-index-person-tab" };


GtkWidget *
gimv_index_tab_get_page (GimvThumbWin *tw, GimvIndexTabType type)
{
   return g_object_get_data (G_OBJECT (tw), tab_keys[type]);
}


static void
cb_page_destroy (GtkWidget *page, GimvThumbWin *tw)
{
   gint type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (page), "gimv-index-tab-type"));
   if (gimv_index_tab_get_page (tw, type) == page)
      g_object_set_data (G_OBJECT (tw), tab_keys[type], NULL);
}


void
gimv_index_tab_open (GimvThumbWin *tw, GimvIndexTabType type)
{
   GtkWidget *page, *hbox, *label, *button;

   g_return_if_fail (GIMV_IS_THUMB_WIN (tw));
   if (gimv_index_tab_get_page (tw, type) || !tw->side_notebook) return;

   page = index_view_new (tw, type);
   g_object_set_data (G_OBJECT (page), "gimv-index-tab-type", GINT_TO_POINTER (type));

   hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
   g_object_set_data (G_OBJECT (hbox), "page-container", page);
   label = gtkutil_icon_tab_label_new (tab_icon (type), tab_title (type));
   gtk_widget_set_name (label, "TabLabel");
   gtk_box_append (GTK_BOX (hbox), label);

   /* no close button: the side bar is narrow; the tabs open and close
      with the preferences */
   (void) button;

   /* next to the directory view (the side notebook), not among the
      thumbnail tabs: the tree stays in view with the results */
   {
      /* the page shown stays (opened at start: the folders) */
      gint cur = gtk_notebook_get_current_page (GTK_NOTEBOOK (tw->side_notebook));
      gtk_notebook_append_page (GTK_NOTEBOOK (tw->side_notebook), page, hbox);
      if (cur >= 0)
         gtk_notebook_set_current_page (GTK_NOTEBOOK (tw->side_notebook), cur);
   }
   gtk_notebook_set_menu_label_text (GTK_NOTEBOOK (tw->side_notebook), page, tab_title (type));
   gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (tw->side_notebook), page, TRUE);

   g_object_set_data (G_OBJECT (tw), tab_keys[type], page);
   g_signal_connect (page, "destroy", G_CALLBACK (cb_page_destroy), tw);
   gimv_thumb_win_set_statusbar_page_info (tw, GIMV_THUMB_WIN_CURRENT_PAGE);
}


void
gimv_index_tab_close (GimvThumbWin *tw, GimvIndexTabType type)
{
   GtkWidget *page;
   gint num;

   g_return_if_fail (GIMV_IS_THUMB_WIN (tw));
   page = gimv_index_tab_get_page (tw, type);
   if (!page) return;

   g_object_set_data (G_OBJECT (tw), tab_keys[type], NULL);
   num = tw->side_notebook
         ? gtk_notebook_page_num (GTK_NOTEBOOK (tw->side_notebook), page) : -1;
   if (num >= 0)
      gtk_notebook_remove_page (GTK_NOTEBOOK (tw->side_notebook), num);
   gimv_thumb_win_set_statusbar_page_info (tw, GIMV_THUMB_WIN_CURRENT_PAGE);
}


/* the tabs there should be, from the preferences */
void
gimv_index_tabs_sync (GimvThumbWin *tw)
{
   if (gimv_index_is_configured ()) {
      gimv_index_tab_open (tw, GIMV_INDEX_TAB_DATE);
      gimv_index_tab_open (tw, GIMV_INDEX_TAB_TAG);
   } else {
      gimv_index_tab_close (tw, GIMV_INDEX_TAB_DATE);
      gimv_index_tab_close (tw, GIMV_INDEX_TAB_TAG);
   }
   if (gimv_index_faces_is_configured ())
      gimv_index_tab_open (tw, GIMV_INDEX_TAB_PERSON);
   else
      gimv_index_tab_close (tw, GIMV_INDEX_TAB_PERSON);
}
