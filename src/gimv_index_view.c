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
#include "gimv_list_view.h"
#include "gtkutils.h"
#include "gimv_icon_stock.h"
#include "gimv_index.h"
#include "gimv_index_db.h"
#include "gimv_index_faces.h"
#include "gimv_index_faces_win.h"
#include "gimv_prefs_win.h"
#include "menu.h"
#include "prefs.h"
#include "gimv_index_view.h"
#include "gimv_bookmark.h"
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
   KIND_BOOKMARK,  /* bookmarks: "Today", "2026-10", "With a note", ... */
} RowKind;

enum {
   COL_LABEL,
   COL_KIND,
   COL_KEY,        /* "2019", "2019-05", "2019-05-03", or the tag name;
                    * a tag put by a rule starts with "," (a tag never
                    * has a comma), a group of them is ",folder/" */
   COL_COLOR,      /* the tags put by rules have a color of their own */
   COL_TITLE,      /* people, bookmarks: the title of the result tab */
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
/* the names of faces, written into the comment files by the indexer (so
 * tags put by a person, but kept apart in their own group) */
#define NAME_TAG       "person/"
#define NAME_TAG_LEN   7
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
const gchar *
gimv_index_plain_key (const gchar *key)
{
   static const gchar kw[] = { AUTO_KEY_MARK, 'k', 'e', 'y', 'w', 'o', 'r', 'd', '/', '\0' };

   if (key && g_str_has_prefix (key, kw) && key[sizeof kw - 1]) return key + sizeof kw - 1;
   return key;
}


static gchar *
tag_title (const gchar *key)
{
   gint kind;
   const gchar *v;

   key = gimv_index_plain_key (key);
   if (g_str_has_prefix (key, NAME_TAG) && key[NAME_TAG_LEN])
      return g_strdup_printf (_("%s (Name)"), key + NAME_TAG_LEN);
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
   GtkWidget     *tree;        /* a GtkListView */
   GListStore    *store;       /* the root row (a GimvRow; the rest below it) */
   GimvRow       *root;
   GtkTreeListModel *model;
   guint          watch_id;
   guint          open_id;
   GimvThumbView *tv;          /* the tab of the last result, weak */
   gboolean       filling;
   GtkWidget     *popover;
   GtkWidget     *rename_popover; /* the entry a tag is renamed in */
   GtkWidget     *rename_entry;
   guint          rename_focus_id;
   gchar         *menu_key;    /* row of the right button menu */
   gchar         *click_key;   /* row clicked with the left button */
   gchar         *shown_key;   /* what the result tab shows */

   GCancellable  *cancel;      /* cancelled when the view goes away */
   gboolean       counting;    /* a day count query is running */
   gboolean       count_again; /* the database changed meanwhile */
   gboolean       listing;     /* a file list query is running */
   gboolean       filled;      /* refilled once: what is open is kept */
   GSimpleActionGroup *actions;
   gchar         *editing_key; /* the tag being renamed in the tree */
   guint          rename_id;
   gboolean       click_open;  /* open_key () from a click on the tree */
   GtkWidget     *search_bar;  /* Ctrl+F: a row by its name */
   GtkWidget     *search_entry;
   guint          bookmark_watch;
} IndexView;


static gboolean key_is_manual_tag (IndexView *iv, const gchar *key);
static gboolean key_is_renamable (IndexView *iv, const gchar *key);

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

