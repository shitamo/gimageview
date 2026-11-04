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
 * The indexer (src/indexer/gimv_indexer.c), run as gimv runs it, on
 * images and comment files made here in a temporary HOME: the date of
 * each image (EXIF, the file name, the mtime), the tags of the comment
 * files (X-IMG-Tags:), the tags of the rules (folder, camera, shape), and
 * files added, moved and removed while it runs (inotify) and while it does
 * not.  The database it writes is read here.
 */

#include <gio/gio.h>
#include <sqlite3.h>
#include <unistd.h>

#include "gimv_test.h"
#ifdef HAVE_EXIF
#  include "gimv_test_exif.h"
#endif

typedef struct {
   GSubprocess      *proc;
   GDataInputStream *out;
   GOutputStream    *in;
   GPtrArray        *lines;      /* all it said */
   gchar            *db;
   gint64            deadline;   /* killed when waiting for it so long */
   gboolean          done;
   GThread          *watchdog;
   GMutex            lock;
} Indexer;

static gchar *home;          /* HOME of the indexer: ~/.gimv/comment */
static gchar *tmp;           /* the test directory */
static guint  serial = 0;


/******************************************************************************
 *  files
 ******************************************************************************/

/* a PNG of w x h (its header has the size) */
static void
write_png (const gchar *path, gint w, gint h)
{
   GdkPixbuf *p = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, w, h);
   GError *error = NULL;
   gchar *dir = g_path_get_dirname (path);

   g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
   g_free (dir);
   gdk_pixbuf_fill (p, 0x204060ff);
   gdk_pixbuf_save (p, path, "png", &error, NULL);
   g_assert_no_error (error);
   g_object_unref (p);
}


static gchar *
comment_file (const gchar *image)
{
   return g_strconcat (home, "/.gimv/comment", image, ".txt", NULL);
}


static void
write_comment (const gchar *image, const gchar *text)
{
   gchar *c = comment_file (image);
   gimv_test_write_string (c, text);
   g_free (c);
}


/* a new root directory for one test */
static gchar *
new_root (void)
{
   gchar *name = g_strdup_printf ("root%u", serial++);
   gchar *root = g_build_filename (tmp, name, NULL);

   g_assert_cmpint (g_mkdir_with_parents (root, 0700), ==, 0);
   g_free (name);
   return root;
}


static gchar *
in (const gchar *root, const gchar *rel)
{
   return g_build_filename (root, rel, NULL);
}


/******************************************************************************
 *  the indexer
 ******************************************************************************/

static gpointer
watchdog (gpointer data)
{
   Indexer *ix = data;

   for (;;) {
      gboolean stop, late;
      g_usleep (100 * 1000);
      g_mutex_lock (&ix->lock);
      stop = ix->done;
      late = ix->deadline && g_get_monotonic_time () > ix->deadline;
      g_mutex_unlock (&ix->lock);
      if (stop) return NULL;
      if (late) {
         g_printerr ("# the indexer took too long: killed\n");
         g_subprocess_force_exit (ix->proc);
         return NULL;
      }
   }
}


static void
set_deadline (Indexer *ix, gint seconds)
{
   g_mutex_lock (&ix->lock);
   ix->deadline = seconds ? g_get_monotonic_time () + seconds * G_USEC_PER_SEC : 0;
   g_mutex_unlock (&ix->lock);
}


/* gimv-indexer --db <tmp>/<db> [args...] root */
static Indexer *
ix_start (const gchar *db, const gchar *root, const gchar *const *args)
{
   const gchar *exe = g_getenv ("GIMV_TEST_INDEXER");
   GSubprocessLauncher *l;
   GPtrArray *argv = g_ptr_array_new ();
   Indexer *ix = g_new0 (Indexer, 1);
   GError *error = NULL;
   guint i;

   g_assert_nonnull (exe);
   ix->db = g_build_filename (tmp, db, NULL);
   g_ptr_array_add (argv, (gpointer) exe);
   g_ptr_array_add (argv, (gpointer) "--db");
   g_ptr_array_add (argv, ix->db);
   for (i = 0; args && args[i]; i++) g_ptr_array_add (argv, (gpointer) args[i]);
   g_ptr_array_add (argv, (gpointer) root);
   g_ptr_array_add (argv, NULL);

   l = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE);
   g_subprocess_launcher_setenv (l, "HOME", home, TRUE);
   ix->proc = g_subprocess_launcher_spawnv (l, (const gchar * const *) argv->pdata, &error);
   g_assert_no_error (error);
   g_object_unref (l);
   g_ptr_array_unref (argv);

   ix->out = g_data_input_stream_new (g_subprocess_get_stdout_pipe (ix->proc));
   ix->in = g_subprocess_get_stdin_pipe (ix->proc);
   ix->lines = g_ptr_array_new_with_free_func (g_free);
   g_mutex_init (&ix->lock);
   ix->watchdog = g_thread_new ("watchdog", watchdog, ix);
   return ix;
}


