/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the comments written in an image file (MAG, PNG, JPEG, GIF),
 * read from its header without loading the image.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_FILE_COMMENT_H__
#define __GIMV_FILE_COMMENT_H__

#include <glib.h>

typedef struct GimvFileComment_Tag {
   gchar *name;   /* shown (translated), e.g. "Comment", or a PNG keyword */
   gchar *text;   /* UTF-8, lines ended by "\n" */
} GimvFileComment;

/* the comments of the file, an array of GimvFileComment (freed with it);
 * empty when there is none or the format is not known */
GPtrArray *gimv_file_comment_read (const gchar *filename);

#endif /* __GIMV_FILE_COMMENT_H__ */
