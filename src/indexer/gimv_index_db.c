/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index database, shared by gimv and gimv-indexer.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include <glib/gstdio.h>

#include "gimv_index_db.h"

/*
 * dirs:  every directory below a root, roots have parent NULL.
 *        mtime/scanned: the directory mtime when its file list was last read.
 * files: image files.  date is local time "YYYY-MM-DD HH:MM:SS" taken from
 *        date_src (mtime, file name or EXIF).  stage is the last scan stage
 *        done for the file.
 */
static const gchar *schema =
   "CREATE TABLE dirs ("
   "  id      INTEGER PRIMARY KEY,"
   "  path    TEXT NOT NULL UNIQUE,"
   "  parent  INTEGER REFERENCES dirs(id) ON DELETE CASCADE,"
   "  mtime   INTEGER NOT NULL DEFAULT 0,"
   "  scanned INTEGER NOT NULL DEFAULT 0);"
   "CREATE INDEX dirs_parent ON dirs(parent);"
   "CREATE TABLE files ("
   "  id       INTEGER PRIMARY KEY,"
   "  dir_id   INTEGER NOT NULL REFERENCES dirs(id) ON DELETE CASCADE,"
   "  name     TEXT NOT NULL,"
   "  size     INTEGER NOT NULL,"
   "  mtime    INTEGER NOT NULL,"
   "  date     TEXT,"
   "  date_src INTEGER NOT NULL DEFAULT 0,"
   "  stage    INTEGER NOT NULL DEFAULT 1,"
   "  UNIQUE (dir_id, name));"
   "CREATE INDEX files_date ON files(date);"
   "CREATE INDEX files_stage ON files(stage);"
   "PRAGMA user_version = " GIMV_INDEX_DB_VERSION_STR ";";


gchar *
gimv_index_db_default_path (void)
{
   return g_build_filename (g_get_user_data_dir (), "gimageview", "index.db", NULL);
}


static gint
get_version (sqlite3 *db, gboolean *ok)
{
   sqlite3_stmt *st;
   gint v = -1;

   *ok = FALSE;
   if (sqlite3_prepare_v2 (db, "PRAGMA user_version", -1, &st, NULL) != SQLITE_OK)
      return -1;
   if (sqlite3_step (st) == SQLITE_ROW) {
      v = sqlite3_column_int (st, 0);
      *ok = TRUE;
   }
   sqlite3_finalize (st);
   return v;
}


static gboolean
quick_check (sqlite3 *db)
{
   sqlite3_stmt *st;
   gboolean ok = FALSE;

   if (sqlite3_prepare_v2 (db, "PRAGMA quick_check", -1, &st, NULL) != SQLITE_OK)
      return FALSE;
   if (sqlite3_step (st) == SQLITE_ROW) {
      const gchar *r = (const gchar *) sqlite3_column_text (st, 0);
      ok = r && !strcmp (r, "ok");
   }
   sqlite3_finalize (st);
   return ok;
}


static void
move_aside (const gchar *path)
{
   static const gchar *suffix[] = { "", "-wal", "-shm" };
   guint i;

   for (i = 0; i < G_N_ELEMENTS (suffix); i++) {
      gchar *from = g_strconcat (path, suffix[i], NULL);
      gchar *to   = g_strconcat (path, ".old", suffix[i], NULL);
      g_unlink (to);
      if (g_file_test (from, G_FILE_TEST_EXISTS))
         g_rename (from, to);
      g_free (from);
      g_free (to);
   }
}


static GimvIndexDbResult
open_rw (const gchar *path, sqlite3 **dbp, gchar **errmsg, gboolean retry)
{
   sqlite3 *db = NULL;
   gboolean ok;
   gint v;
   gchar *dir = g_path_get_dirname (path);

   g_mkdir_with_parents (dir, 0700);
   g_free (dir);

   if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK)
      goto broken;

   sqlite3_busy_timeout (db, 5000);
   v = get_version (db, &ok);
   if (!ok) goto broken;

   if (v == 0) {
      /* new, empty file */
      sqlite3_exec (db, "PRAGMA journal_mode = WAL", NULL, NULL, NULL);
      if (sqlite3_exec (db, schema, NULL, NULL, NULL) != SQLITE_OK)
         goto broken;
   } else if (v != GIMV_INDEX_DB_VERSION || !quick_check (db)) {
      /* another version or damaged: the index can always be rebuilt from
       * the files themselves, and tags live in the comment files */
      goto broken;
   }

   sqlite3_exec (db, "PRAGMA journal_mode = WAL;"
                     "PRAGMA synchronous = NORMAL;"
                     "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
   *dbp = db;
   return GIMV_INDEX_DB_OK;

broken:
   if (db) sqlite3_close (db);
   if (retry) {
      move_aside (path);
      return open_rw (path, dbp, errmsg, FALSE);
   }
   if (errmsg)
      *errmsg = g_strdup_printf ("cannot create the database %s", path);
   return GIMV_INDEX_DB_ERROR;
}


GimvIndexDbResult
gimv_index_db_open (const gchar *path, gboolean writable,
                    sqlite3 **dbp, gchar **errmsg)
{
   sqlite3 *db = NULL;
   gboolean ok;
   gint v;

   *dbp = NULL;
   if (writable)
      return open_rw (path, dbp, errmsg, TRUE);

   if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
      return GIMV_INDEX_DB_MISSING;

   if (sqlite3_open_v2 (path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
      if (errmsg) *errmsg = g_strdup (sqlite3_errmsg (db));
      sqlite3_close (db);
      return GIMV_INDEX_DB_ERROR;
   }
   sqlite3_busy_timeout (db, 1000);
   v = get_version (db, &ok);
   if (!ok || v != GIMV_INDEX_DB_VERSION) {
      sqlite3_close (db);
      /* the indexer will rebuild it */
      return GIMV_INDEX_DB_MISSING;
   }
   *dbp = db;
   return GIMV_INDEX_DB_OK;
}
