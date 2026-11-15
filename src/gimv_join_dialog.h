/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: joining the selected images into a new one.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_JOIN_DIALOG_H__
#define __GIMV_JOIN_DIALOG_H__

#include <gtk/gtk.h>

/* paths: gchar * of the images (copied), two or more */
void gimv_join_dialog_open (GList *paths, GtkWindow *parent);

#endif /* __GIMV_JOIN_DIALOG_H__ */
