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
 * tags, file_tags (version 2): tag names and which file has which tag.
 *        auto = 0: put by a person, read from the comment file (the comment
 *        file is the record, the database only an index of it);
 *        auto = 1: put by a rule (kept only here).
 * comments (version 2): the mtime (nanoseconds) of each comment file read,
 *        to read only the changed ones again.
 */
/* added in version 2: a version 1 database just gets these */
#define SCHEMA_V2 \
   "CREATE TABLE tags (" \
   "  id   INTEGER PRIMARY KEY," \
   "  name TEXT NOT NULL UNIQUE);" \
   "CREATE TABLE file_tags (" \
   "  file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE," \
   "  tag_id  INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE," \
   "  auto    INTEGER NOT NULL DEFAULT 0," \
   "  PRIMARY KEY (file_id, tag_id, auto));" \
   "CREATE INDEX file_tags_tag ON file_tags(tag_id);" \
   "CREATE TABLE comments (" \
   "  file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE," \
   "  mtime   INTEGER NOT NULL);"

/*
 * faces (version 3), made by gimv-faces for the folders chosen for it:
 * face_scans: the files looked at, with the mtime then (read again when it
 *        changes) and the faces found (-1: gimv-faces died reading it).
 * faces: one row for each face.  x, y, w, h in the pixels of the image as
 *        shown (EXIF orientation applied); feature: 128 floats (SFace,
 *        L2-normalized, native byte order).
 *        person_id: named by a person; guess: the person it looks like;
 *        cluster: a group of unnamed faces that look alike (NULL: alone).
 * persons: the names given to faces.
 */
#define SCHEMA_V3 \
   "CREATE TABLE persons (" \
   "  id   INTEGER PRIMARY KEY," \
   "  name TEXT NOT NULL UNIQUE);" \
   "CREATE TABLE face_scans (" \
   "  file_id INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE," \
   "  mtime   INTEGER NOT NULL," \
   "  n       INTEGER NOT NULL);" \
   "CREATE TABLE faces (" \
   "  id        INTEGER PRIMARY KEY," \
   "  file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE," \
   "  x INTEGER NOT NULL, y INTEGER NOT NULL," \
   "  w INTEGER NOT NULL, h INTEGER NOT NULL," \
   "  score     REAL NOT NULL," \
   "  feature   BLOB NOT NULL," \
   "  person_id INTEGER REFERENCES persons(id) ON DELETE SET NULL," \
   "  guess     INTEGER REFERENCES persons(id) ON DELETE SET NULL," \
   "  cluster   INTEGER);" \
   "CREATE INDEX faces_file ON faces(file_id);" \
   "CREATE INDEX faces_person ON faces(person_id);" \
   "CREATE INDEX faces_cluster ON faces(cluster);"

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
   SCHEMA_V2
   SCHEMA_V3
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
   } else if (v == 1 || v == 2) {
      /* older versions: only new tables */
      gchar *sql = g_strconcat ("BEGIN;", v == 1 ? SCHEMA_V2 : "", SCHEMA_V3,
                                "PRAGMA user_version = " GIMV_INDEX_DB_VERSION_STR
                                "; COMMIT;", NULL);
      gint r = sqlite3_exec (db, sql, NULL, NULL, NULL);
      g_free (sql);
      if (r != SQLITE_OK)
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
