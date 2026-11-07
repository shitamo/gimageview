/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: the EXIF data of an image file of any format.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_EXIF_H__
#define __GIMV_EXIF_H__

#include "config.h"
#include <glib.h>

#ifdef ENABLE_EXIF

#include <libexif/exif-data.h>

/*
 * The EXIF data of the image at path, as exif_data_new_from_file () gives
 * it for a JPEG: read by libexif from a JPEG, else (built with gexiv2) the
 * EXIF data Exiv2 finds in the other formats it knows (TIFF and the raw
 * files, PNG, WebP, HEIF/AVIF, JPEG XL, ...).  NULL when there is none.
 */
ExifData *gimv_exif_data_new_from_file (const gchar *path);

/*
 * The EXIF orientation (1..8) the image at path is to be turned by when it
 * is shown: 1 when there is none, or when the decoder of its format turns
 * the image itself (HEIF/AVIF, JPEG XL: the orientation in their EXIF data
 * is what they did, not what is left to do).
 */
gint      gimv_exif_orientation        (const gchar *path);

#endif /* ENABLE_EXIF */

#endif /* __GIMV_EXIF_H__ */