static const gchar *
ix_read_line (Indexer *ix)
{
   GError *error = NULL;
   gchar *line = g_data_input_stream_read_line (ix->out, NULL, NULL, &error);

   g_assert_no_error (error);
   if (!line) g_error ("the indexer ended (or was killed)");
   if (g_test_verbose ()) g_printerr ("# indexer: %s\n", line);
   g_ptr_array_add (ix->lines, line);
   return line;
}


/* the lines until one that starts with prefix (returned) */
static const gchar *
ix_wait (Indexer *ix, const gchar *prefix)
{
   const gchar *line;

   set_deadline (ix, 60);
   do {
      line = ix_read_line (ix);
      if (g_str_has_prefix (line, "error")) g_error ("indexer: %s", line);
   } while (!g_str_has_prefix (line, prefix));
   set_deadline (ix, 0);
   return line;
}


static gboolean
ix_said (Indexer *ix, const gchar *line)
{
   guint i;
   for (i = 0; i < ix->lines->len; i++)
      if (!strcmp (g_ptr_array_index (ix->lines, i), line)) return TRUE;
   return FALSE;
}


static void
ix_send (Indexer *ix, const gchar *cmd)
{
   GError *error = NULL;
   gchar *s = g_strconcat (cmd, "\n", NULL);

   g_output_stream_write_all (ix->in, s, strlen (s), NULL, NULL, &error);
   g_assert_no_error (error);
   g_output_stream_flush (ix->in, NULL, &error);
   g_assert_no_error (error);
   g_free (s);
}


static void
ix_stop (Indexer *ix)
{
   GError *error = NULL;

   ix_send (ix, "quit");
   set_deadline (ix, 30);
   g_subprocess_wait (ix->proc, NULL, &error);
   g_assert_no_error (error);
   g_assert_true (g_subprocess_get_if_exited (ix->proc));
   g_assert_cmpint (g_subprocess_get_exit_status (ix->proc), ==, 0);

   g_mutex_lock (&ix->lock);
   ix->done = TRUE;
   g_mutex_unlock (&ix->lock);
   g_thread_join (ix->watchdog);
   g_mutex_clear (&ix->lock);
   g_object_unref (ix->out);
   g_object_unref (ix->proc);
   g_ptr_array_unref (ix->lines);
   g_free (ix->db);
   g_free (ix);
}


/******************************************************************************
 *  the database
 ******************************************************************************/

static sqlite3 *
db_open (Indexer *ix)
{
   sqlite3 *db = NULL;
   g_assert_cmpint (sqlite3_open_v2 (ix->db, &db, SQLITE_OPEN_READONLY, NULL), ==, SQLITE_OK);
   sqlite3_busy_timeout (db, 5000);
   return db;
}


