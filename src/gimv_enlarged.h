/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: telling an enlarged copy (a small image only made larger)
 * from an image that is really larger.  No GTK: it runs in a thread and in
 * the tests.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_ENLARGED_H__
#define __GIMV_ENLARGED_H__

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* below it: large is small enlarged */
#define GIMV_ENLARGED_LIMIT       0.35
/* only from twice the size (less is not told well) */
#define GIMV_ENLARGED_MIN_FACTOR  1.9

/* How much of what large has beyond small is not explained by enlarging
 * small, against the same for small and small made smaller by as much:
 * about 1 for a picture really larger, low for an enlarged copy.
 *
 * Any ordinary enlarging (nearest, bilinear, bicubic, Lanczos, of any
 * program) makes each pixel a weighted sum of the pixels around its place
 * in the small image, with weights that depend only on where it falls
 * between them: the best such weights are fitted (least squares, for 16 x
 * 16 places) on half of the pixels and tried on the other half.  An
 * enlarged copy is then predicted down to its JPEG noise, a picture really
 * larger keeps the detail the small one does not have.  Natural pictures
 * are alike at all scales, so the detail small has beyond its own smaller
 * copy tells how much to expect.
 *
 * < 0: cannot tell (not GIMV_ENLARGED_MIN_FACTOR larger, too small, a
 * picture with almost no detail). */
gdouble  gimv_enlarged_score       (GdkPixbuf    *small,
                                    GdkPixbuf    *large);

/* the same for two files, read in their full size and turned by their
 * EXIF orientation */
gdouble  gimv_enlarged_score_files (const gchar  *small,
                                    const gchar  *large,
                                    GCancellable *cancel);

#endif /* __GIMV_ENLARGED_H__ */
