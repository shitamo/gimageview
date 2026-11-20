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

/*
 * The cover of a book for its thumbnail: the first image of a folder, or
 * of an archive.  Run in the threads of the thumbnails (no GTK).
 *
 * The cover is the first image by name, as the viewer shows them.  An
 * archive whose entries can be listed without unpacking them (zip, LHA,
 * tar not compressed) is listed, then that image read.  In the others
 * (rar, 7z, compressed tar: maybe solid) an image that comes before the
 * one kept by name is read as it is found; when GIMV_COVER_MAX_SECONDS or
 * GIMV_COVER_MAX_BYTES unpacked is passed, the best of those gone over is
 * the cover (in a big solid archive: of its first part).
 */

#ifndef __GIMV_COVER_H__
#define __GIMV_COVER_H__

#include <glib.h>

#define GIMV_COVER_MAX_SECONDS 3
#define GIMV_COVER_MAX_BYTES   (256 * 1024 * 1024)   /* unpacked, skipped */
#define GIMV_COVER_MAX_IMAGE   (64 * 1024 * 1024)    /* the image */

/* whether a name is of an image (by its extension) */
typedef gboolean (*GimvCoverIsImage) (const gchar *name);

/* the first image of the folder by name (numbers by value): its path;
   NULL: none.  g_free () it */
gchar    *gimv_cover_of_dir     (const gchar      *dir,
                                 GimvCoverIsImage  is_image);

/* the cover of the archive: its bytes and its name in the archive;
   FALSE: none, or not within the limits.  g_free () them */
gboolean  gimv_cover_of_archive (const gchar      *path,
                                 GimvCoverIsImage  is_image,
                                 guchar          **data,
                                 gsize            *len,
                                 gchar           **name);

/* an archive gimv can take a cover of (by its extension) */
gboolean  gimv_cover_archive_ext (const gchar     *path);

#endif /* __GIMV_COVER_H__ */