   /* the tags put by a person with the keywords of other programs of the
    * same name (each image once), then those put by rules (the same name
    * can be both; it is not usual: the rules make "folder/...") */
   if (db && sqlite3_prepare_v2 (db,
          "SELECT n, 0, count(DISTINCT id) FROM ("
          " SELECT t.name AS n, ft.file_id AS id FROM file_tags ft"
          "  JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 0"
          " UNION ALL SELECT substr (t.name, 9), ft.file_id FROM file_tags ft"
          "  JOIN tags t ON t.id = ft.tag_id WHERE ft.auto = 1"
          "  AND t.name >= 'keyword/' AND t.name < 'keyword0') GROUP BY n "
          "UNION ALL SELECT t.name, 1, count(*) FROM file_tags ft"
          " JOIN tags t ON t.id = ft.tag_id WHERE " GIMV_INDEX_TAG_AUTO_SQL
          " GROUP BY t.id ORDER BY 1",
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
                                  g_strdup_printf (ngettext ("%s (%'d person)", "%s (%'d people)", members->len), cur, members->len),
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
   } else if (req->type == GIMV_INDEX_TAB_TAG && gimv_index_plain_key (key)[0] != AUTO_KEY_MARK)
      /* put by a person, or a keyword of the same name */
      sql = "SELECT d.path, f.name, f.size, f.mtime, f.date FROM files f "
            "JOIN dirs d ON d.id = f.dir_id WHERE f.id IN (SELECT ft.file_id FROM file_tags ft "
            "JOIN tags t ON t.id = ft.tag_id WHERE (t.name = ?1 AND ft.auto = ?2) "
            "OR (t.name = 'keyword/' || ?1 AND ft.auto = 1)) ORDER BY f.date, f.name";
   else if (req->type == GIMV_INDEX_TAB_TAG)
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
         gboolean is_auto;
         key = gimv_index_plain_key (key);
         is_auto = key[0] == AUTO_KEY_MARK;
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

static GimvRow *
row_new (GimvRow *parent, const gchar *label, RowKind kind, const gchar *key,
         const gchar *color, const gchar *title)
{
   GimvRow *row = gimv_row_new (N_COLS, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_STRING);
   gimv_row_set (row, COL_LABEL, label, COL_KIND, kind, COL_KEY, key,
                 COL_COLOR, color, COL_TITLE, title, -1);
   if (parent) {
      gimv_row_append_child (parent, row);
      g_object_unref (row);   /* the parent holds it */
   }
   return row;
}


static void
set_count (GimvRow *row, gint len, gint count)
{
   const gchar *k = gimv_row_peek_string (row, COL_KEY);
   /* the last part of the key: "2019", "05", "03" */
   gchar *label = g_strdup_printf ("%s (%'d)", k + (len == 4 ? 0 : len - 2), count);
   gimv_row_set (row, COL_LABEL, label, -1);
   g_free (label);
}


/* days come newest first, and are shown so: years, months and days */
static void
build_tree (IndexView *iv, GimvRow *root, GArray *days)
{
   GimvRow *year = NULL, *month = NULL, *day;
   gchar cur_year[5] = "", cur_month[8] = "";
   gint year_count = 0, month_count = 0;
   gint i;

   for (i = 0; i < (gint) days->len; i++) {
      DayCount *dc = &g_array_index (days, DayCount, i);

      if (strncmp (dc->day, cur_year, 4)) {
         gchar *k = g_strndup (dc->day, 4);
         if (*cur_month) set_count (month, 7, month_count);
         if (*cur_year) set_count (year, 4, year_count);
         year = row_new (root, NULL, KIND_YEAR, k, NULL, NULL);
         g_free (k);
         memcpy (cur_year, dc->day, 4);
         year_count = 0;
         *cur_month = '\0';
      }
      if (strncmp (dc->day, cur_month, 7)) {
         gchar *k = g_strndup (dc->day, 7);
         if (*cur_month) set_count (month, 7, month_count);
         month = row_new (year, NULL, KIND_MONTH, k, NULL, NULL);
         g_free (k);
         memcpy (cur_month, dc->day, 7);
         month_count = 0;
      }
      day = row_new (month, NULL, KIND_DAY, dc->day, NULL, NULL);
      set_count (day, 10, dc->count);

      year_count  += dc->count;
      month_count += dc->count;
   }
   if (*cur_month) set_count (month, 7, month_count);
   if (*cur_year)  set_count (year, 4, year_count);
}


/* the row of the key below parent (any depth), NULL if none */
static GimvRow *
find_key (GimvRow *parent, const gchar *key)
{
   GListStore *children = parent ? gimv_row_get_children (parent, FALSE) : NULL;
   guint i, n;

   if (!children || !key) return NULL;
   n = g_list_model_get_n_items (G_LIST_MODEL (children));
   for (i = 0; i < n; i++) {
      GimvRow *row = gimv_row_store_peek (G_LIST_MODEL (children), i), *found;
      if (!g_strcmp0 (gimv_row_peek_string (row, COL_KEY), key)) return row;
      if ((found = find_key (row, key))) return found;
   }
   return NULL;
}


/* the tags put by a person at the top, then a group for each rule */
static void
build_tag_list (IndexView *iv, GimvRow *root, GPtrArray *tags)
{
   GimvRow *groups[G_N_ELEMENTS (auto_kinds)] = { NULL };
   GimvRow *names = NULL;
   guint i, k;

   for (i = 0; i < tags->len; i++) {
      TagCount *tc = g_ptr_array_index (tags, i);
      gchar *label;

      if (tc->auto_tag || g_str_has_prefix (tc->name, NAME_TAG)) continue;
      label = g_strdup_printf ("%s (%'d)", tc->name, tc->count);
      row_new (root, label, KIND_TAG, tc->name, NULL, NULL);
      g_free (label);
   }

   /* the names of faces in the comment files */
   for (i = 0; i < tags->len; i++) {
      TagCount *tc = g_ptr_array_index (tags, i);
      gchar *label;

      if (tc->auto_tag || !g_str_has_prefix (tc->name, NAME_TAG)) continue;
      if (!names)
         names = row_new (root, _("Names of People"), KIND_GROUP, NAME_TAG, NULL, NULL);
      label = g_strdup_printf ("%s (%'d)", tc->name + NAME_TAG_LEN, tc->count);
      row_new (names, label, KIND_TAG, tc->name, NULL, NULL);
      g_free (label);
   }

   for (k = 0; k < G_N_ELEMENTS (auto_kinds); k++) {
      for (i = 0; i < tags->len; i++) {
         TagCount *tc = g_ptr_array_index (tags, i);
         gchar *label, *key;
         gint kind;
         const gchar *v;

         if (!tc->auto_tag) continue;
         v = auto_tag_value (tc->name, &kind);
         if (kind != (gint) k) continue;

         if (!groups[k]) {
            gchar *gkey = g_strdup_printf ("%c%s", AUTO_KEY_MARK, auto_kinds[k].prefix);
            groups[k] = row_new (root, _(auto_kinds[k].group), KIND_GROUP, gkey,
                                 AUTO_TAG_COLOR, NULL);
            g_free (gkey);
         }
         label = g_strdup_printf ("%s (%'d)", v, tc->count);
         key = g_strdup_printf ("%c%s", AUTO_KEY_MARK, tc->name);
         row_new (groups[k], label, KIND_TAG, key, AUTO_TAG_COLOR, NULL);
         g_free (key);
         g_free (label);
      }
   }
}


static void
build_people_list (IndexView *iv, GimvRow *root, GPtrArray *rows)
{
   /* the last row at each depth: the parent of the rows below it */
   GimvRow *at[4];
   gint have = -1;
   guint i;

   for (i = 0; i < rows->len; i++) {
      PeopleRow *r = g_ptr_array_index (rows, i);
      gint depth = MIN (r->depth, have + 1);
      GimvRow *parent;

      depth = MIN (depth, (gint) G_N_ELEMENTS (at) - 1);
      parent = depth > 0 ? at[depth - 1] : root;
      at[depth] = row_new (parent, r->label, r->kind, r->key,
                           r->kind == KIND_FACES && r->key[0] != 'p' && r->key[0] != 't'
                           ? AUTO_TAG_COLOR : NULL, r->title);
      have = depth;
   }
}


static void
expand_row (IndexView *iv, GimvRow *row, gboolean expand)
{
   GtkTreeListRow *tree_row = gimv_tree_list_model_get_row_for (iv->model, row, NULL);
   if (tree_row) {
      gtk_tree_list_row_set_expanded (tree_row, expand);
      g_object_unref (tree_row);
   }
}


/******************************************************************************
 *  bookmarks: not of the index, from gimv_bookmark.c (read at once)
 ******************************************************************************/

/* the period a bookmark was added in: "today", "yesterday", "week" (2 to 6
 * days ago), else "month/YYYY-MM" */
static gchar *
bookmark_period (const GimvBookmark *bm, GDateTime *today)
{
   GDateTime *t = g_date_time_new_from_unix_local (bm->added);
   gint64 diff;
   gchar *key;

   if (!t) return g_strdup ("month/0000-00");
   diff = g_date_time_difference (today, t);   /* today 00:00 - added */
   if (diff <= 0)
      key = g_strdup ("today");
   else if (diff <= G_TIME_SPAN_DAY)
      key = g_strdup ("yesterday");
   else if (diff <= 6 * G_TIME_SPAN_DAY)
      key = g_strdup ("week");
   else
      key = g_date_time_format (t, "month/%Y-%m");
   g_date_time_unref (t);
   return key;
}


static GDateTime *
today_start (void)
{
   GDateTime *now = g_date_time_new_now_local (), *today;

   today = g_date_time_new_local (g_date_time_get_year (now), g_date_time_get_month (now),
                                  g_date_time_get_day_of_month (now), 0, 0, 0);
   g_date_time_unref (now);
   return today;
}


/* does the bookmark go into the row of key? */
static gboolean
bookmark_in (const GimvBookmark *bm, const gchar *key, GDateTime *today)
{
   gboolean in;
   gchar *period;

   if (!strcmp (key, "all"))    return TRUE;
   if (!strcmp (key, "note"))   return bm->note != NULL;
   if (!strcmp (key, "nonote")) return bm->note == NULL;
   period = bookmark_period (bm, today);
   in = !strcmp (period, key);
   g_free (period);
   return in;
}


static void
bookmark_row (GimvRow *root, const gchar *label, const gchar *key, gint count)
{
   gchar *text = g_strdup_printf ("%s (%'d)", label, count);
   row_new (root, text, KIND_BOOKMARK, key, NULL, label);
   g_free (text);
}


static void
build_bookmark_list (IndexView *iv, GimvRow *root)
{
   GPtrArray *bms = gimv_bookmark_list ();
   GDateTime *today = today_start ();
   GHashTable *counts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
   GPtrArray *months = g_ptr_array_new ();   /* the keys, newest first */
   gint note = 0, missing = 0, all = 0;
   guint i;

   for (i = 0; i < bms->len; i++) {
      GimvBookmark *bm = g_ptr_array_index (bms, i);
      gchar *period;
      gint n;

      /* not there (moved by hand, a disk not mounted): counted apart, the
       * rows count what they open */
      if (!g_file_test (bm->path, G_FILE_TEST_IS_REGULAR)) {
         missing++;
         continue;
      }
      all++;
      period = bookmark_period (bm, today);
      n = GPOINTER_TO_INT (g_hash_table_lookup (counts, period));

      if (!n && g_str_has_prefix (period, "month/"))
         g_ptr_array_add (months, period);   /* the table keeps it */
      g_hash_table_insert (counts, period, GINT_TO_POINTER (n + 1));
      if (bm->note) note++;
   }

   bookmark_row (root, _("All"), "all", all);
   {
      static const struct { const gchar *key, *label; } periods[] = {
         { "today", N_("Today") }, { "yesterday", N_("Yesterday") },
         { "week",  N_("This Week") },
      };
      for (i = 0; i < G_N_ELEMENTS (periods); i++) {
         gint n = GPOINTER_TO_INT (g_hash_table_lookup (counts, periods[i].key));
         if (n) bookmark_row (root, _(periods[i].label), periods[i].key, n);
      }
   }
   for (i = 0; i < months->len; i++) {
      const gchar *key = g_ptr_array_index (months, i);
      gint y = 0, m = 0;
      gchar *label;

      if (sscanf (key, "month/%d-%d", &y, &m) == 2 && m >= 1 && m <= 12) {
         GDateTime *t = g_date_time_new_local (y, m, 1, 0, 0, 0);
         /* TRANSLATORS: a month of the bookmarks tab (g_date_time_format) */
         label = t ? g_date_time_format (t, _("%B %Y")) : g_strdup (key + 6);
         if (t) g_date_time_unref (t);
      } else {
         label = g_strdup (key + 6);
      }
      bookmark_row (root, label, key, GPOINTER_TO_INT (g_hash_table_lookup (counts, key)));
      g_free (label);
   }
   if (all) {
      bookmark_row (root, _("With a Note"), "note", note);
      bookmark_row (root, _("Without a Note"), "nonote", all - note);
   }
   if (missing) {
      /* shown, not opened (there is nothing to show) */
      gchar *text = g_strdup_printf ("%s (%'d)", _("Not Found"), missing);
      row_new (root, text, KIND_HEADER, "missing", NULL, NULL);
      g_free (text);
   }

   g_ptr_array_unref (months);
   g_hash_table_unref (counts);
   g_date_time_unref (today);
   g_ptr_array_unref (bms);
}


/* the images of a row, newest first (those not there any more left out) */
static FileList *
bookmark_files (const gchar *key)
{
   GPtrArray *bms = gimv_bookmark_list ();
   GDateTime *today = today_start ();
   FileList *fl = g_new0 (FileList, 1);
   GList *list = NULL;
   guint i;

   for (i = 0; i < bms->len; i++) {
      GimvBookmark *bm = g_ptr_array_index (bms, i);
      if (bookmark_in (bm, key, today) && g_file_test (bm->path, G_FILE_TEST_IS_REGULAR))
         list = g_list_prepend (list, g_strdup (bm->path));
   }
   fl->paths = g_list_reverse (list);
   g_date_time_unref (today);
   g_ptr_array_unref (bms);
   return fl;
}


static void start_count (IndexView *iv);

static void
cb_bookmarks_changed (gpointer data)
{
   start_count (data);
}


/* rebuild the tree from new counts and open again what was open;
 * result: a GArray of DayCount or a GPtrArray of TagCount */
static void
refill (IndexView *iv, gpointer result)
{
   GListModel *model = G_LIST_MODEL (iv->model);
   GPtrArray *keys = g_ptr_array_new_with_free_func (g_free);
   GimvRow *row, *selected_row;
   gchar *selected = NULL;
   guint i, n;

   /* the rows open, in the order shown: a year before its months */
   n = g_list_model_get_n_items (model);
   for (i = 0; i < n; i++) {
      GtkTreeListRow *tree_row = g_list_model_get_item (model, i);
      if (gtk_tree_list_row_get_expanded (tree_row)) {
         row = gimv_row_from_item (tree_row);
         if (row && row != iv->root && gimv_row_peek_string (row, COL_KEY))
            g_ptr_array_add (keys, g_strdup (gimv_row_peek_string (row, COL_KEY)));
      }
      g_object_unref (tree_row);
   }
   selected_row = gimv_list_view_get_selected_row (iv->tree);
   if (selected_row)
      selected = g_strdup (gimv_row_peek_string (selected_row, COL_KEY));

   iv->filling = TRUE;
   /* built apart, then put below the root: a row shows an arrow when it
    * has rows below it as it goes into the tree */
   {
      GimvRow *tmp = row_new (NULL, NULL, KIND_ROOT, NULL, NULL, NULL);
      GListStore *built;

      if (iv->type == GIMV_INDEX_TAB_TAG)
         build_tag_list (iv, tmp, result);
      else if (iv->type == GIMV_INDEX_TAB_BOOKMARK)
         build_bookmark_list (iv, tmp);
      else if (iv->type == GIMV_INDEX_TAB_PERSON)
         build_people_list (iv, tmp, result);
      else
         build_tree (iv, tmp, result);
      gimv_row_remove_children (iv->root);
      built = gimv_row_get_children (tmp, FALSE);
      n = built ? g_list_model_get_n_items (G_LIST_MODEL (built)) : 0;
      for (i = 0; i < n; i++)
         gimv_row_append_child (iv->root, gimv_row_store_peek (G_LIST_MODEL (built), i));
      g_object_unref (tmp);
   }

   /* the root is always open */
   expand_row (iv, iv->root, TRUE);
   for (i = 0; i < keys->len; i++) {
      if ((row = find_key (iv->root, g_ptr_array_index (keys, i))))
         expand_row (iv, row, TRUE);
   }
   /* people: the groups and the guesses open the first time */
   if (!iv->filled && iv->type == GIMV_INDEX_TAB_PERSON) {
      GListStore *children = gimv_row_get_children (iv->root, FALSE);
      n = children ? g_list_model_get_n_items (G_LIST_MODEL (children)) : 0;
      for (i = 0; i < n; i++) {
         row = gimv_row_store_peek (G_LIST_MODEL (children), i);
         if (gimv_row_get_children (row, FALSE))
            expand_row (iv, row, TRUE);
      }
   }
   iv->filled = TRUE;
   if (selected && (row = find_key (iv->root, selected))) {
      GtkTreeListRow *tree_row;
      guint pos;
      if ((tree_row = gimv_tree_list_model_get_row_for (iv->model, row, &pos))) {
         gimv_list_view_select (iv->tree, pos);
         g_object_unref (tree_row);
      }
   }
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
   /* a few hundred lines of a file of their own: at once */
   if (iv->type == GIMV_INDEX_TAB_BOOKMARK) {
      refill (iv, NULL);
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
   /* bookmarks: newest first (the others: the sort of the window) */
   g_object_set_data (G_OBJECT (tv), GIMV_THUMB_VIEW_KEEP_ORDER,
                      GINT_TO_POINTER (req->type == GIMV_INDEX_TAB_BOOKMARK));
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
   else if (req->type == GIMV_INDEX_TAB_BOOKMARK)
      title = g_strdup_printf (_("%s (Bookmarks)"), req->title ? req->title : req->key);
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
   GimvRow *row;

   if (iv->type != GIMV_INDEX_TAB_PERSON && iv->type != GIMV_INDEX_TAB_BOOKMARK) return NULL;
   row = find_key (iv->root, key);
   return row ? g_strdup (gimv_row_peek_string (row, COL_TITLE)) : NULL;
}


/* new_tab FALSE: replace what the result tab shows (the usual way) */
static void
open_key (IndexView *iv, const gchar *key, gboolean new_tab)
{
   OpenRequest *req;
   GTask *task;

   if (iv->type == GIMV_INDEX_TAB_TAG) key = gimv_index_plain_key (key);

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

   if (iv->type == GIMV_INDEX_TAB_BOOKMARK) {
      open_files (iv, req, bookmark_files (key));
      open_request_free (req);
      return;
   }

   iv->listing = TRUE;
   task = g_task_new (NULL, iv->cancel, cb_listed, iv);
   g_task_set_task_data (task, req, (GDestroyNotify) open_request_free);
   g_task_run_in_thread (task, thread_list_files);
   g_object_unref (task);
}


/* the key of a row that opens images (not a group), or NULL */
static gchar *
row_key (GimvRow *row)
{
   gint kind;
   const gchar *key;

   if (!row) return NULL;
   kind = gimv_row_peek_int (row, COL_KIND);
   key = gimv_row_peek_string (row, COL_KEY);
   if (key && (kind == KIND_YEAR || kind == KIND_MONTH || kind == KIND_DAY
               || kind == KIND_TAG || kind == KIND_FACES || kind == KIND_BOOKMARK))
      return g_strdup (key);
   return NULL;
}


/* Enter (and a double click) */
static void
cb_activate (GtkListView *list, guint position, IndexView *iv)
{
   gchar *key = row_key (gimv_row_store_peek (G_LIST_MODEL (iv->model), position));

   if (key) {
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
   GimvRow *row = NULL;

   if (!gimv_list_view_get_row_at (iv->tree, x, y, NULL, &row) || !row)
      return NULL;
   if (on_text) {
      /* right of the arrow: the content of the expander (the label) */
      GtkWidget *w = gtk_widget_pick (iv->tree, x, y, GTK_PICK_DEFAULT);
      graphene_rect_t b;

      *on_text = FALSE;
      for (; w && w != iv->tree; w = gtk_widget_get_parent (w)) {
         if (GTK_IS_TREE_EXPANDER (w)) {
            GtkWidget *child = gtk_tree_expander_get_child (GTK_TREE_EXPANDER (w));
            *on_text = child && gtk_widget_compute_bounds (child, iv->tree, &b)
                       && x >= b.origin.x;
            break;
         }
      }
   }
   return row_key (row);
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


/* the folder name of a folder tag (",folder/NAME"), or NULL */
static const gchar *
key_folder_name (const gchar *key)
{
   static const gchar prefix[] = { AUTO_KEY_MARK, 'f', 'o', 'l', 'd', 'e', 'r', '/', '\0' };

   if (!key || !g_str_has_prefix (key, prefix) || !key[sizeof prefix - 1])
      return NULL;
   return key + sizeof prefix - 1;
}


/* "Do not make this folder name a tag": into the preferences, and the
 * indexer started again (it takes the tags off in the background) */
static void
cb_action_no_folder_tag (GSimpleAction *action, GVariant *param, gpointer data)
{
   IndexView *iv = data;
   const gchar *name = key_folder_name (iv->menu_key);
   gchar **v, *list;
   guint i;

   /* the preferences window would write its own copy back on OK */
   if (!name || gimv_prefs_win_is_opened ()) return;
   v = g_strsplit (conf.index_auto_folder_exclude ? conf.index_auto_folder_exclude : "", ";", -1);
   for (i = 0; v[i]; i++) {
      if (!g_utf8_collate (v[i], name)) {
         g_strfreev (v);
         return;
      }
   }
   g_strfreev (v);
   list = conf.index_auto_folder_exclude && *conf.index_auto_folder_exclude
      ? g_strconcat (conf.index_auto_folder_exclude, ";", name, NULL)
      : g_strdup (name);
   g_free (conf.index_auto_folder_exclude);
   conf.index_auto_folder_exclude = list;
   prefs_save_config ();
   gimv_index_restart ();
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
      /* on the scrolled window (with the rename popover) */
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
         if (iv->type == GIMV_INDEX_TAB_TAG || iv->type == GIMV_INDEX_TAB_PERSON)
            g_menu_append (menu, _("Rename... (F2)"), "index.rename");
         if (iv->type == GIMV_INDEX_TAB_TAG)
            g_menu_append (menu, _("Do Not Make This Folder Name a Tag"), "index.no-folder-tag");
         iv->popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
         gtk_widget_set_parent (iv->popover, iv->scroll);
         gtk_popover_set_has_arrow (GTK_POPOVER (iv->popover), FALSE);
         g_object_unref (menu);
      }
      if (iv->type == GIMV_INDEX_TAB_TAG || iv->type == GIMV_INDEX_TAB_PERSON) {
         /* the tags of rules are not renamed; people: only their tags (the
          * persons are renamed in the faces window) */
         GAction *a = g_action_map_lookup_action (G_ACTION_MAP (iv->actions), "rename");
         g_simple_action_set_enabled (G_SIMPLE_ACTION (a), key_is_renamable (iv, key));
         a = g_action_map_lookup_action (G_ACTION_MAP (iv->actions), "no-folder-tag");
         /* not while the preferences window is open: it has the list too
          * and would write it back on OK */
         g_simple_action_set_enabled (G_SIMPLE_ACTION (a), key_folder_name (key) != NULL
                                      && !gimv_prefs_win_is_opened ());
      }
      gtk_popover_set_pointing_to (GTK_POPOVER (iv->popover), &rect);
      gtk_popover_popup (GTK_POPOVER (iv->popover));
   }
}


/******************************************************************************
 *  renaming a tag in the tree (F2, or the menu); copying a row (Ctrl+C)
 ******************************************************************************/

static gboolean manual_tag_exists (const gchar *name);

/* a tag a person put (those of rules are made by the rules; a keyword of
 * another program alone is renamed there) */
static gboolean
key_is_manual_tag (IndexView *iv, const gchar *key)
{
   GimvRow *row;

   if (iv->type != GIMV_INDEX_TAB_TAG || !key || !*key || key[0] == AUTO_KEY_MARK)
      return FALSE;
   /* the names of faces: renamed as the person (the faces window) */
   if (g_str_has_prefix (key, NAME_TAG))
      return FALSE;
   row = find_key (iv->root, key);
   return row && gimv_row_peek_int (row, COL_KIND) == KIND_TAG && manual_tag_exists (key);
}


/* the tag of people of a row (",person/<tag>" in the tag tab, "t:<tag>"
 * in the people tab), NULL for the others */
static const gchar *
key_person_tag (IndexView *iv, const gchar *key)
{
   if (!key) return NULL;
   if (iv->type == GIMV_INDEX_TAB_TAG && g_str_has_prefix (key, ",person/") && key[8])
      return key + 8;
   if (iv->type == GIMV_INDEX_TAB_PERSON && g_str_has_prefix (key, "t:") && key[2])
      return key + 2;
   return NULL;
}


static gboolean
key_is_renamable (IndexView *iv, const gchar *key)
{
   return key_is_manual_tag (iv, key) || key_person_tag (iv, key) != NULL;
}


static void rename_done (IndexView *iv, const gchar *text);

static void
cb_rename_activate (GtkEntry *entry, IndexView *iv)
{
   gchar *text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));
   /* takes editing_key: the popover closing after it is no cancel */
   rename_done (iv, text);
   g_free (text);
   gtk_popover_popdown (GTK_POPOVER (iv->rename_popover));
}


static void
cb_rename_closed (GtkPopover *popover, IndexView *iv)
{
   if (!iv->editing_key) return;
   g_clear_pointer (&iv->editing_key, g_free);
   if (iv->count_again) start_count (iv);
}


static gboolean
idle_rename_focus_out (gpointer data)
{
   IndexView *iv = data;
   GtkRoot *root = gtk_widget_get_root (iv->tree);
   GtkWidget *focus = GTK_IS_ROOT (root) ? gtk_root_get_focus (root) : NULL;

   iv->rename_focus_id = 0;
   if (!iv->editing_key || !iv->rename_popover) return G_SOURCE_REMOVE;
   /* another program (to copy a name from it): the entry stays */
   if (GTK_IS_WINDOW (root) && !gtk_window_is_active (GTK_WINDOW (root)))
      return G_SOURCE_REMOVE;
   if (focus && (focus == iv->rename_entry || gtk_widget_is_ancestor (focus, iv->rename_entry)))
      return G_SOURCE_REMOVE;
   /* clicked elsewhere in the window: as Escape */
   gtk_popover_popdown (GTK_POPOVER (iv->rename_popover));
   return G_SOURCE_REMOVE;
}


static void
cb_rename_focus_out (GtkEventControllerFocus *ctrl, IndexView *iv)
{
   /* looked at after the window has said whether it is still active */
   if (!iv->rename_focus_id)
      iv->rename_focus_id = g_timeout_add (100, idle_rename_focus_out, iv);
}


static gboolean
cb_rename_key (GtkEventControllerKey *ctrl, guint keyval, guint code, GdkModifierType state,
               IndexView *iv)
{
   if (keyval == GDK_KEY_Escape) {
      gtk_popover_popdown (GTK_POPOVER (iv->rename_popover));
      return TRUE;
   }
   /* the text, not the window's Ctrl+A (select all the thumbnails) */
   if ((keyval == GDK_KEY_a || keyval == GDK_KEY_A) && (state & GDK_CONTROL_MASK)) {
      gtk_editable_select_region (GTK_EDITABLE (iv->rename_entry), 0, -1);
      return TRUE;
   }
   return FALSE;
}


/* a tag is renamed in an entry in a popover on its row */
static void
start_rename (IndexView *iv, const gchar *key)
{
   GtkTreeListRow *tree_row;
   GimvRow *row, *parent;
   guint pos;

   if (!key_is_renamable (iv, key)) return;
   if (!(row = find_key (iv->root, key))) return;
   if ((parent = gimv_row_get_parent (row)))
      gimv_tree_list_model_expand_to (iv->model, parent);
   if (!(tree_row = gimv_tree_list_model_get_row_for (iv->model, row, &pos))) return;
   g_object_unref (tree_row);
   g_free (iv->editing_key);
   iv->editing_key = g_strdup (key);
   gimv_list_view_set_focus_position (iv->tree, pos, TRUE);

   if (!iv->rename_popover) {
      GtkEventController *keys = gtk_event_controller_key_new ();
      iv->rename_entry = gtk_entry_new ();
      gtk_editable_set_width_chars (GTK_EDITABLE (iv->rename_entry), 24);
      g_signal_connect (iv->rename_entry, "activate", G_CALLBACK (cb_rename_activate), iv);
      gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_rename_key), iv);
      gtk_widget_add_controller (iv->rename_entry, keys);
      keys = gtk_event_controller_focus_new ();
      g_signal_connect (keys, "leave", G_CALLBACK (cb_rename_focus_out), iv);
      gtk_widget_add_controller (iv->rename_entry, keys);
      iv->rename_popover = gtk_popover_new ();
      /* not closed by a click on another program (a name copied from
       * it): closed by Enter, Escape and a click elsewhere here */
      gtk_popover_set_autohide (GTK_POPOVER (iv->rename_popover), FALSE);
      gtk_popover_set_child (GTK_POPOVER (iv->rename_popover), iv->rename_entry);
      gtk_popover_set_position (GTK_POPOVER (iv->rename_popover), GTK_POS_BOTTOM);
      /* on the scrolled window, as the menu */
      gtk_widget_set_parent (iv->rename_popover, iv->scroll);
      g_signal_connect (iv->rename_popover, "closed", G_CALLBACK (cb_rename_closed), iv);
   }
   {
      /* the name alone, not "Kyoto (12)" */
      const gchar *tag = key_person_tag (iv, key);
      GdkRectangle rect = { 0, 0, 1, 1 };
      graphene_rect_t area;
      graphene_point_t pt, out;

      if (gimv_list_view_get_row_area (iv->tree, pos, &area)) {
         pt = GRAPHENE_POINT_INIT (area.origin.x, area.origin.y);
         if (gtk_widget_compute_point (iv->tree, iv->scroll, &pt, &out)) {
            rect.x = (gint) out.x;
            rect.y = (gint) out.y;
         }
         rect.width = MAX ((gint) area.size.width, 1);
         rect.height = MAX ((gint) area.size.height, 1);
      }
      gtk_popover_set_pointing_to (GTK_POPOVER (iv->rename_popover), &rect);
      gtk_editable_set_text (GTK_EDITABLE (iv->rename_entry), tag ? tag : key);
      gtk_popover_popup (GTK_POPOVER (iv->rename_popover));
      gtk_widget_grab_focus (iv->rename_entry);
      gtk_editable_select_region (GTK_EDITABLE (iv->rename_entry), 0, -1);
   }
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
   gboolean person;    /* a tag of people */
} RenameAsk;