/* the id of the row of the file (0: none) */
static gint64
file_id (sqlite3 *db, const gchar *path)
{
   gchar *dir = g_path_get_dirname (path), *name = g_path_get_basename (path);
   sqlite3_stmt *st;
   gint64 id = 0;

   g_assert_cmpint (sqlite3_prepare_v2 (db, "SELECT f.id FROM files f JOIN dirs d ON d.id = f.dir_id "
                                            "WHERE d.path = ? AND f.name = ?", -1, &st, NULL), ==, SQLITE_OK);
   sqlite3_bind_text (st, 1, dir, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text (st, 2, name, -1, SQLITE_TRANSIENT);
   if (sqlite3_step (st) == SQLITE_ROW) id = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   g_free (dir);
   g_free (name);
   return id;
}


static gboolean
has_file (Indexer *ix, const gchar *path)
{
   sqlite3 *db = db_open (ix);
   gboolean ret = file_id (db, path) != 0;
   sqlite3_close (db);
   return ret;
}


/* "date src" of the file */
static gchar *
file_date (Indexer *ix, const gchar *path)
{
   sqlite3 *db = db_open (ix);
   sqlite3_stmt *st;
   gchar *ret = NULL;
   gint64 id = file_id (db, path);

   if (!id) g_error ("%s is not in the index", path);
   sqlite3_prepare_v2 (db, "SELECT date, date_src FROM files WHERE id = ?", -1, &st, NULL);
   sqlite3_bind_int64 (st, 1, id);
   g_assert_cmpint (sqlite3_step (st), ==, SQLITE_ROW);
   ret = g_strdup_printf ("%s %d", sqlite3_column_text (st, 0), sqlite3_column_int (st, 1));
   sqlite3_finalize (st);
   sqlite3_close (db);
   return ret;
}


/* the tags of the file, "name" (put by a person) or "name*" (by a rule),
 * sorted, ", " between */
static gchar *
file_tags (Indexer *ix, const gchar *path)
{
   sqlite3 *db = db_open (ix);
   sqlite3_stmt *st;
   GString *s = g_string_new (NULL);
   gint64 id = file_id (db, path);

   if (!id) g_error ("%s is not in the index", path);
   sqlite3_prepare_v2 (db, "SELECT t.name, ft.auto FROM file_tags ft JOIN tags t ON t.id = ft.tag_id "
                           "WHERE ft.file_id = ? ORDER BY t.name, ft.auto", -1, &st, NULL);
   sqlite3_bind_int64 (st, 1, id);
   while (sqlite3_step (st) == SQLITE_ROW) {
      if (s->len) g_string_append (s, ", ");
      g_string_append (s, (const gchar *) sqlite3_column_text (st, 0));
      if (sqlite3_column_int (st, 1)) g_string_append_c (s, '*');
   }
   sqlite3_finalize (st);
   sqlite3_close (db);
   return g_string_free (s, FALSE);
}


static gint64
count (Indexer *ix, const gchar *sql)
{
   sqlite3 *db = db_open (ix);
   sqlite3_stmt *st;
   gint64 n = -1;

   g_assert_cmpint (sqlite3_prepare_v2 (db, sql, -1, &st, NULL), ==, SQLITE_OK);
   if (sqlite3_step (st) == SQLITE_ROW) n = sqlite3_column_int64 (st, 0);
   sqlite3_finalize (st);
   sqlite3_close (db);
   return n;
}


#define assert_date(ix, path, expected)                                \
   G_STMT_START {                                                      \
      gchar *_d = file_date ((ix), (path));                            \
      g_assert_cmpstr (_d, ==, (expected));                            \
      g_free (_d);                                                     \
   } G_STMT_END

#define assert_tags(ix, path, expected)                                \
   G_STMT_START {                                                      \
      gchar *_t = file_tags ((ix), (path));                            \
      g_assert_cmpstr (_t, ==, (expected));                            \
      g_free (_t);                                                     \
   } G_STMT_END


/* while watching: the lines until cond holds (each change ends with
 * "changed" and "idle") */
typedef gboolean (*Cond) (Indexer *ix, gconstpointer data);

static void
ix_wait_until (Indexer *ix, Cond cond, gconstpointer data)
{
   while (!cond (ix, data))
      ix_wait (ix, "idle");
}

static gboolean
cond_has (Indexer *ix, gconstpointer path)
{
   return has_file (ix, path);
}

static gboolean
cond_gone (Indexer *ix, gconstpointer path)
{
   return !has_file (ix, path);
}


/******************************************************************************
 *  the date
 ******************************************************************************/

static void
test_dates (void)
{
   gchar *root = new_root ();
   static const struct { const gchar *name; const gchar *date; } by_name[] = {
      { "IMG_20190503_101500.png",                "2019-05-03 10:15:00 1" },
      { "2018-01-02 03.04.05.png",                "2018-01-02 03:04:05 1" },
      { "Screenshot 2020-12-31 at 23.59.58.png",  "2020-12-31 23:59:58 1" },
      { "DSC20170203.png",                        "2017-02-03 00:00:00 1" },
      { "photo_2016.07.08.png",                   "2016-07-08 00:00:00 1" },
      /* not dates: from the mtime (2017-07-14 02:40:00 UTC) */
      { "IMG_20191340_000000.png",                "2017-07-14 02:40:00 0" },
      { "IMG_20190230.png",                       "2017-07-14 02:40:00 0" },
      { "IMG_20990101.png",                       "2017-07-14 02:40:00 0" },   /* future */
      { "x120190503.png",                         "2017-07-14 02:40:00 0" },
      { "plain.png",                              "2017-07-14 02:40:00 0" },
   };
   const gchar *args[] = { "--auto=folder", NULL };
   Indexer *ix;
   guint i;

   for (i = 0; i < G_N_ELEMENTS (by_name); i++) {
      gchar *p = in (root, by_name[i].name);
      write_png (p, 4, 4);
      gimv_test_set_mtime (p, 1500000000);
      g_free (p);
   }
#ifdef HAVE_EXIF
   {
      gchar *p = in (root, "exif.jpg");
      gimv_test_write_jpeg (p, 8, 8, 0, "2015:06:07 08:09:10", NULL, NULL);
      gimv_test_set_mtime (p, 1500000000);
      g_free (p);
      /* EXIF before the file name */
      p = in (root, "IMG_20190503_101500.jpg");
      gimv_test_write_jpeg (p, 8, 8, 1, "2016:01:01 00:00:01", NULL, NULL);
      g_free (p);
      /* a broken EXIF date: the file name, then the mtime */
      p = in (root, "IMG_20140101.jpg");
      gimv_test_write_jpeg (p, 8, 8, 0, "0000:00:00 00:00:00", NULL, NULL);
      g_free (p);
      p = in (root, "nodate.jpg");
      gimv_test_write_jpeg (p, 8, 8, 0, "2015:13:07 08:09:10", NULL, NULL);
      gimv_test_set_mtime (p, 1500000000);
      g_free (p);
   }
#endif

   ix = ix_start ("dates.db", root, args);
   ix_wait (ix, "idle");

   for (i = 0; i < G_N_ELEMENTS (by_name); i++) {
      gchar *p = in (root, by_name[i].name);
      assert_date (ix, p, by_name[i].date);
      g_free (p);
   }
#ifdef HAVE_EXIF
   {
      gchar *p = in (root, "exif.jpg");
      assert_date (ix, p, "2015-06-07 08:09:10 2");
      g_free (p);
      p = in (root, "IMG_20190503_101500.jpg");
      assert_date (ix, p, "2016-01-01 00:00:01 2");
      g_free (p);
      p = in (root, "IMG_20140101.jpg");
      assert_date (ix, p, "2014-01-01 00:00:00 1");
      g_free (p);
      p = in (root, "nodate.jpg");
      assert_date (ix, p, "2017-07-14 02:40:00 0");
      g_free (p);
   }
#endif
   g_assert_cmpint (count (ix, "SELECT count(*) FROM files WHERE stage < 2"), ==, 0);
   ix_stop (ix);
   g_free (root);
}


/******************************************************************************
 *  the tags of the comment files
 ******************************************************************************/

static void
test_manual_tags (void)
{
   gchar *root = new_root ();
   gchar *a = in (root, "a.png"), *b = in (root, "b.png"), *c = in (root, "c.png");
   gchar *d = in (root, "sub/d.png"), *e = in (root, "e.png"), *cmd;
   const gchar *args[] = { "--auto=folder", NULL };
   Indexer *ix;

   write_png (a, 4, 4);
   write_png (b, 4, 4);
   write_png (c, 4, 4);
   write_png (d, 4, 4);
   write_png (e, 4, 4);
   write_comment (a, "Comment: a trip\nX-IMG-Tags: trip, family , trip\n\n"
                     "X-IMG-Tags: the note, not a tag\n");
   write_comment (b, "X-IMG-Tags: one\nX-IMG-Tags: two,\n");
   write_comment (d, "X-IMG-Tags:\n");
   write_comment (e, "Comment: x-img-tags: none here\n");
   /* a comment file of an image that is not there */
   {
      gchar *gone = in (root, "gone.png");
      write_comment (gone, "X-IMG-Tags: ghost\n");
      g_free (gone);
   }

   ix = ix_start ("manual.db", root, args);
   ix_wait (ix, "idle");
   assert_tags (ix, a, "family, trip");
   assert_tags (ix, b, "one, two");
   assert_tags (ix, c, "");
   assert_tags (ix, d, "folder/sub*");
   assert_tags (ix, e, "");
   g_assert_cmpint (count (ix, "SELECT count(*) FROM tags WHERE name = 'ghost'"), ==, 0);

   /* gimv wrote a comment file: "tags <path>" */
   write_comment (a, "X-IMG-Tags: trip, sea\n");
   cmd = g_strconcat ("tags ", a, NULL);
   ix_send (ix, cmd);
   g_free (cmd);
   ix_wait (ix, "tags-updated");
   assert_tags (ix, a, "sea, trip");
   g_assert_cmpint (count (ix, "SELECT count(*) FROM tags WHERE name = 'family'"), ==, 0);

   /* the comment file removed */
   {
      gchar *cb = comment_file (b);
      g_unlink (cb);
      g_free (cb);
   }
   cmd = g_strconcat ("tags ", b, NULL);
   ix_send (ix, cmd);
   g_free (cmd);
   ix_wait (ix, "tags-updated");
   assert_tags (ix, b, "");
   ix_stop (ix);

   /* changed while it did not run: read again at the start */
   write_comment (c, "X-IMG-Tags: later\n");
   ix = ix_start ("manual.db", root, args);
   ix_wait (ix, "idle");
   assert_tags (ix, c, "later");
   assert_tags (ix, a, "sea, trip");
   ix_stop (ix);

   g_free (a); g_free (b); g_free (c); g_free (d); g_free (e);
   g_free (root);
}


/******************************************************************************
 *  the tags of the rules
 ******************************************************************************/

static void
test_auto_tags (void)
{
   gchar *root = new_root ();
   static const struct { const gchar *rel; gint w, h; const gchar *tags; } t[] = {
      { "top.png",                 100, 50,  "shape/landscape*" },   /* the root: no folder */
      { "Kyoto 2019/k.png",        50, 100,  "folder/Kyoto 2019*, shape/portrait*" },
      { "2019/n.png",              100, 99,  "shape/square*" },      /* digits only */
      { "2019-05-03/n.png",        99, 100,  "shape/square*" },
      { "2019_05 03/n.png",        10, 10,   "shape/square*" },
      { "DCIM/d.png",              10, 10,   "shape/square*" },
      { "dcim/100CANON/c.png",     10, 10,   "shape/square*" },
      { "DCIM/101_PANA/p.png",     10, 10,   "shape/square*" },
      { "Private Stuff/p.png",     10, 10,   "shape/square*" },      /* --folder-exclude */
      { "tmp/t.png",               10, 10,   "shape/square*" },
      { "Holiday/sub 1/h.png",     10, 10,   "folder/sub 1*, shape/square*" },
      { "1000photos/x.png",        10, 10,   "folder/1000photos*, shape/square*" },
      { "Privat/x.png",            10, 10,   "folder/Privat*, shape/square*" },
   };
   const gchar *args[] = { "--folder-exclude=private*; Tmp", NULL };
   Indexer *ix;
   guint i;

   for (i = 0; i < G_N_ELEMENTS (t); i++) {
      gchar *p = in (root, t[i].rel);
      write_png (p, t[i].w, t[i].h);
      g_free (p);
   }
#ifdef HAVE_EXIF
   {
      gchar *p = in (root, "turned.jpg");
      gimv_test_write_jpeg (p, 60, 40, 6, NULL, "NIKON CORPORATION", "NIKON D90");
      g_free (p);
      p = in (root, "apple.jpg");
      gimv_test_write_jpeg (p, 60, 40, 1, NULL, "Apple", "iPhone 12");
      g_free (p);
      p = in (root, "canon.jpg");
      gimv_test_write_jpeg (p, 40, 60, 0, NULL, "Canon", "Canon EOS 5D");
      g_free (p);
      p = in (root, "nomake.jpg");
      gimv_test_write_jpeg (p, 40, 60, 0, NULL, NULL, "Model Only");
      g_free (p);
   }
#endif

   ix = ix_start ("auto.db", root, args);
   ix_wait (ix, "idle");
   for (i = 0; i < G_N_ELEMENTS (t); i++) {
      gchar *p = in (root, t[i].rel);
      assert_tags (ix, p, t[i].tags);
      g_free (p);
   }
#ifdef HAVE_EXIF
   {
      gchar *p = in (root, "turned.jpg");
      assert_tags (ix, p, "camera/NIKON D90*, shape/portrait*");   /* 90 degrees */
      g_free (p);
      p = in (root, "apple.jpg");
      assert_tags (ix, p, "camera/Apple iPhone 12*, shape/landscape*");
      g_free (p);
      p = in (root, "canon.jpg");
      assert_tags (ix, p, "camera/Canon EOS 5D*, shape/portrait*");
      g_free (p);
      p = in (root, "nomake.jpg");
      assert_tags (ix, p, "camera/Model Only*, shape/portrait*");
      g_free (p);
   }
#endif
   ix_stop (ix);

   /* other rules, the same database: the tags put again */
   {
      const gchar *args2[] = { "--auto=folder", NULL };
      gchar *p = in (root, "Kyoto 2019/k.png"), *q = in (root, "top.png");
      ix = ix_start ("auto.db", root, args2);
      ix_wait (ix, "idle");
      assert_tags (ix, p, "folder/Kyoto 2019*");
      assert_tags (ix, q, "");
      ix_stop (ix);
      g_free (p);
      g_free (q);
   }
   {
      const gchar *args3[] = { "--auto=shape", NULL };
      gchar *p = in (root, "Kyoto 2019/k.png");
      ix = ix_start ("auto.db", root, args3);
      ix_wait (ix, "idle");
      assert_tags (ix, p, "shape/portrait*");
      ix_stop (ix);
      g_free (p);
   }
   g_free (root);
}


/* the tags of a person and of a rule may have the same name: both kept */
static void
test_manual_and_auto (void)
{
   gchar *root = new_root ();
   gchar *p = in (root, "Paris/x.png");
   const gchar *args[] = { "--auto=folder", NULL };
   Indexer *ix;

   write_png (p, 4, 4);
   write_comment (p, "X-IMG-Tags: folder/Paris, mine\n");
   ix = ix_start ("both.db", root, args);
   ix_wait (ix, "idle");
   assert_tags (ix, p, "folder/Paris, folder/Paris*, mine");
   ix_stop (ix);
   g_free (p);
   g_free (root);
}


/******************************************************************************
 *  changes while it runs (inotify)
 ******************************************************************************/

static void
test_watch (void)
{
   gchar *root = new_root ();
   gchar *a = in (root, "a.png"), *b = in (root, "sub/b.png");
   gchar *added = in (root, "added.png"), *w = in (root, "sub/written.png");
   gchar *a2 = in (root, "sub/a2.png"), *b2 = in (root, "sub/b2.png");
   gchar *outside = g_build_filename (tmp, "outside", NULL);
   gchar *newdir = in (root, "newdir"), *nd = in (root, "newdir/x.png");
   gchar *sub2 = in (root, "sub2"), *line;
   const gchar *args[] = { "--auto=folder", NULL };
   gint64 id_b;
   Indexer *ix;

   write_png (a, 4, 4);
   write_png (b, 4, 4);
   write_comment (b, "X-IMG-Tags: kept\n");
   ix = ix_start ("watch.db", root, args);
   ix_wait (ix, "idle");
   g_assert_true (has_file (ix, a));
   {
      sqlite3 *db = db_open (ix);
      id_b = file_id (db, b);
      sqlite3_close (db);
   }

   /* a new image, written to a temporary file and renamed */
   write_png (added, 4, 4);
   ix_wait_until (ix, cond_has, added);
   line = g_strconcat ("new\t", added, NULL);
   g_assert_true (ix_said (ix, line));
   g_free (line);

   /* written in place */
   {
      FILE *f = fopen (w, "wb");
      GdkPixbuf *px = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 2, 2);
      gchar *buf; gsize len;
      gdk_pixbuf_save_to_buffer (px, &buf, &len, "png", NULL, NULL);
      g_assert_nonnull (f);
      fwrite (buf, 1, len, f);
      fclose (f);
      g_free (buf);
      g_object_unref (px);
   }
   ix_wait_until (ix, cond_has, w);
   assert_tags (ix, w, "folder/sub*");

   /* not an image: not there */
   {
      gchar *txt = in (root, "notes.txt");
      gimv_test_write_string (txt, "x");
      g_free (txt);
   }

   /* moved to another folder: the folder tag of the new one */
   g_assert_cmpint (g_rename (a, a2), ==, 0);
   ix_wait_until (ix, cond_has, a2);
   g_assert_false (has_file (ix, a));
   ix_wait_until (ix, cond_has, a2);
   assert_tags (ix, a2, "folder/sub*");

   /* an image with a comment file moved: the same row (its tags), and
    * gimv told the comment file is left behind */
   g_assert_cmpint (g_rename (b, b2), ==, 0);
   ix_wait_until (ix, cond_has, b2);
   {
      sqlite3 *db = db_open (ix);
      g_assert_cmpint (file_id (db, b2), ==, id_b);
      sqlite3_close (db);
   }
   line = g_strconcat ("moved\t", b, "\t", b2, NULL);
   g_assert_true (ix_said (ix, line));
   g_free (line);
   /* the comment file is the record of the tags: until gimv moves it,
    * none (gimv moves it and says "tags <new>") */
   assert_tags (ix, b2, "folder/sub*");
   {
      gchar *cb = comment_file (b), *cb2 = comment_file (b2), *cmd;
      g_assert_cmpint (g_rename (cb, cb2), ==, 0);
      cmd = g_strconcat ("tags ", b2, NULL);
      ix_send (ix, cmd);
      ix_wait (ix, "tags-updated");
      g_free (cmd);
      g_free (cb);
      g_free (cb2);
   }
   assert_tags (ix, b2, "folder/sub*, kept");

   /* removed */
   g_assert_cmpint (g_unlink (w), ==, 0);
   ix_wait_until (ix, cond_gone, w);

   /* a folder of images moved in from outside */
   {
      gchar *o = g_build_filename (outside, "x.png", NULL);
      write_png (o, 4, 4);
      g_free (o);
   }
   g_assert_cmpint (g_rename (outside, newdir), ==, 0);
   ix_wait_until (ix, cond_has, nd);
   assert_tags (ix, nd, "folder/newdir*");

   /* a folder renamed: its images keep their rows, with the new folder tag */
   g_assert_cmpint (g_rename (in (root, "sub"), sub2), ==, 0);
   {
      gchar *b3 = in (root, "sub2/b2.png");
      ix_wait_until (ix, cond_has, b3);
      g_assert_false (has_file (ix, b2));
      {
         sqlite3 *db = db_open (ix);
         g_assert_cmpint (file_id (db, b3), ==, id_b);
         sqlite3_close (db);
      }
      /* the folder tag after stage 2, which is done by now; the tags of
       * the comment files once gimv moved them (it was told) */
      assert_tags (ix, b3, "folder/sub2*");
      {
         gchar *osub = in (root, "sub"), *msg = g_strconcat ("moved-dir\t", osub, "\t", sub2, NULL);
         gchar *cd = g_strconcat (home, "/.gimv/comment", osub, NULL);
         gchar *cd2 = g_strconcat (home, "/.gimv/comment", sub2, NULL);
         gchar *cmd = g_strconcat ("tags ", b3, NULL);
         g_assert_true (ix_said (ix, msg));
         g_assert_cmpint (g_rename (cd, cd2), ==, 0);
         ix_send (ix, cmd);
         ix_wait (ix, "tags-updated");
         g_free (osub); g_free (msg); g_free (cd); g_free (cd2); g_free (cmd);
      }
      assert_tags (ix, b3, "folder/sub2*, kept");
      g_free (b3);
   }

   /* a folder removed */
   gimv_test_rm_rf (newdir);
   ix_wait_until (ix, cond_gone, nd);
   g_assert_cmpint (count (ix, "SELECT count(*) FROM dirs WHERE path LIKE '%/newdir'"), ==, 0);

   ix_stop (ix);
   g_free (a); g_free (b); g_free (added); g_free (w); g_free (a2); g_free (b2);
   g_free (outside); g_free (newdir); g_free (nd); g_free (sub2);
   g_free (root);
}


