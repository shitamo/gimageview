/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * Tests of src/gimv_bookmark_cand.c: where a bookmarked file or folder
 * that is not found may have gone (the files and folders of the same name
 * in the index that are there).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>

#include "gimv_bookmark_cand.h"

static gchar   *top;
static sqlite3 *db;


static void
add_file (const gchar *rel, gboolean on_disk)
{
   g_autofree gchar *path = g_build_filename (top, rel, NULL);
   g_autofree gchar *dir = g_path_get_dirname (path);
   g_autofree gchar *name = g_path_get_basename (path);
   g_autofree gchar *sql = NULL;

   if (on_disk) {
      g_mkdir_with_parents (dir, 0700);
      g_assert_true (g_file_set_contents (path, "x", 1, NULL));
   }
   sql = sqlite3_mprintf ("INSERT OR IGNORE INTO dirs (path) VALUES (%Q);"
                          "INSERT INTO files (dir_id, name) "
                          "SELECT id, %Q FROM dirs WHERE path = %Q;", dir, name, dir);
   g_assert_cmpint (sqlite3_exec (db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
   sqlite3_free (sql);
   sql = NULL;
}


static void
add_dir (const gchar *rel)
{
   g_autofree gchar *path = g_build_filename (top, rel, NULL);
   gchar *sql;

   g_mkdir_with_parents (path, 0700);
   sql = sqlite3_mprintf ("INSERT OR IGNORE INTO dirs (path) VALUES (%Q);", path);
   g_assert_cmpint (sqlite3_exec (db, sql, NULL, NULL, NULL), ==, SQLITE_OK);
   sqlite3_free (sql);
}


static GPtrArray *
cands (const gchar *rel)
{
   g_autofree gchar *path = g_build_filename (top, rel, NULL);
   return gimv_bookmark_missing_candidates (db, path);
}


static void
assert_rel (GPtrArray *a, guint i, const gchar *rel)
{
   g_autofree gchar *path = g_build_filename (top, rel, NULL);
   g_assert_cmpstr (g_ptr_array_index (a, i), ==, path);
}


static void
test_candidates (void)
{
   GPtrArray *a;

   add_file ("b/x1.png", TRUE);          /* moved from a/ */
   add_file ("d/two.png", TRUE);
   add_file ("e/two.png", TRUE);
   add_file ("f/gone.png", FALSE);       /* in the index, not on the disk */
   add_file ("g/X1.PNG", TRUE);          /* another name */
   add_dir ("b/trip");
   add_dir ("b/trip_2025");              /* "_" of LIKE is one character */
   add_file ("b/50%/p.png", TRUE);

   a = cands ("a/x1.png");
   g_assert_cmpuint (a->len, ==, 1);
   assert_rel (a, 0, "b/x1.png");
   g_ptr_array_unref (a);

   a = cands ("a/two.png");
   g_assert_cmpuint (a->len, ==, 2);
   assert_rel (a, 0, "d/two.png");
   assert_rel (a, 1, "e/two.png");
   g_ptr_array_unref (a);

   /* not the path itself; not one gone from the disk */
   a = cands ("d/two.png");
   g_assert_cmpuint (a->len, ==, 1);
   assert_rel (a, 0, "e/two.png");
   g_ptr_array_unref (a);
   a = cands ("a/gone.png");
   g_assert_cmpuint (a->len, ==, 0);
   g_ptr_array_unref (a);

   /* folders: the name, not a name LIKE it */
   a = cands ("trip");
   g_assert_cmpuint (a->len, ==, 1);
   assert_rel (a, 0, "b/trip");
   g_ptr_array_unref (a);
   a = cands ("old/trip-2025");
   g_assert_cmpuint (a->len, ==, 0);
   g_ptr_array_unref (a);
   a = cands ("old/50%");
   g_assert_cmpuint (a->len, ==, 1);
   assert_rel (a, 0, "b/50%");
   g_ptr_array_unref (a);

   /* no index */
   a = gimv_bookmark_missing_candidates (NULL, "/a/x1.png");
   g_assert_cmpuint (a->len, ==, 0);
   g_ptr_array_unref (a);
}


static void
rm_rf (const gchar *path)
{
   GDir *d = g_dir_open (path, 0, NULL);
   const gchar *n;

   if (d) {
      while ((n = g_dir_read_name (d))) {
         g_autofree gchar *p = g_build_filename (path, n, NULL);
         rm_rf (p);
      }
      g_dir_close (d);
   }
   g_remove (path);
}


int
main (int argc, char *argv[])
{
   gint ret;

   g_test_init (&argc, &argv, NULL);
   top = g_dir_make_tmp ("gimv-bmcand-XXXXXX", NULL);
   g_assert_nonnull (top);
   g_assert_cmpint (sqlite3_open (":memory:", &db), ==, SQLITE_OK);
   g_assert_cmpint (sqlite3_exec (db,
                       "CREATE TABLE dirs (id INTEGER PRIMARY KEY, path TEXT UNIQUE);"
                       "CREATE TABLE files (id INTEGER PRIMARY KEY, dir_id INTEGER, name TEXT);",
                       NULL, NULL, NULL), ==, SQLITE_OK);

   g_test_add_func ("/bookmark-cand/candidates", test_candidates);
   ret = g_test_run ();

   sqlite3_close (db);
   rm_rf (top);
   g_free (top);
   return ret;
}
