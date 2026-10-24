/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the notice icon of the thumbnail window.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_NOTICE_H__
#define __GIMV_INDEX_NOTICE_H__

#include <gtk/gtk.h>

/* the icon for the status bar, shown only while there are notices */
GtkWidget *gimv_index_notice_button_new  (void);

/* an image (or a folder) with comment files was moved by another program */
void       gimv_index_notice_moved       (const gchar *old,
                                          const gchar *new,
                                          gboolean     dir);
/* images that came while gimv was not running */
void       gimv_index_notice_added_since_last (guint n);
/* the indexer cannot watch more folders */
void       gimv_index_notice_watch_limit (guint        n_watched);

#endif /* __GIMV_INDEX_NOTICE_H__ */
