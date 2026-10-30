/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index database, shared by gimv and gimv-indexer.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_DB_H__
#define __GIMV_INDEX_DB_H__

#include <glib.h>
#include <sqlite3.h>

/* PRAGMA user_version of the current schema */
#define GIMV_INDEX_DB_VERSION      5
#define GIMV_INDEX_DB_VERSION_STR  "5"

/* the key of the tags in the gimv comment files */
#define GIMV_INDEX_TAGS_KEY        "X-IMG-Tags"

/* files.date_src */
typedef enum {
   GIMV_INDEX_DATE_MTIME    = 0,
   GIMV_INDEX_DATE_FILENAME = 1,
   GIMV_INDEX_DATE_EXIF     = 2,
   /* the indexer died reading it: the date is from mtime; read again once
    * at the next start (it may have died for another reason), then left */
   GIMV_INDEX_DATE_SKIPPED      = 3,
   GIMV_INDEX_DATE_SKIPPED_TWICE = 4,
} GimvIndexDateSource;

typedef enum {
   GIMV_INDEX_DB_OK,
   GIMV_INDEX_DB_MISSING,     /* read-only open of a database not made yet */
   GIMV_INDEX_DB_ERROR,
} GimvIndexDbResult;

gchar            *gimv_index_db_default_path (void);

/* writable: create the file and schema if needed, and when the file is
 * broken or has another schema version, move it aside as FILE.old and start
 * a new one.  read-only: just open, fail on a version mismatch. */
GimvIndexDbResult gimv_index_db_open         (const gchar *path,
                                              gboolean     writable,
                                              sqlite3    **db,
                                              gchar      **errmsg);

/* wait for the other writers up to "seconds", looking every few
 * milliseconds: they commit often but take the lock again at once, and
 * SQLite's own waiting (sleeps growing to 100 ms) mostly misses the gaps */
void              gimv_index_db_wait_busy    (sqlite3     *db,
                                              gint         seconds);

#endif /* __GIMV_INDEX_DB_H__ */
