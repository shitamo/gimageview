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

#ifndef __GIMV_RENAME_FILES_H__
#define __GIMV_RENAME_FILES_H__

#include "gimv_thumb_view.h"

typedef enum {
   GIMV_RENAME_OK,
   GIMV_RENAME_SAME,     /* no change */
   GIMV_RENAME_BAD,      /* not a name (NULL) */
   GIMV_RENAME_EXISTS,   /* a file there has it (not renamed, or waited for one that stays) */
   GIMV_RENAME_TWICE,    /* another file gets it */
   GIMV_RENAME_ROUND     /* the names go round (a swap) */
} GimvRenameState;

/* i renamed (error 0) or not (errno) */
typedef void (*GimvRenameDoneFunc) (guint i, gint error, gpointer data);

/* what keeps each of n files (paths) from being renamed to new_paths
   (NULL: not a name): a name of a file there is fine when that file is
   renamed too and goes first */
void  gimv_rename_plan_check (const gchar * const *paths,
                              const gchar * const *new_paths,
                              guint                n,
                              GimvRenameState     *states);
/* from[i] -> to[i] for those wanted, never over a file: one whose name is
   taken waits for the file there to go; done[i] tells; the number renamed */
guint gimv_rename_plan_run   (const gchar * const *from,
                              const gchar * const *to,
                              guint                n,
                              const gboolean      *wanted,
                              gboolean            *done,
                              GimvRenameDoneFunc   func,
                              gpointer             data);

/* the dialog to rename several files (thumbs: GimvThumb, the selection) */
void gimv_rename_files_dialog (GimvThumbView *tv, GList *thumbs);

#endif /* __GIMV_RENAME_FILES_H__ */
