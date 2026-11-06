/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: bookmarks of images (~/.gimv/bookmarks).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_BOOKMARK_H__
#define __GIMV_BOOKMARK_H__

#include <glib.h>

/*
 * A bookmark is an image path, the time it was added and a note (may be
 * NULL).  Any image can have one, in the index or not.  They are kept in
 * a text file of their own, written at each change:
 *
 *   # GImageView bookmarks
 *   <added (seconds since the epoch)> TAB <path> TAB <note>
 *
 * with "\\", TAB, CR and LF in the path and the note written as \\, \t,
 * \r and \n.  The file is read again when another gimv changed it.
 */
typedef struct {
   gchar  *path;
   gint64  added;
   gchar  *note;    /* NULL: none */
} GimvBookmark;

/* the file (NULL: the default, ~/.gimv/bookmarks; for the tests) */
void          gimv_bookmark_set_file     (const gchar  *filename);

gboolean      gimv_bookmark_has          (const gchar  *path);
/* a copy (gimv_bookmark_free ()), NULL when the path has none */
GimvBookmark *gimv_bookmark_get          (const gchar  *path);
/* adds the path (now) when it has none; note NULL or "": no note.
 * Returns TRUE when it was added. */
gboolean      gimv_bookmark_add          (const gchar  *path);
void          gimv_bookmark_set_note     (const gchar  *path,
                                          const gchar  *note);
void          gimv_bookmark_remove       (const gchar  *path);
/* a file renamed or moved (gimv, or seen by the indexer): the bookmark of
 * old goes to new; a folder: the bookmarks below it too */
void          gimv_bookmark_move         (const gchar  *old_path,
                                          const gchar  *new_path);
/* all of them, newest first: a GPtrArray of GimvBookmark (copies) */
GPtrArray    *gimv_bookmark_list         (void);
guint         gimv_bookmark_count        (void);

GimvBookmark *gimv_bookmark_copy         (const GimvBookmark *bm);
void          gimv_bookmark_free         (GimvBookmark *bm);

/* called after every change (from the main loop of the caller) */
typedef void (*GimvBookmarkChangedFunc) (gpointer data);
guint         gimv_bookmark_add_watch    (GimvBookmarkChangedFunc func,
                                          gpointer                data);
void          gimv_bookmark_remove_watch (guint                   id);

/* a line for the tooltip of a bookmarked image ("★ the note", "★
 * Bookmarked"), NULL when it has none */
gchar        *gimv_bookmark_tooltip      (const gchar  *path);

/* the escaping of the file (for the tests) */
gchar        *gimv_bookmark_escape       (const gchar  *text);
gchar        *gimv_bookmark_unescape     (const gchar  *text);

#endif /* __GIMV_BOOKMARK_H__ */