static void
cb_person_tag_renamed (GObject *source, GAsyncResult *res, gpointer data)
{
   GtkWidget *tree = data;   /* held until now */
   GError *error = NULL;

   if (!gimv_index_faces_edit_finish (res, NULL, &error)) {
      GtkAlertDialog *dialog = gtk_alert_dialog_new ("%s", _("The tag could not be renamed."));
      GtkRoot *root = gtk_widget_get_root (tree);
      gtk_alert_dialog_set_detail (dialog, error ? error->message : NULL);
      gtk_alert_dialog_show (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL);
      g_object_unref (dialog);
      g_clear_error (&error);
   }
   g_object_unref (tree);
}


/* every person with the tag gets the new one (in a worker thread) */
static void
do_rename_person_tag (IndexView *iv, const gchar *old, const gchar *new)
{
   gchar *both = g_strconcat (old, "\n", new, NULL);
   gimv_index_faces_edit_async (GIMV_INDEX_FACES_RENAME_PERSON_TAG, NULL, both, 0,
                                cb_person_tag_renamed, g_object_ref (iv->tree));
   g_free (both);
}


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

   if (button == 1 && !g_cancellable_is_cancelled (ask->cancel)) {
      if (ask->person)
         do_rename_person_tag (ask->iv, ask->old, ask->new);
      else
         do_rename (ask->iv, ask->old, ask->new);
   }
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
rename_done (IndexView *iv, const gchar *text)
{
   gchar *old = g_steal_pointer (&iv->editing_key);
   gchar *new = g_strstrip (g_strdup (text ? text : ""));
   const gchar *ptag = key_person_tag (iv, old);

   if (ptag) {
      /* a tag of people: kept in the index only, renamed there */
      if (!*new || !strcmp (ptag, new)) {
         /* nothing */
      } else if (strpbrk (new, "\t\n")) {
         alert (iv, _("A tag cannot have a tab or a new line in it."), NULL);
      } else {
         GPtrArray *all = gimv_index_faces_get_all_person_tags ();
         gboolean there = g_ptr_array_find_with_equal_func (all, new, g_str_equal, NULL);
         g_ptr_array_unref (all);
         if (there) {
            GtkAlertDialog *dialog = gtk_alert_dialog_new (_("The tag \"%s\" is there already."),
                                                           new);
            const gchar *buttons[] = { _("_Cancel"), _("_Merge Them"), NULL };
            gchar *detail = g_strdup_printf (_("The people with \"%s\" get \"%s\" instead, "
                                               "and \"%s\" goes."), ptag, new, ptag);
            RenameAsk *ask = g_new0 (RenameAsk, 1);
            GtkRoot *root = gtk_widget_get_root (iv->tree);

            ask->iv = iv;
            ask->cancel = g_object_ref (iv->cancel);
            ask->old = g_strdup (ptag);
            ask->new = g_strdup (new);
            ask->person = TRUE;
            gtk_alert_dialog_set_detail (dialog, detail);
            gtk_alert_dialog_set_buttons (dialog, buttons);
            gtk_alert_dialog_set_cancel_button (dialog, 0);
            gtk_alert_dialog_set_default_button (dialog, 1);
            gtk_alert_dialog_choose (dialog, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                     NULL, cb_merge_answer, ask);
            g_object_unref (dialog);
            g_free (detail);
         } else {
            do_rename_person_tag (iv, ptag, new);
         }
      }
   } else if (!old || !*new || !strcmp (old, new)) {
      /* nothing */
   } else if (strchr (new, ',') || strpbrk (new, "\t\n")) {
      /* the comment files keep the tags separated by commas */
      alert (iv, _("A tag cannot have a comma in it."), NULL);
   } else if (g_str_has_prefix (new, NAME_TAG)) {
      alert (iv, _("Tags starting with \"person/\" are the names of faces."),
             _("They are written by GImageView from the names given in the faces window."));
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
row_copy_text (IndexView *iv, GimvRow *row)
{
   const gchar *key = gimv_row_peek_string (row, COL_KEY);
   gint kind = gimv_row_peek_int (row, COL_KIND);

   if (!key || kind == KIND_ROOT || kind == KIND_GROUP || kind == KIND_HEADER)
      return NULL;
   if (iv->type == GIMV_INDEX_TAB_PERSON)
      return g_strdup (gimv_row_peek_string (row, COL_TITLE));
   if (iv->type == GIMV_INDEX_TAB_TAG && key[0] == AUTO_KEY_MARK)
      return g_strdup (auto_tag_value (key + 1, NULL));
   if (iv->type == GIMV_INDEX_TAB_TAG && g_str_has_prefix (key, NAME_TAG))
      return g_strdup (key + NAME_TAG_LEN);
   return g_strdup (key);
}


/******************************************************************************
 *  Ctrl+F: a row by (a part of) its name, in the rows there are (the days
 *  of a month not opened yet are not looked at)
 ******************************************************************************/

/* the rows below parent (NULL if none) */
static GListModel *
row_children (GimvRow *parent)
{
   GListStore *c = gimv_row_get_children (parent, FALSE);
   return c && g_list_model_get_n_items (G_LIST_MODEL (c)) ? G_LIST_MODEL (c) : NULL;
}


/* the last row below row, all levels (row itself if none) */
static GimvRow *
row_last (GimvRow *row)
{
   GListModel *c;
   while ((c = row_children (row)))
      row = gimv_row_store_peek (c, g_list_model_get_n_items (c) - 1);
   return row;
}


/* the row after (forward) or before "row" in the order shown, all levels
 * (also those not opened); NULL at the end */
static GimvRow *
tree_step (IndexView *iv, GimvRow *row, gboolean forward)
{
   GimvRow *parent;
   GListModel *siblings;
   guint pos;

   if (forward) {
      GListModel *c = row_children (row);
      if (c) return gimv_row_store_peek (c, 0);
      for (;;) {
         parent = gimv_row_get_parent (row);
         siblings = parent ? G_LIST_MODEL (gimv_row_get_children (parent, FALSE))
                           : G_LIST_MODEL (iv->store);
         if (gimv_row_store_find (siblings, row, &pos)
             && pos + 1 < g_list_model_get_n_items (siblings))
            return gimv_row_store_peek (siblings, pos + 1);
         if (!parent) return NULL;
         row = parent;
      }
   }
   parent = gimv_row_get_parent (row);
   siblings = parent ? G_LIST_MODEL (gimv_row_get_children (parent, FALSE))
                     : G_LIST_MODEL (iv->store);
   if (gimv_row_store_find (siblings, row, &pos) && pos > 0)
      return row_last (gimv_row_store_peek (siblings, pos - 1));
   return parent;
}


static gboolean
row_matches (GimvRow *row, const gchar *folded)
{
   const gchar *label = gimv_row_peek_string (row, COL_LABEL);
   gchar *norm, *fold;
   gboolean match = FALSE;

   if (label) {
      norm = g_utf8_normalize (label, -1, G_NORMALIZE_ALL);
      fold = norm ? g_utf8_casefold (norm, -1) : NULL;
      match = fold && strstr (fold, folded);
      g_free (fold);
      g_free (norm);
   }
   return match;
}


/* from the row selected (itself too when "again" is FALSE) on, around at
 * the end; the row found is selected and shown */
static gboolean
search_row (IndexView *iv, gboolean forward, gboolean again)
{
   const gchar *text = gtk_editable_get_text (GTK_EDITABLE (iv->search_entry));
   gchar *norm, *folded;
   GimvRow *start, *row;
   gboolean found = FALSE;

   if (!text || !*text) return FALSE;
   norm = g_utf8_normalize (text, -1, G_NORMALIZE_ALL);
   folded = norm ? g_utf8_casefold (norm, -1) : NULL;
   g_free (norm);
   if (!folded) return FALSE;

   start = gimv_list_view_get_selected_row (iv->tree);
   if (!start) start = iv->root;
   row = start;
   if (!again && row_matches (row, folded)) {
      found = TRUE;
   } else {
      for (;;) {
         row = tree_step (iv, row, forward);
         /* around: from the first (the last) row */
         if (!row) row = forward ? iv->root : row_last (iv->root);
         if (row == start) break;
         if (row_matches (row, folded)) { found = TRUE; break; }
      }
   }
   g_free (folded);

   if (found) {
      GtkTreeListRow *tree_row;
      GimvRow *parent = gimv_row_get_parent (row);
      guint pos;

      if (parent) gimv_tree_list_model_expand_to (iv->model, parent);
      if ((tree_row = gimv_tree_list_model_get_row_for (iv->model, row, &pos))) {
         gimv_list_view_scroll_to (iv->tree, pos, TRUE, FALSE);
         g_object_unref (tree_row);
      }
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
   gchar *key = row_key (gimv_list_view_get_selected_row (iv->tree));

   if (key) {
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
   GimvRow *row;
   gchar *text;

   /* Ctrl+F here: a row by its name (not the window's "open a file") */
   if ((keyval == GDK_KEY_f || keyval == GDK_KEY_F)
       && (state & GDK_MODIFIER_MASK) == GDK_CONTROL_MASK)
   {
      search_start (iv);
      return TRUE;
   }
   if (!(row = gimv_list_view_get_selected_row (iv->tree))) return FALSE;

   if (keyval == GDK_KEY_F2 && !(state & GDK_MODIFIER_MASK)) {
      start_rename (iv, gimv_row_peek_string (row, COL_KEY));
      return TRUE;
   }
   if ((keyval == GDK_KEY_c || keyval == GDK_KEY_C) && (state & GDK_CONTROL_MASK)) {
      text = row_copy_text (iv, row);
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
   if (iv->bookmark_watch) gimv_bookmark_remove_watch (iv->bookmark_watch);
   if (iv->open_id) g_source_remove (iv->open_id);
   if (iv->rename_id) g_source_remove (iv->rename_id);
   if (iv->popover) gtk_widget_unparent (iv->popover);
   if (iv->rename_focus_id) g_source_remove (iv->rename_focus_id);
   if (iv->rename_popover) gtk_widget_unparent (iv->rename_popover);
   set_result_tab (iv, NULL);
   g_clear_object (&iv->model);
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
   case GIMV_INDEX_TAB_BOOKMARK: return _("Bookmarks");
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
   case GIMV_INDEX_TAB_BOOKMARK: return "starred-symbolic";
   default:                    return "x-office-calendar-symbolic";
   }
}


static GtkWidget *
tree_cell_setup (gpointer data)
{
   static gboolean css = FALSE;

   if (!css) {
      GtkCssProvider *provider = gtk_css_provider_new ();
      gtk_css_provider_load_from_string (provider,
         "label.gimv-auto-tag { color: " AUTO_TAG_COLOR "; }\n"
         "row:selected label.gimv-auto-tag { color: inherit; }\n");
      gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                  GTK_STYLE_PROVIDER (provider),
                                                  GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      g_object_unref (provider);
      css = TRUE;
   }

   GtkWidget *label = gtk_label_new (NULL);
   gtk_label_set_xalign (GTK_LABEL (label), 0.0);
   return label;
}


static void
tree_cell_bind (GtkWidget *cell, GimvRow *row, GtkListItem *item, gpointer data)
{
   GtkWidget *label = gimv_cell_get_content (cell);
   GtkWidget *expander = gtk_widget_get_parent (cell);
   GListStore *children = gimv_row_get_children (row, FALSE);

   gtk_label_set_text (GTK_LABEL (label), gimv_row_peek_string (row, COL_LABEL));
   /* the tags put by rules have a color of their own (not when selected) */
   if (gimv_row_peek_string (row, COL_COLOR))
      gtk_widget_add_css_class (label, "gimv-auto-tag");
   else
      gtk_widget_remove_css_class (label, "gimv-auto-tag");
   /* no arrow on a row without rows below it (the root has one always) */
   if (GTK_IS_TREE_EXPANDER (expander)) {
      /* the names line up, with an arrow or not (as GtkTreeView) */
      gtk_tree_expander_set_indent_for_icon (GTK_TREE_EXPANDER (expander), TRUE);
   }
   if (GTK_IS_TREE_EXPANDER (expander))
      gtk_tree_expander_set_hide_expander (GTK_TREE_EXPANDER (expander),
                                           gimv_row_peek_int (row, COL_KIND) != KIND_ROOT
                                           && !(children && g_list_model_get_n_items (G_LIST_MODEL (children))));
}


static GtkWidget *
index_view_new (GimvThumbWin *tw, GimvIndexTabType type)
{
   static const GActionEntry actions[] = {
      { "open",         cb_action_open, NULL, NULL, NULL },
      { "open-new-tab", cb_action_open, NULL, NULL, NULL },
      { "faces",        cb_action_faces, NULL, NULL, NULL },
      { "rename",       cb_action_rename, NULL, NULL, NULL },
      { "no-folder-tag", cb_action_no_folder_tag, NULL, NULL, NULL },
   };
   IndexView *iv = g_new0 (IndexView, 1);
   GtkGesture *click;
   GSimpleActionGroup *group;

   iv->type = type;
   iv->tw = tw;
   iv->store = gimv_row_store_new ();
   iv->root = row_new (NULL, tab_title (type), KIND_ROOT, NULL, NULL, NULL);
   /* expandable from the start */
   gimv_row_get_children (iv->root, TRUE);
   g_list_store_append (iv->store, iv->root);
   g_object_unref (iv->root);
   iv->model = gimv_tree_list_model_new (G_LIST_MODEL (iv->store));
   iv->cancel = g_cancellable_new ();

   iv->tree = gimv_list_view_new (G_LIST_MODEL (g_object_ref (iv->model)), GTK_SELECTION_SINGLE,
                                  gimv_cell_factory_new (tree_cell_setup, tree_cell_bind,
                                                         NULL, NULL, TRUE));
   gtk_widget_set_name (iv->tree, "IndexTree");
   {
      GtkEventController *keys = gtk_event_controller_key_new ();
      g_signal_connect (keys, "key-pressed", G_CALLBACK (cb_tree_key), iv);
      gtk_widget_add_controller (iv->tree, keys);
   }
   /* Enter (and a double click) */
   g_signal_connect (iv->tree, "activate", G_CALLBACK (cb_activate), iv);
   expand_row (iv, iv->root, TRUE);

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
   if (type == GIMV_INDEX_TAB_BOOKMARK) {
      iv->bookmark_watch = gimv_bookmark_add_watch (cb_bookmarks_changed, iv);
      /* "Today" is another day when the tab is shown again */
      g_signal_connect_swapped (iv->scroll, "map", G_CALLBACK (start_count), iv);
   } else {
      iv->watch_id = gimv_index_add_watch (cb_index_changed, iv);
   }
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
      /* for gimv_index_open_tag () */
      g_object_set_data (G_OBJECT (box), "gimv-index-view", iv);
      return box;
   }
}


/******************************************************************************
 *  the "Date" and "Tag" tabs next to the directory view
 ******************************************************************************/

static const gchar *tab_keys[] = { "gimv-index-date-tab", "gimv-index-tag-tab",
                                   "gimv-index-person-tab", "gimv-index-bookmark-tab" };


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
   /* any image can have one: with the index or not */
   gimv_index_tab_open (tw, GIMV_INDEX_TAB_BOOKMARK);
}


/******************************************************************************
 *  the tags of one image (the "Data" tab of the comments)
 ******************************************************************************/

static void
add_strings (sqlite3 *db, const gchar *sql, gint64 file_id, const gchar *prefix,
             GPtrArray *keys)
{
   sqlite3_stmt *st;

   if (sqlite3_prepare_v2 (db, sql, -1, &st, NULL) != SQLITE_OK) return;
   sqlite3_bind_int64 (st, 1, file_id);
   while (sqlite3_step (st) == SQLITE_ROW) {
      gchar *key = g_strconcat (prefix, (const gchar *) sqlite3_column_text (st, 0), NULL);
      if (!g_ptr_array_find_with_equal_func (keys, key, g_str_equal, NULL))
         g_ptr_array_add (keys, key);
      else
         g_free (key);
   }
   sqlite3_finalize (st);
}


static void
thread_get_tags (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
   const gchar *path = data;
   gchar *dbfile = gimv_index_db_default_path ();
   gchar *dir = g_path_get_dirname (path), *name = g_path_get_basename (path);
   sqlite3 *db = NULL;
   sqlite3_stmt *st;
   gint64 file_id = 0;
   GPtrArray *keys = NULL;

   if (gimv_index_db_open (dbfile, FALSE, &db, NULL) == GIMV_INDEX_DB_OK
       && sqlite3_prepare_v2 (db, "SELECT f.id FROM files f JOIN dirs d ON d.id = f.dir_id "
                                  "WHERE d.path = ?1 AND f.name = ?2", -1, &st, NULL) == SQLITE_OK)
   {
      sqlite3_bind_text (st, 1, dir, -1, SQLITE_STATIC);
      sqlite3_bind_text (st, 2, name, -1, SQLITE_STATIC);
      if (sqlite3_step (st) == SQLITE_ROW) file_id = sqlite3_column_int64 (st, 0);
      sqlite3_finalize (st);
   }
   if (file_id && !g_cancellable_is_cancelled (cancel)) {
      keys = g_ptr_array_new_with_free_func (g_free);
      /* put by a person (the names apart, after them) */
      /* put by a person, and the keywords of other programs as they are */
      add_strings (db, "SELECT t.name FROM file_tags ft JOIN tags t ON t.id = ft.tag_id "
                       "WHERE ft.file_id = ?1 AND ft.auto = 0 "
                       "AND NOT (t.name >= 'person/' AND t.name < 'person0') "
                       "UNION SELECT substr (t.name, 9) FROM file_tags ft JOIN tags t "
                       "ON t.id = ft.tag_id WHERE ft.file_id = ?1 AND ft.auto = 1 "
                       "AND t.name >= 'keyword/' AND t.name < 'keyword0' ORDER BY 1",
                   file_id, "", keys);
      /* the names: given to the faces, and in the comment file (one not
       * given back to a face yet) */
      add_strings (db, "SELECT p.name FROM faces fa JOIN persons p ON p.id = fa.person_id "
                       "WHERE fa.file_id = ?1 UNION SELECT substr (t.name, 8) FROM file_tags ft "
                       "JOIN tags t ON t.id = ft.tag_id WHERE ft.file_id = ?1 AND ft.auto = 0 "
                       "AND t.name >= 'person/' AND t.name < 'person0' ORDER BY 1",
                   file_id, NAME_TAG, keys);
      add_strings (db, "SELECT DISTINCT pt.tag FROM faces fa JOIN person_tags pt "
                       "ON pt.person_id = fa.person_id WHERE fa.file_id = ?1 ORDER BY 1",
                   file_id, ",person/", keys);
      add_strings (db, "SELECT t.name FROM file_tags ft JOIN tags t ON t.id = ft.tag_id "
                       "WHERE ft.file_id = ?1 AND " GIMV_INDEX_TAG_AUTO_SQL " ORDER BY t.name",
                   file_id, ",", keys);
   }
   if (db) sqlite3_close (db);
   g_free (dbfile);
   g_free (dir);
   g_free (name);
   g_task_return_pointer (task, keys, keys ? (GDestroyNotify) g_ptr_array_unref : NULL);
}


void
gimv_index_get_tags_async (const gchar *path, GCancellable *cancel,
                           GAsyncReadyCallback callback, gpointer data)
{
   GTask *task = g_task_new (NULL, cancel, callback, data);

   g_task_set_task_data (task, g_strdup (path), g_free);
   g_task_run_in_thread (task, thread_get_tags);
   g_object_unref (task);
}


GPtrArray *
gimv_index_get_tags_finish (GAsyncResult *res)
{
   return g_task_propagate_pointer (G_TASK (res), NULL);
}


gint
gimv_index_tag_kind (const gchar *key)
{
   if (!key) return 0;
   key = gimv_index_plain_key (key);
   if (key[0] == AUTO_KEY_MARK)
      return g_str_has_prefix (key + 1, "person/") ? 2 : 3;
   return g_str_has_prefix (key, NAME_TAG) ? 1 : 0;
}


gchar *
gimv_index_tag_label (const gchar *key)
{
   gint kind = gimv_index_tag_kind (key);

   if (kind == 0) return g_strdup (gimv_index_plain_key (key));
   if (kind == 1) return g_strdup (key + NAME_TAG_LEN);
   return tag_title (key);
}


void
gimv_index_open_tag (GimvThumbWin *tw, const gchar *key)
{
   GtkWidget *page;
   IndexView *iv;

   g_return_if_fail (GIMV_IS_THUMB_WIN (tw) && key);
   if (!(page = gimv_index_tab_get_page (tw, GIMV_INDEX_TAB_TAG))) {
      gimv_index_tab_open (tw, GIMV_INDEX_TAB_TAG);
      page = gimv_index_tab_get_page (tw, GIMV_INDEX_TAB_TAG);
   }
   iv = page ? g_object_get_data (G_OBJECT (page), "gimv-index-view") : NULL;
   if (iv) open_key (iv, key, FALSE);
}

