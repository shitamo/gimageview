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
/* images with comment files are missing: the log of this run, n so far */
void       gimv_index_notice_missing     (const gchar *log,
                                          guint        n);
void       gimv_index_notice_logs_pruned (guint        n);
/* the moves (files) nobody answered, "old\tnew" strings to free */
GList     *gimv_index_notice_take_unanswered (void);
/* the indexer cannot watch more folders */
void       gimv_index_notice_watch_limit (guint        n_watched);
void       gimv_index_notice_skipped     (const gchar *path);
void       gimv_index_notice_indexer_died (const gchar *why,
                                           gboolean     restarted);

/* renaming a tag: some comment files could not be written */
void gimv_index_notice_rename_failed (const gchar *old, const gchar *new,
                                      guint n, const gchar *first);

#endif /* __GIMV_INDEX_NOTICE_H__ */
