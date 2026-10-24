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
#define GIMV_INDEX_DB_VERSION      1
#define GIMV_INDEX_DB_VERSION_STR  "1"

/* files.date_src */
typedef enum {
   GIMV_INDEX_DATE_MTIME    = 0,
   GIMV_INDEX_DATE_FILENAME = 1,
   GIMV_INDEX_DATE_EXIF     = 2,
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

#endif /* __GIMV_INDEX_DB_H__ */
