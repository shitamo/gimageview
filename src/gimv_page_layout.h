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
 * The layouts of pages shown side by side ([Two Pages] of the image view):
 * how many columns and rows, from right to left or not, the first page
 * alone or not, under a name.  GLib only (for the tests).
 */

#ifndef __GIMV_PAGE_LAYOUT_H__
#define __GIMV_PAGE_LAYOUT_H__

#include <glib.h>

#define GIMV_PAGE_LAYOUT_MAX_COLS 4
#define GIMV_PAGE_LAYOUT_MAX_ROWS 3

typedef struct {
   gchar    *name;
   gint      cols;          /* 1 .. GIMV_PAGE_LAYOUT_MAX_COLS */
   gint      rows;          /* 1 .. GIMV_PAGE_LAYOUT_MAX_ROWS */
   gboolean  rtl;           /* the first page on the right (manga) */
   gboolean  first_alone;   /* the first page of the list (the cover) alone */
} GimvPageLayout;

GimvPageLayout *gimv_page_layout_new   (const gchar          *name,
                                        gint                  cols,
                                        gint                  rows,
                                        gboolean              rtl,
                                        gboolean              first_alone);
GimvPageLayout *gimv_page_layout_copy  (const GimvPageLayout *layout);
void            gimv_page_layout_free  (GimvPageLayout       *layout);

/* "cols,rows,flags,name;..." (flags: r right to left, a first alone) <->
   an array of GimvPageLayout (it frees them); entries that cannot be read
   are left out, ';' is taken out of the names */
GPtrArray      *gimv_page_layouts_parse     (const gchar *str);
gchar          *gimv_page_layouts_to_string (GPtrArray   *layouts);

/* where n pages go (width[i] by height each, in the order they are read):
   rows of cols pages, side by side, from the right when rtl; the rows
   under each other, as wide as the widest, a shorter one on the side the
   pages start from.  x[] and y[] (n each) get the places, *width and
   *height the size of the whole */
void            gimv_page_layout_place (gint         cols,
                                        gboolean     rtl,
                                        gint         n,
                                        const gint  *width,
                                        gint         height,
                                        gint        *x,
                                        gint        *y,
                                        gint        *total_width,
                                        gint        *total_height);

#endif /* __GIMV_PAGE_LAYOUT_H__ */
