/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: the window of the missing images.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_MISSING_H__
#define __GIMV_INDEX_MISSING_H__

#include <gtk/gtk.h>

/* log_path: the log to show first, NULL for the newest */
void gimv_index_missing_open (GtkWindow *parent, const gchar *log_path);

#endif /* __GIMV_INDEX_MISSING_H__ */
