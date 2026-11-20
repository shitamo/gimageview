/* -*- Mode: C; tab-width: 3; indent-tabs-mode: nil; c-basic-offset: 3 -*- */

/*
 * GImageView: how two alike pictures of the duplicate scan differ: the
 * same pixels, saved again (JPEG noise all over), the whole a little
 * brighter or of other colours, a part changed (a difference picture,
 * writing), or other pictures.  No GTK (the thread of the scan, the tests).
 *
 * The kinds and their limits come from a survey of 2.4 million pictures
 * (2026-11): of the pairs [Find Duplicates] put in "similar", a fifth
 * were the same picture, the rest other pictures or parts changed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __GIMV_DUPL_CLASSIFY_H__
#define __GIMV_DUPL_CLASSIFY_H__

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* the pictures are compared this small first */
#define GIMV_DUPL_SMALL_SIZE 256

typedef enum {
   GIMV_DUPL_REL_PIXELS,     /* the same pixels (other metadata, format) */
   GIMV_DUPL_REL_RESAVED,    /* the same picture saved again */
   GIMV_DUPL_REL_RESIZED,    /* the same picture, another size */
   GIMV_DUPL_REL_ADJUSTED,   /* the whole brighter, darker, other colours */
   GIMV_DUPL_REL_PARTLY,     /* a part changed */
   GIMV_DUPL_REL_DIFFERENT   /* another picture */
} GimvDuplRelation;

typedef struct {
   gdouble mean;        /* the mean difference, 0 .. 255 (RGB averaged) */
   gdouble block_max;   /* that of the most different block */
   gdouble share;       /* the share of the blocks over 8 */
   gdouble shift;       /* the largest mean shift of a colour (JPEG noise
                           has none) */
   gdouble residual;    /* the mean difference left when the shifts of the
                           colours are taken out (brighter is the same) */
} GimvDuplDiff;

/* a and b of the same size; block: the side of the blocks.  FALSE: not
   the same size */
gboolean          gimv_dupl_diff           (GdkPixbuf          *a,
                                            GdkPixbuf          *b,
                                            gint                block,
                                            GimvDuplDiff       *diff);

/* of the small pictures: GIMV_DUPL_REL_RESAVED for the same picture (the
   full ones tell more), _PARTLY or _DIFFERENT */
GimvDuplRelation  gimv_dupl_classify_small (const GimvDuplDiff *diff);
/* of the full pictures of the same size, alike when small: _PIXELS,
   _RESAVED, _ADJUSTED or _PARTLY (a small change seen only at full size);
   lossy: one of them is JPEG (its noise is larger in some blocks) */
GimvDuplRelation  gimv_dupl_classify_full  (const GimvDuplDiff *diff,
                                            gboolean            lossy);

/* the small pictures a and b (their own sizes) compared: _DIFFERENT when
   their shapes differ */
GimvDuplRelation  gimv_dupl_compare_small  (GdkPixbuf          *a,
                                            GdkPixbuf          *b);

/* a picture as shown (EXIF orientation), whole or at most size x size
   (size 0: whole); NULL when it cannot be read */
GdkPixbuf        *gimv_dupl_load           (const gchar        *path,
                                            gint                size);

/* the quality (1 .. 100) of the libjpeg tables the luminance table of a
   JPEG file is nearest to; -1: not a JPEG, or no table found */
gint              gimv_dupl_jpeg_quality   (const gchar        *path);
/* the same of the bytes of the start of a file */
gint              gimv_dupl_jpeg_quality_of (const guchar      *buf,
                                            gsize               len);

#endif /* __GIMV_DUPL_CLASSIFY_H__ */