/* changed while it did not run: found at the next start */
static void
test_offline_changes (void)
{
   gchar *root = new_root ();
   gchar *a = in (root, "a.png"), *b = in (root, "sub/b.png"), *c = in (root, "sub/c.png");
   gchar *d = in (root, "d.png"), *e = in (root, "deep/er/e.png");
   const gchar *args[] = { "--auto=folder,shape", NULL };
   Indexer *ix;

   write_png (a, 4, 4);
   write_png (b, 4, 4);
   write_png (c, 10, 20);
   ix = ix_start ("offline.db", root, args);
   ix_wait (ix, "idle");
   g_assert_cmpint (count (ix, "SELECT count(*) FROM files"), ==, 3);
   ix_stop (ix);

   /* the mtimes of the directories are compared in seconds: a change in
    * the second of the last look is not seen (until a rescan) */
   g_usleep (1100 * 1000);
   g_unlink (a);
   write_png (d, 4, 4);
   write_png (e, 4, 4);
   g_rename (b, in (root, "b-renamed.png"));
   /* the same file changed (the dates of the directories may not change) */
   write_png (c, 20, 10);
   gimv_test_set_mtime (c, 1600000000);

   ix = ix_start ("offline.db", root, args);
   ix_wait (ix, "idle");
   g_assert_false (has_file (ix, a));
   g_assert_false (has_file (ix, b));
   g_assert_true (has_file (ix, d));
   g_assert_true (has_file (ix, e));
   g_assert_true (has_file (ix, in (root, "b-renamed.png")));
   assert_tags (ix, c, "folder/sub*, shape/landscape*");
   assert_date (ix, c, "2020-09-13 12:26:40 0");
   g_assert_cmpint (count (ix, "SELECT count(*) FROM files"), ==, 4);
   g_assert_true (ix_said (ix, "added-since-last 3"));
   ix_stop (ix);

   g_free (a); g_free (b); g_free (c); g_free (d); g_free (e);
   g_free (root);
}


