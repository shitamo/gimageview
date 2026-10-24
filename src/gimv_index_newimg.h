/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView image index: desktop notifications of new images.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_INDEX_NEWIMG_H__
#define __GIMV_INDEX_NEWIMG_H__

#include <glib.h>

/* an image came into the indexed folders while gimv runs */
void gimv_index_newimg_add  (const gchar *path);
void gimv_index_newimg_quit (void);

#endif /* __GIMV_INDEX_NEWIMG_H__ */
