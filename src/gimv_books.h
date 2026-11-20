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
 * Where a book was left: the page last shown of a folder or an archive
 * read with pages side by side ([Two Pages]), and its layout, to go on
 * from there when it is opened again.  Kept in ~/.gimv/books (a key file;
 * the most recent GIMV_BOOKS_MAX).  GLib only (for the tests).
 */

#ifndef __GIMV_BOOKS_H__
#define __GIMV_BOOKS_H__

#include <glib.h>

#define GIMV_BOOKS_MAX 1000

/* the file (tests; else ~/.gimv/books) */
void      gimv_books_set_file (const gchar *path);

/* book: the folder (no '/' at the end) or the archive; page: the file in
   the folder, or the member of the archive; layout: the name of the
   layout.  FALSE: not known.  g_free () what is given */
gboolean  gimv_books_get      (const gchar  *book,
                               gchar       **page,
                               gchar       **layout);
void      gimv_books_set      (const gchar  *book,
                               const gchar  *page,
                               const gchar  *layout);
void      gimv_books_forget   (const gchar  *book);

/* written now (else a little after a change, and at the end) */
void      gimv_books_save     (void);

/* the book and the page of a path: a folder and a file in it ("/a/b.jpg"
   -> "/a", "b.jpg"); g_free () them */
void      gimv_books_split    (const gchar  *path,
                               gchar       **book,
                               gchar       **page);

#endif /* __GIMV_BOOKS_H__ */