/* a folder whose name is not ASCII moved while watching: the paths of the
 * folders below it */
static void
test_watch_dir_move_utf8 (void)
{
   gchar *root = new_root ();
   gchar *old = in (root, "\xe5\x86\x99\xe7\x9c\x9f"), *new = in (root, "photos");   /* 写真 */
   gchar *x = in (root, "\xe5\x86\x99\xe7\x9c\x9f/sub/x.png"), *nx = in (root, "photos/sub/x.png");
   const gchar *args[] = { "--auto=folder", NULL };
   Indexer *ix;

   if (gimv_test_known_bug ("gimv-indexer, a folder moved while watching: the paths "
                            "below it are cut with substr () (characters) at strlen () "
                            "(bytes); wrong when the old path is not ASCII"))
      return;

   write_png (x, 4, 4);
   ix = ix_start ("utf8.db", root, args);
   ix_wait (ix, "idle");
   g_assert_true (has_file (ix, x));
   g_assert_cmpint (g_rename (old, new), ==, 0);
   ix_wait (ix, "idle");
   while (!has_file (ix, nx) && count (ix, "SELECT count(*) FROM files WHERE stage < 2") > 0)
      ix_wait (ix, "idle");
   g_assert_true (has_file (ix, nx));
   ix_stop (ix);
   g_free (old); g_free (new); g_free (x); g_free (nx);
   g_free (root);
}


