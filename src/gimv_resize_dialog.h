/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: saving the selected images smaller, as new files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_RESIZE_DIALOG_H__
#define __GIMV_RESIZE_DIALOG_H__

#include <gtk/gtk.h>

/* paths: gchar * of the images (copied) */
void gimv_resize_dialog_open (GList *paths, GtkWindow *parent);

/* the comment (with the tags given by hand) of the image src for the new
 * image dest too (the main thread) */
void gimv_resize_copy_comment (const gchar *src, const gchar *dest);

#endif /* __GIMV_RESIZE_DIALOG_H__ */
