/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: editing the tags of images.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_TAGS_H__
#define __GIMV_INDEX_TAGS_H__

#include <gtk/gtk.h>

/* the dialog to edit the tags of the images (a list of GimvImageInfo) */
void gimv_index_edit_tags (GtkWindow *parent, GList *infos);

#endif /* __GIMV_INDEX_TAGS_H__ */