/* two folders whose names differ only in case (Trip, trip): one removed */
static void
test_dir_remove_case (void)
{
   gchar *root = new_root ();
   gchar *small = in (root, "trip"), *x = in (root, "trip/x.png");
   gchar *y = in (root, "Trip/sub/y.png");
   const gchar *args[] = { "--auto=folder", NULL };
   Indexer *ix;

   if (gimv_test_known_bug ("gimv-indexer: a folder removed (or moved) takes the rows "
                            "of the folders below another folder whose name differs "
                            "only in case (LIKE ignores the case of ASCII letters)"))
      return;

   write_png (x, 4, 4);
   write_png (y, 4, 4);
   ix = ix_start ("case.db", root, args);
   ix_wait (ix, "idle");
   g_assert_true (has_file (ix, x));
   g_assert_true (has_file (ix, y));
   gimv_test_rm_rf (small);
   ix_wait_until (ix, cond_gone, x);
   g_assert_true (has_file (ix, y));
   ix_stop (ix);
   g_free (small); g_free (x); g_free (y);
   g_free (root);
}


/* the database is taken by one indexer at a time */
static void
test_busy (void)
{
   gchar *root = new_root ();
   gchar *a = in (root, "a.png");
   Indexer *ix, *other;

   write_png (a, 4, 4);
   ix = ix_start ("busy.db", root, NULL);
   ix_wait (ix, "idle");
   other = ix_start ("busy.db", root, NULL);
   ix_wait (other, "busy");
   {
      GError *error = NULL;
      g_subprocess_wait (other->proc, NULL, &error);
      g_assert_no_error (error);
      g_assert_cmpint (g_subprocess_get_exit_status (other->proc), ==, 0);
      g_mutex_lock (&other->lock);
      other->done = TRUE;
      g_mutex_unlock (&other->lock);
      g_thread_join (other->watchdog);
      g_object_unref (other->out);
      g_object_unref (other->proc);
      g_ptr_array_unref (other->lines);
      g_free (other->db);
      g_free (other);
   }
   ix_stop (ix);
   g_free (a);
   g_free (root);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   tmp = gimv_test_mkdtemp ();
   home = g_build_filename (tmp, "home", NULL);
   g_mkdir_with_parents (home, 0700);

   g_test_add_func ("/indexer/dates", test_dates);
   g_test_add_func ("/indexer/manual-tags", test_manual_tags);
   g_test_add_func ("/indexer/auto-tags", test_auto_tags);
   g_test_add_func ("/indexer/manual-and-auto", test_manual_and_auto);
   g_test_add_func ("/indexer/watch", test_watch);
   g_test_add_func ("/indexer/offline-changes", test_offline_changes);
   g_test_add_func ("/indexer/watch-dir-move-utf8", test_watch_dir_move_utf8);
   g_test_add_func ("/indexer/dir-remove-case", test_dir_remove_case);
   g_test_add_func ("/indexer/busy", test_busy);

   ret = g_test_run ();
   gimv_test_rm_rf (tmp);
   g_free (home);
   g_free (tmp);
   return ret;
}
