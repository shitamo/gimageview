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
 * The index database (src/indexer/gimv_index_db.c): made new, opened
 * read-only, older versions brought up to date, other versions and broken
 * files put aside, and the waiting for the other writers.
 */

#include "gimv_test.h"
#include "gimv_index_db.h"

static gchar *tmp_dir;


static gint
query_int (sqlite3 *db, const gchar *sql)
{
   sqlite3_stmt *st;
   gint v = -1;

   g_assert_cmpint (sqlite3_prepare_v2 (db, sql, -1, &st, NULL), ==, SQLITE_OK);
   if (sqlite3_step (st) == SQLITE_ROW) v = sqlite3_column_int (st, 0);
   sqlite3_finalize (st);
   return v;
}


static gchar *
query_text (sqlite3 *db, const gchar *sql)
{
   sqlite3_stmt *st;
   gchar *v = NULL;

   g_assert_cmpint (sqlite3_prepare_v2 (db, sql, -1, &st, NULL), ==, SQLITE_OK);
   if (sqlite3_step (st) == SQLITE_ROW)
      v = g_strdup ((const gchar *) sqlite3_column_text (st, 0));
   sqlite3_finalize (st);
   return v;
}


static gboolean
has_table (sqlite3 *db, const gchar *name)
{
   gchar *sql = sqlite3_mprintf ("SELECT count(*) FROM sqlite_master "
                                 "WHERE type = 'table' AND name = '%q'", name);
   gboolean ret = query_int (db, sql) == 1;
   sqlite3_free (sql);
   return ret;
}


static gchar *
db_path (const gchar *name)
{
   return g_build_filename (tmp_dir, name, NULL);
}


static void
test_default_path (void)
{
   gchar *path, *want;

   g_setenv ("XDG_DATA_HOME", "/data/home", TRUE);
   path = gimv_index_db_default_path ();
   want = g_build_filename (g_get_user_data_dir (), "gimageview", "index.db", NULL);
   g_assert_cmpstr (path, ==, want);
   g_free (path);
   g_free (want);
}


static void
test_create (void)
{
   gchar *path = g_build_filename (tmp_dir, "new", "dir", "index.db", NULL);
   static const gchar *tables[] = {
      "dirs", "files", "tags", "file_tags", "comments", "persons", "face_scans",
      "faces", "face_rejects", "face_thumbs", "person_tags",
   };
   sqlite3 *db = NULL;
   gchar *err = NULL, *mode;
   guint i;

   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, &err), ==, GIMV_INDEX_DB_OK);
   g_assert_null (err);
   g_assert_nonnull (db);
   g_assert_true (g_file_test (path, G_FILE_TEST_IS_REGULAR));
   g_assert_cmpint (query_int (db, "PRAGMA user_version"), ==, GIMV_INDEX_DB_VERSION);
   for (i = 0; i < G_N_ELEMENTS (tables); i++)
      g_assert_true (has_table (db, tables[i]));
   mode = query_text (db, "PRAGMA journal_mode");
   g_assert_cmpstr (mode, ==, "wal");
   g_free (mode);
   g_assert_cmpint (query_int (db, "PRAGMA foreign_keys"), ==, 1);
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM faces WHERE ignored = 0"), ==, 0);

   /* the foreign keys: a directory takes its files and their tags */
   g_assert_cmpint (sqlite3_exec (db,
      "INSERT INTO dirs (id, path) VALUES (1, '/a');"
      "INSERT INTO files (id, dir_id, name, size, mtime) VALUES (1, 1, 'x.jpg', 1, 1);"
      "INSERT INTO tags (id, name) VALUES (1, 't');"
      "INSERT INTO file_tags (file_id, tag_id, auto) VALUES (1, 1, 0);"
      "DELETE FROM dirs WHERE id = 1;", NULL, NULL, NULL), ==, SQLITE_OK);
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM files"), ==, 0);
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM file_tags"), ==, 0);
   sqlite3_close (db);

   /* opened again: as it is */
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM tags"), ==, 1);
   sqlite3_close (db);
   g_free (path);
}


