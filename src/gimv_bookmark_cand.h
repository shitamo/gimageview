/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: where a bookmarked file or folder that is not found may
 * have gone.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_BOOKMARK_CAND_H__
#define __GIMV_BOOKMARK_CAND_H__

#include <glib.h>
#include <sqlite3.h>

/* the files (or folders) of the same name as path in the index that are
 * there (not path itself): a GPtrArray of paths, sorted (free it with
 * g_ptr_array_unref ()).  db: the index (the files and dirs tables), NULL:
 * none; any thread, the one of the connection. */
GPtrArray *gimv_bookmark_missing_candidates (sqlite3 *db, const gchar *path);

#endif /* __GIMV_BOOKMARK_CAND_H__ */
