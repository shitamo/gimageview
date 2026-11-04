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
 * The EXIF orientations (1..8) as turnings of an image, used by the
 * thumbnails (gimv_thumb.c).  Internal to gimv: apart from gimv_thumb.c
 * only for the tests (tests/test_orient.c).
 */

#ifndef __GIMV_ORIENT_H__
#define __GIMV_ORIENT_H__

#include <gdk-pixbuf/gdk-pixbuf.h>

/* the turning a, then b */
gint       gimv_orient_compose     (gint        a,
                                    gint        b);
/* the turning that undoes o */
gint       gimv_orient_inverse     (gint        o);
/* the pixbuf turned by the orientation o (its options not looked at):
   a new reference, NULL when gdk-pixbuf failed */
GdkPixbuf *gimv_orient_pixbuf_turn (GdkPixbuf  *pixbuf,
                                    gint        o);
/* the turning of a thumbnail turned by its EXIF orientation "exif" for it
   to be shown as the user turned the image in the image window (turned
   over left and right when "flip", then turned by "rotate": 1 90 degrees
   counterclockwise, 2 180, 3 90 clockwise): 1 for none */
gint       gimv_orient_user_turn   (gint        exif,
                                    gint        rotate,
                                    gboolean    flip);

#endif /* __GIMV_ORIENT_H__ */