static void
test_read_only (void)
{
   gchar *path = db_path ("ro.db");
   sqlite3 *db = (sqlite3 *) 0x1;

   /* not made yet */
   g_assert_cmpint (gimv_index_db_open (path, FALSE, &db, NULL), ==, GIMV_INDEX_DB_MISSING);
   g_assert_null (db);
   g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   sqlite3_exec (db, "INSERT INTO tags (name) VALUES ('kept')", NULL, NULL, NULL);
   sqlite3_close (db);

   g_assert_cmpint (gimv_index_db_open (path, FALSE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM tags WHERE name = 'kept'"), ==, 1);
   g_assert_cmpint (sqlite3_exec (db, "INSERT INTO tags (name) VALUES ('no')", NULL, NULL, NULL),
                    ==, SQLITE_READONLY);
   sqlite3_close (db);

   /* another version: as if missing (the indexer makes it again) */
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   sqlite3_exec (db, "PRAGMA user_version = 99", NULL, NULL, NULL);
   sqlite3_close (db);
   g_assert_cmpint (gimv_index_db_open (path, FALSE, &db, NULL), ==, GIMV_INDEX_DB_MISSING);
   g_assert_null (db);
   g_free (path);
}


/* the schema of version 1 (dirs and files) */
static void
make_v1 (const gchar *path)
{
   sqlite3 *db;

   g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
   g_assert_cmpint (sqlite3_exec (db,
      "CREATE TABLE dirs (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
      "  parent INTEGER REFERENCES dirs(id) ON DELETE CASCADE,"
      "  mtime INTEGER NOT NULL DEFAULT 0, scanned INTEGER NOT NULL DEFAULT 0);"
      "CREATE TABLE files (id INTEGER PRIMARY KEY,"
      "  dir_id INTEGER NOT NULL REFERENCES dirs(id) ON DELETE CASCADE,"
      "  name TEXT NOT NULL, size INTEGER NOT NULL, mtime INTEGER NOT NULL,"
      "  date TEXT, date_src INTEGER NOT NULL DEFAULT 0,"
      "  stage INTEGER NOT NULL DEFAULT 1, UNIQUE (dir_id, name));"
      "INSERT INTO dirs (id, path) VALUES (1, '/photos');"
      "INSERT INTO files (dir_id, name, size, mtime, date) VALUES (1, 'a.jpg', 10, 20, '2001-02-03 04:05:06');"
      "PRAGMA user_version = 1;", NULL, NULL, NULL), ==, SQLITE_OK);
   sqlite3_close (db);
}


static void
test_upgrade (void)
{
   gchar *path = db_path ("v1.db"), *old = g_strconcat (path, ".old", NULL), *date;
   sqlite3 *db;

   make_v1 (path);
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (query_int (db, "PRAGMA user_version"), ==, GIMV_INDEX_DB_VERSION);
   g_assert_true (has_table (db, "tags"));
   g_assert_true (has_table (db, "faces"));
   g_assert_true (has_table (db, "person_tags"));
   g_assert_cmpint (query_int (db, "SELECT count(*) FROM faces WHERE ignored = 0"), ==, 0);
   /* the rows kept */
   date = query_text (db, "SELECT date FROM files WHERE name = 'a.jpg'");
   g_assert_cmpstr (date, ==, "2001-02-03 04:05:06");
   g_free (date);
   sqlite3_close (db);
   g_assert_false (g_file_test (old, G_FILE_TEST_EXISTS));
   g_free (old);
   g_free (path);
}


static void
test_other_version (void)
{
   gchar *path = db_path ("v99.db"), *old = g_strconcat (path, ".old", NULL);
   sqlite3 *db;

   g_assert_cmpint (sqlite3_open (path, &db), ==, SQLITE_OK);
   sqlite3_exec (db, "CREATE TABLE future (x); INSERT INTO future VALUES (42);"
                     "PRAGMA user_version = 99;", NULL, NULL, NULL);
   sqlite3_close (db);

   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (query_int (db, "PRAGMA user_version"), ==, GIMV_INDEX_DB_VERSION);
   g_assert_false (has_table (db, "future"));
   sqlite3_close (db);

   /* put aside as FILE.old */
   g_assert_true (g_file_test (old, G_FILE_TEST_IS_REGULAR));
   g_assert_cmpint (sqlite3_open (old, &db), ==, SQLITE_OK);
   g_assert_cmpint (query_int (db, "SELECT x FROM future"), ==, 42);
   sqlite3_close (db);
   g_free (old);
   g_free (path);
}


static void
test_broken (void)
{
   gchar *path = db_path ("broken.db"), *old = g_strconcat (path, ".old", NULL);
   gchar *text = NULL;
   sqlite3 *db;

   gimv_test_write_string (path, "this is not an SQLite database, but it is long enough "
                                 "to be taken for the start of one by nobody");
   g_assert_cmpint (gimv_index_db_open (path, FALSE, &db, NULL), !=, GIMV_INDEX_DB_OK);
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (query_int (db, "PRAGMA user_version"), ==, GIMV_INDEX_DB_VERSION);
   sqlite3_close (db);
   g_assert_true (g_file_get_contents (old, &text, NULL, NULL));
   g_assert_true (g_str_has_prefix (text, "this is not"));
   g_free (text);
   g_free (old);
   g_free (path);
}


static void
test_cannot_create (void)
{
   gchar *file = db_path ("a-file"), *path = g_build_filename (file, "index.db", NULL);
   gchar *err = NULL;
   sqlite3 *db = NULL;

   gimv_test_write_string (file, "x");
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &db, &err), ==, GIMV_INDEX_DB_ERROR);
   g_assert_null (db);
   g_assert_nonnull (err);
   g_assert_nonnull (strstr (err, path));
   g_free (err);
   g_free (path);
   g_free (file);
}


