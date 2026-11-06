/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: bookmarking images (the popover with the note).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_BOOKMARK_EDIT_H__
#define __GIMV_BOOKMARK_EDIT_H__

#include <gtk/gtk.h>

/* bookmarks the images (paths: gchar *) at once, then shows a popover on
 * parent (pointing to rect, NULL: all of it) with the note, which is kept
 * however it closes, and a button to remove them again */
void   gimv_bookmark_edit            (GtkWidget          *parent,
                                      const GdkRectangle *rect,
                                      GList              *paths);
/* the paths of a list of GimvImageInfo that can be bookmarked (images, not
 * folders or the files in an archive); free with g_list_free_full (g_free) */
GList *gimv_bookmark_paths_of_infos  (GList              *infos);

#endif /* __GIMV_BOOKMARK_EDIT_H__ */
