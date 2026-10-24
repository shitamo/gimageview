/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: runs gimv-indexer and reads its database.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_H__
#define __GIMV_INDEX_H__

#include <glib.h>
#include <sqlite3.h>

typedef enum {
   GIMV_INDEX_STATE_OFF,       /* not configured */
   GIMV_INDEX_STATE_STARTING,
   GIMV_INDEX_STATE_SCANNING,
   GIMV_INDEX_STATE_IDLE,
   GIMV_INDEX_STATE_BUSY,      /* another gimv runs the indexer */
   GIMV_INDEX_STATE_ERROR,
} GimvIndexState;

/* enabled and at least one directory given */
gboolean        gimv_index_is_configured    (void);
/* start the indexer if it is enabled in the preferences */
void            gimv_index_init             (void);
void            gimv_index_quit             (void);
/* after the preferences have changed */
void            gimv_index_restart          (void);
void            gimv_index_rescan           (void);

GimvIndexState  gimv_index_get_state        (void);
const gchar    *gimv_index_get_status_text  (void);

/* read-only connection to the database, NULL when there is none yet.
 * Owned by gimv_index, don't close it. */
sqlite3        *gimv_index_get_db           (void);

/* the comment file of the image (its tags) was written: let the indexer
 * read it again */
void            gimv_index_tags_changed     (const gchar *path);

/* something new is in the database */
typedef enum {
   GIMV_INDEX_CHANGED_DATES = 1 << 0,
   GIMV_INDEX_CHANGED_TAGS  = 1 << 1,
} GimvIndexChange;

typedef void (*GimvIndexChangedFunc) (GimvIndexChange what, gpointer data);
guint           gimv_index_add_watch        (GimvIndexChangedFunc func,
                                             gpointer             data);
void            gimv_index_remove_watch     (guint                id);

#endif /* __GIMV_INDEX_H__ */