typedef struct {
   sqlite3 *db;
   guint    ms;
} Holder;

static gpointer
release_later (gpointer data)
{
   Holder *h = data;
   g_usleep (h->ms * 1000);
   sqlite3_exec (h->db, "COMMIT", NULL, NULL, NULL);
   return NULL;
}


/* another writer holds the lock: waited for, up to the seconds given */
static void
test_wait_busy (void)
{
   gchar *path = db_path ("busy.db");
   sqlite3 *a, *b;
   GThread *t;
   Holder h;
   gint64 start, took;

   g_assert_cmpint (gimv_index_db_open (path, TRUE, &a, NULL), ==, GIMV_INDEX_DB_OK);
   g_assert_cmpint (gimv_index_db_open (path, TRUE, &b, NULL), ==, GIMV_INDEX_DB_OK);

   /* held for 300 ms: b waits and writes */
   g_assert_cmpint (sqlite3_exec (a, "BEGIN IMMEDIATE; INSERT INTO tags (name) VALUES ('a')",
                                  NULL, NULL, NULL), ==, SQLITE_OK);
   gimv_index_db_wait_busy (b, 5);
   h.db = a; h.ms = 300;
   t = g_thread_new ("holder", release_later, &h);
   start = g_get_monotonic_time ();
   g_assert_cmpint (sqlite3_exec (b, "INSERT INTO tags (name) VALUES ('b')", NULL, NULL, NULL),
                    ==, SQLITE_OK);
   took = g_get_monotonic_time () - start;
   g_thread_join (t);
   g_assert_cmpint (took, >=, 250 * 1000);
   g_assert_cmpint (took, <, 4 * G_USEC_PER_SEC);
   g_assert_cmpint (query_int (b, "SELECT count(*) FROM tags"), ==, 2);

   /* held longer than 1 second: b gives up after about 1 second */
   g_assert_cmpint (sqlite3_exec (a, "BEGIN IMMEDIATE", NULL, NULL, NULL), ==, SQLITE_OK);
   gimv_index_db_wait_busy (b, 1);
   start = g_get_monotonic_time ();
   g_assert_cmpint (sqlite3_exec (b, "INSERT INTO tags (name) VALUES ('c')", NULL, NULL, NULL),
                    ==, SQLITE_BUSY);
   took = g_get_monotonic_time () - start;
   g_assert_cmpint (took, >=, 900 * 1000);
   g_assert_cmpint (took, <, 5 * G_USEC_PER_SEC);
   sqlite3_exec (a, "COMMIT", NULL, NULL, NULL);

   sqlite3_close (a);
   sqlite3_close (b);
   g_free (path);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   tmp_dir = gimv_test_mkdtemp ();

   g_test_add_func ("/index-db/default-path", test_default_path);
   g_test_add_func ("/index-db/create", test_create);
   g_test_add_func ("/index-db/read-only", test_read_only);
   g_test_add_func ("/index-db/upgrade", test_upgrade);
   g_test_add_func ("/index-db/other-version", test_other_version);
   g_test_add_func ("/index-db/broken", test_broken);
   g_test_add_func ("/index-db/cannot-create", test_cannot_create);
   g_test_add_func ("/index-db/wait-busy", test_wait_busy);

   ret = g_test_run ();
   gimv_test_rm_rf (tmp_dir);
   g_free (tmp_dir);
   return ret;
}
